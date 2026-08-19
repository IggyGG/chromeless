// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cloud-browser/capture/build-integration/cb_active_webcontents_resolver.h"

#include "base/check.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"

namespace cloud_browser {

CbActiveWebContentsResolver::CbActiveWebContentsResolver()
    : content::WebContentsObserver(/*web_contents=*/nullptr) {}

CbActiveWebContentsResolver::~CbActiveWebContentsResolver() = default;

void CbActiveWebContentsResolver::SetActiveCapture(content::WebContents* wc,
                                                   viz::FrameSinkId fsid) {
  // Idempotent fast path. Avoids re-Observe churn when the delegate
  // stops + restarts capture against the same tab (e.g. a Phase-2 UAT
  // that re-runs Cb.startFrameSinkCapture without an intervening
  // CreateNewTarget).
  if (active_ == wc && active_fsid_ == fsid) {
    return;
  }

  if (wc && fsid.is_valid()) {
    // Informational FSID mismatch check (TODO M4-R2-fsid-revalidation
    // in the header). The FSVC was told to capture |fsid|; the WC's
    // current primary-main-frame RWH publishes its own FSID. They
    // SHOULD match at capture-start time; if they don't, the FSVC is
    // already capturing a sink that doesn't belong to this WC's
    // current top-level RWH — likely a navigation snuck in between
    // capture-start and this call. Log and proceed: input dispatch
    // walks the WC's live RWH chain anyway, so it lands on the right
    // widget regardless of the FSVC's view of the world.
    if (auto* rwhv = wc->GetRenderWidgetHostView()) {
      if (auto* rwh = rwhv->GetRenderWidgetHost()) {
        const auto current_fsid = rwh->GetFrameSinkId();
        if (current_fsid.is_valid() && current_fsid != fsid) {
          LOG(WARNING)
              << "CbActiveWebContentsResolver: capture FSID "
              << fsid.ToString()
              << " differs from WebContents primary main frame FSID "
              << current_fsid.ToString()
              << " — input dispatch will target the live FSID";
        }
      }
    }
  }

  active_ = wc;
  active_fsid_ = fsid;

  // Re-target the WebContentsObserver so destruction observation
  // follows the active WC. Observe(nullptr) detaches.
  Observe(wc);

  if (wc) {
    LOG(INFO) << "CbActiveWebContentsResolver: active capture set, fsid="
              << fsid.ToString();
  } else {
    LOG(INFO) << "CbActiveWebContentsResolver: active capture cleared";
  }
}

content::WebContents* CbActiveWebContentsResolver::GetActiveWebContents() {
  return active_.get();
}

content::RenderWidgetHost*
CbActiveWebContentsResolver::GetActiveMainFrameRenderWidgetHost() {
  if (!active_) {
    return nullptr;
  }
  // Walk on every call — cross-document navigation swaps the RWH on
  // the same WC, and we want callers to transparently see the new
  // RWH. Each link can be null in transient renderer-crash recovery.
  content::RenderWidgetHostView* rwhv = active_->GetRenderWidgetHostView();
  if (!rwhv) {
    return nullptr;
  }
  return rwhv->GetRenderWidgetHost();
}

content::RenderWidgetHost*
CbActiveWebContentsResolver::GetActiveFocusedRenderWidgetHost() {
  if (!active_) {
    return nullptr;
  }
  // Matches R5's inline ResolveFocusedRenderWidgetHost walk exactly.
  // No fallback to main frame — the caller (R5) decides what to do on
  // null. Returning a non-null main-frame RWH here would risk landing
  // an IME commit in the wrong document when an <iframe> child has
  // focus (the very thing R5 is designed to avoid).
  content::RenderFrameHost* focused = active_->GetFocusedFrame();
  if (!focused) {
    return nullptr;
  }
  content::RenderWidgetHostView* rwhv = focused->GetView();
  if (!rwhv) {
    return nullptr;
  }
  return rwhv->GetRenderWidgetHost();
}

void CbActiveWebContentsResolver::WebContentsDestroyed() {
  // The observed (active) WebContents is being destroyed — renderer
  // crash, tab close, or end-of-process teardown. Clear active_ +
  // active_fsid_ so any input dispatch arriving between now and the
  // next SetActiveCapture() null-checks and skips.
  //
  // We log at INFO rather than WARNING — capture-target destruction
  // is expected behaviour on tab close, not an error. M4 R10 (last-
  // pointer accessor) reads the cleared state via GetActiveWeb
  // Contents() returning null and stops painting the remote-cursor
  // overlay accordingly.
  LOG(INFO) << "CbActiveWebContentsResolver: active WebContents destroyed, "
               "clearing (fsid was "
            << active_fsid_.ToString() << ")";
  active_ = nullptr;
  active_fsid_ = viz::FrameSinkId();
  // WebContentsObserver auto-detaches in its own teardown path; no
  // explicit Observe(nullptr) needed here.
}

void CbActiveWebContentsResolver::RenderViewHostChanged(
    content::RenderViewHost* old_host,
    content::RenderViewHost* new_host) {
  // Cross-document navigation. The WC pointer is stable but the
  // RenderWidgetHost (and its FrameSinkId) was replaced. The FSID we
  // were told about at SetActiveCapture is now stale; invalidate it
  // so:
  //   * active_frame_sink_id() reports the truth (no FSID currently
  //     valid for diagnostics).
  //   * The next SetActiveCapture call doesn't fire a false-positive
  //     mismatch warning comparing the WC's fresh FSID against our
  //     pre-nav cached one.
  //
  // active_ stays set — input still belongs to this WC. The dispatcher
  // helpers walk WC → RWHV → RWH on every call, so they transparently
  // pick up the new RWH on the next event without us doing anything.
  //
  // Suppress unused-parameter warnings — we don't read the host
  // pointers here, but the signature is fixed by WebContentsObserver.
  (void)old_host;
  (void)new_host;

  // Whether a capture was live on this WC before the swap — read BEFORE
  // the invalidation below clears active_fsid_. We require both an active
  // capture target (active_) AND a currently-valid FSID: active_ alone
  // can be set with an already-invalidated FSID in the brief window
  // between a prior swap and its posted re-arm, and re-arming again from
  // there is harmless but redundant (the pending re-arm already live-walks
  // to the newest sink). Gating on both keeps one swap → one re-arm.
  const bool had_active_capture = active_ != nullptr && active_fsid_.is_valid();

  if (active_fsid_.is_valid()) {
    LOG(INFO) << "CbActiveWebContentsResolver: RenderViewHostChanged, "
                 "invalidating stale capture FSID "
              << active_fsid_.ToString();
    active_fsid_ = viz::FrameSinkId();
  }

  // CV2-CAPTURE-REARM: the cross-document nav that swapped the RWH also
  // gave the WebContents a fresh FrameSinkId that the capturer is NOT
  // pointed at — it is still bound to the now-dead pre-nav sink, so it
  // receives no frames (renderer-starved, VERDICT=RENDERER-STARVED, no
  // video). The capturer cannot observe this (content-agnostic by
  // design); we can, so ask the owner to re-resolve the new FSID and
  // re-arm. POST rather than call inline: at RenderViewHostChanged the
  // new RWH is swapping in and its RenderWidgetHostView / FrameSinkId
  // may not be the WebContents' live primary yet. A same-turn PostTask
  // on the UI thread runs after the swap settles, so the owner's
  // WC→RWHV→RWH→GetFrameSinkId walk resolves the correct post-nav sink.
  if (had_active_capture && recapture_on_rvh_swap_) {
    LOG(INFO) << "CbActiveWebContentsResolver: RenderViewHostChanged on the "
                 "captured WebContents — scheduling capture re-arm onto the "
                 "new RenderWidgetHost's FrameSinkId";
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, recapture_on_rvh_swap_);
  }
}

void CbActiveWebContentsResolver::PrimaryMainFrameRenderProcessGone(
    base::TerminationStatus status) {
  // The renderer for the captured WebContents died. The WebContents
  // itself survives — it is what a reload has to be issued against — so
  // active_ deliberately stays set.
  //
  // The FSID is dead with the process, though. Invalidate it for the same
  // reason RenderViewHostChanged does: so active_frame_sink_id() reports
  // the truth and the next SetActiveCapture doesn't warn about a mismatch
  // against a sink that belongs to a process that no longer exists.
  const bool had_active_capture = active_ != nullptr;

  if (active_fsid_.is_valid()) {
    active_fsid_ = viz::FrameSinkId();
  }

  LOG(ERROR) << "CbActiveWebContentsResolver: captured renderer process GONE "
                "(TerminationStatus=" << static_cast<int>(status)
             << ", had_active_capture=" << had_active_capture
             << ") — capture is bound to a dead FrameSink and will produce "
                "no further frames until the page is reloaded";

  // Nothing here creates a replacement RenderViewHost: content does not
  // make one until a navigation happens. That is exactly why the RVH-swap
  // re-arm cannot cover this case, and why recovery has to start from the
  // crash. The owner holds the policy (bounded reload → escalate).
  //
  // POST rather than call inline, mirroring the re-arm path above: we are
  // inside a content observer callback and the owner's recovery issues a
  // navigation, which is not something to start from underneath the
  // notification that the renderer just died.
  if (had_active_capture && renderer_gone_) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                                                             renderer_gone_);
  }
}

}  // namespace cloud_browser

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbActiveWebContentsResolver — M4 R2 (CV2-42) concrete implementation
// of the WebContentsResolver interface coined by M4 R3
// (cb_input_dispatch_mouse.h:104-108). Resolves the single WebContents
// that the active cb_devtools_agent is currently capturing from, so the
// M4 input dispatchers (R3 mouse, R4 keyboard, R5 IME, R6 touch, R7
// drag) can target their events at the streamed tab without each
// having to walk DevToolsAgentHost / FrameSinkId on every event.
//
// Why this exists (per CV2-42 Finding 3 — "cross-context routing is
// REAL, not sidestepped"):
//
//   * Two sources of WebContents coexist in the browser process:
//       - `initial_web_contents_` — the boot tab created by
//         cloud_browser_browser_main_parts.cc.
//       - `web_contents_holders_` (vector) — tabs created later via
//         Target.createTarget through CbDevToolsManagerDelegate.
//     The "active" WebContents is the one *currently being captured*
//     by the FrameSinkVideoCapturer, which may be either — selection
//     happens in the capture-startup path (M2). The resolver must NOT
//     guess: GetFocusedWindow / GetLastActive are NOT reliable proxies
//     (a background CDP target can be the captured one while focus
//     sits on the boot tab). The capture-start path is the single
//     authority that calls SetActiveCapture() here.
//
//   * Background-tab case: when the captured WebContents is a
//     background tab (not the currently-focused one), the resolver
//     STILL returns it. Input must route to the streamed pixels
//     regardless of OS-level focus. This is what stops a freshly-
//     created CDP target from stealing routing.
//
//   * Cross-document navigation swaps the RenderWidgetHost on the same
//     WebContents (RenderViewHostChanged); same-document navigation
//     keeps it. The resolver's RWH helpers (Get*RenderWidgetHost) walk
//     WC → RWHV → RWH on every call rather than caching, so callers
//     transparently see the live RWH after either kind of nav. No
//     UAF — the prior RWH is destroyed by chromium before the new one
//     is wired up, and dispatchers null-check between events.
//
//   * WebContents destruction (renderer crash, tab close) clears
//     active_ via WebContentsObserver before any subsequent input
//     dispatch can deref it. Dispatchers null-check GetActiveWeb
//     Contents() on every event and skip on null.
//
// Lifecycle:
//   * Owned by cloud_browser_browser_main_parts (composition root).
//   * Pointer threaded through CbDevToolsManagerDelegate so the
//     Cb.startFrameSinkCapture handler can call SetActiveCapture()
//     immediately after a successful FrameSinkVideoCapturer Start().
//   * Pointer threaded through M4's CbInputDispatch as the resolver
//     argument for R3..R7's constructors.
//
// Threading:
//   * All public methods run on BrowserThread::UI.
//   * SetActiveCapture() is invoked from the DevTools CDP dispatch
//     path (UI thread); GetActive*() is invoked from the input
//     dispatchers (UI thread, after M4 R1's signaling-thread → UI
//     PostTask hop). WebContentsObserver callbacks fire on the UI
//     thread.
//   * No internal locking — the resolver is single-threaded.
//
// Non-goals (per CV2-42):
//   * Event construction. That is M4 R3..R7 territory.
//   * Capture-selection logic — which WebContents *should* be captured
//     is M2's call; the resolver is told the answer.

#ifndef CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_ACTIVE_WEBCONTENTS_RESOLVER_H_
#define CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_ACTIVE_WEBCONTENTS_RESOLVER_H_

#include "base/memory/raw_ptr.h"
#include "components/viz/common/surfaces/frame_sink_id.h"
#include "content/public/browser/web_contents_observer.h"

namespace content {
class RenderWidgetHost;
class WebContents;
}  // namespace content

namespace cloud_browser {

// Abstract interface — must match the inline forward-declaration in
// cb_input_dispatch_mouse.h:104-108 EXACTLY so the input dispatchers
// can compile against the inline decl pre-merge and link against the
// concrete type below post-merge (ODR contract). The forward-decl
// declares a single pure-virtual `GetActiveWebContents()`; this header
// matches that. Adding methods here without also adding them to the
// forward-decl is an ODR violation — until R3 / R4 / R5 / R6 / R7
// migrate their `#include` to point at this header, keep the abstract
// surface stable.
//
// Migration: when R3 / R4 / R5 / R6 / R7 merge, each drops its inline
// `class WebContentsResolver { ... };` forward-decl (see the
// TODO(M4-R3-r2-interface) / TODO(M4-R5-r2-interface) markers in those
// files) and `#include` this header instead. After the migration the
// ODR concern is gone and additional virtuals can be added freely.
class WebContentsResolver {
 public:
  virtual ~WebContentsResolver() = default;

  // Returns the WebContents the active cb_devtools_agent is currently
  // capturing from, or nullptr if no capture is active OR the
  // previously-active WebContents has been destroyed. Callers MUST
  // null-check on every invocation.
  //
  // The returned pointer is valid for the current UI-thread turn only;
  // callers must not retain it across PostTask boundaries.
  virtual content::WebContents* GetActiveWebContents() = 0;
};

// Concrete resolver. One instance per cb-chromium browser process,
// owned by cloud_browser_browser_main_parts. Passed by pointer to:
//   * CbDevToolsManagerDelegate — calls SetActiveCapture() on each
//     successful Cb.startFrameSinkCapture.
//   * Each M4 input dispatcher (R3..R7) — calls GetActive*() on each
//     dispatch. The dispatchers store it as `WebContentsResolver* const
//     resolver_` (raw, non-owning); the resolver MUST outlive every
//     dispatcher.
//
// composition_root_lifetime > resolver_lifetime > dispatcher_lifetime
// > delegate_lifetime — main_parts constructs in that order and tears
// down in reverse, so the raw pointers are always live during dispatch.
class CbActiveWebContentsResolver : public WebContentsResolver,
                                    public content::WebContentsObserver {
 public:
  CbActiveWebContentsResolver();

  CbActiveWebContentsResolver(const CbActiveWebContentsResolver&) = delete;
  CbActiveWebContentsResolver& operator=(
      const CbActiveWebContentsResolver&) = delete;

  ~CbActiveWebContentsResolver() override;

  // Notify the resolver that a Cb.startFrameSinkCapture just succeeded
  // against |wc| targeting FrameSinkId |fsid|. Replaces any prior
  // active capture — the previous WebContents is no longer "the
  // active streamed one" even if it's still alive in the embedder's
  // web_contents_holders_ / initial_web_contents_.
  //
  // Re-targets the WebContentsObserver to |wc| so destruction
  // observation tracks the active one (not a stale prior). Pass
  // nullptr to clear (treated identically to a capture stop).
  //
  // |fsid|: stored for diagnostics + a soft mismatch warning between
  // the captured FSID and the WC's current primary-main-frame FSID.
  // A mismatch is logged but not acted on — input dispatch routes via
  // the WC's live RWH walk, which is the right semantic for input
  // even if the FSVC is briefly capturing a stale sink.
  //
  // TODO(M4-R2-fsid-revalidation): once M7 verification confirms the
  // FSVC's behaviour on RWH swap (cross-doc nav), decide whether the
  // resolver should react to the mismatch (e.g. by waiting for the
  // next capture-start to re-register, or by emitting a metric).
  // Current behaviour is "log + keep going" so a transient nav
  // doesn't break input dispatch.
  void SetActiveCapture(content::WebContents* wc, viz::FrameSinkId fsid);

  // Convenience for callers that hold a CbActiveWebContentsResolver*
  // directly (rather than the abstract WebContentsResolver*). Walks
  // WC → GetRenderWidgetHostView → GetRenderWidgetHost on every call —
  // intentionally NOT cached, so cross-document navigation (which
  // swaps the RWH on the same WC) transparently surfaces the new RWH
  // on the next dispatch.
  //
  // Returns null if no active capture, the RWHV is detached, or the
  // RWH was destroyed (renderer crash recovery window). Callers MUST
  // null-check.
  content::RenderWidgetHost* GetActiveMainFrameRenderWidgetHost();

  // Focused-frame variant for M4 R5 (IME). IME state is per-widget,
  // and a page with a focused <iframe> hosts its editable element
  // inside the iframe's RenderWidgetHost — committing to the main-
  // frame RWH would land the text in the wrong document.
  //
  // Mirrors R5's inline ResolveFocusedRenderWidgetHost walk exactly:
  //   WebContents → GetFocusedFrame → GetView → GetRenderWidgetHost
  // Returns null if any link is missing. Falls through to nothing —
  // callers (R5) decide whether to defer (e.g. log + drop the IME
  // event) or fall back to the main frame.
  //
  // Lifetime contract: the returned RWH is call-scoped only.
  // chromium's GetFocusedFrame() can return a freshly-detached frame
  // mid-navigation; the pointer is valid for the current call but may
  // dangle on the next.
  //
  // TODO(M4-R2-focused-frame-stale): once R5 lands and we have a real
  // end-to-end IME test, decide whether this method should re-validate
  // the focused frame is still attached before returning, or whether
  // R5's per-event null-check is sufficient (it should be — chromium's
  // ImeText* calls also null-check, so a stale-but-not-yet-destroyed
  // frame at worst produces a dropped IME event, not a crash).
  content::RenderWidgetHost* GetActiveFocusedRenderWidgetHost();

  // Current FrameSinkId. Returns viz::FrameSinkId() (invalid) if no
  // capture is active. Read by tests + M7 verification; production
  // dispatch doesn't consume it.
  viz::FrameSinkId active_frame_sink_id() const { return active_fsid_; }

  // Test seam — flush the active state without going through the CDP
  // path. Used by cb_active_webcontents_resolver_test.cc to verify the
  // null-after-clear contract independent of the delegate wiring.
  void ClearForTesting() { SetActiveCapture(nullptr, viz::FrameSinkId()); }

  // WebContentsResolver:
  content::WebContents* GetActiveWebContents() override;

  // content::WebContentsObserver:
  //
  // Fires when the observed (active) WebContents is destroyed.
  // Clears active_ + active_fsid_ so subsequent GetActive*() return
  // null. Without this, an input dispatch arriving between the WC's
  // destruction and the next SetActiveCapture() would deref a
  // dangling pointer.
  //
  // WebContentsObserver detaches itself in its destructor / on
  // WebContentsDestroyed(), so no explicit Observe(nullptr) is needed
  // here.
  void WebContentsDestroyed() override;

  // content::WebContentsObserver: optional RWH-swap hook.
  //
  // Cross-document navigation destroys the old RenderViewHost and
  // creates a new one (with a new RenderWidgetHost and a new
  // FrameSinkId). The WebContents pointer is stable across the swap,
  // but the FSID we were told about at SetActiveCapture is now stale.
  // We do NOT clear active_ — input still belongs to this WC — but we
  // do invalidate active_fsid_ so active_frame_sink_id() reflects
  // reality (and the diagnostic log on the next SetActiveCapture
  // doesn't fire a false-positive mismatch).
  //
  // TODO(M4-R2-rvh-changed-surface): verify chromium fires
  // RenderViewHostChanged for the cases we care about (BFCache
  // restore, COOP/COEP swap, regular cross-origin nav). The headless
  // Aura paths may behave differently from full chromium; re-check
  // against M7 once a cross-doc-nav e2e test exists.
  void RenderViewHostChanged(content::RenderViewHost* old_host,
                             content::RenderViewHost* new_host) override;

 private:
  // Active WebContents the FSVC is capturing from. raw_ptr because
  // lifetime is managed elsewhere (CbDevToolsManagerDelegate's
  // web_contents_holders_ for CDP-created tabs, or main_parts'
  // initial_web_contents_ for the boot tab). Destruction is observed
  // via WebContentsObserver and clears this pointer before any deref
  // can happen.
  raw_ptr<content::WebContents> active_ = nullptr;

  // FrameSinkId the capturer was told to target at SetActiveCapture
  // time. Invalidated by RenderViewHostChanged (the FSID after a
  // cross-doc nav is a fresh one we weren't told about). Stored for
  // diagnostics + the soft mismatch warning; production dispatch
  // routes via the WC's live RWH walk, not this FSID.
  viz::FrameSinkId active_fsid_;
};

}  // namespace cloud_browser

#endif  // CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_ACTIVE_WEBCONTENTS_RESOLVER_H_

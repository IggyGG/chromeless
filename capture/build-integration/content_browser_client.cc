// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserContentBrowserClient — see content_browser_client.h.

#include "capture/build-integration/content_browser_client.h"

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "capture/build-integration/cb_devtools_agent.h"
#include "capture/build-integration/cloud_browser_browser_main_parts.h"
#include "capture/framesink-capturer/cb_framesink_video_track_source.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_main_parts.h"
#include "content/public/browser/devtools_manager_delegate.h"

namespace cloud_browser {

CloudBrowserContentBrowserClient::CloudBrowserContentBrowserClient() = default;

CloudBrowserContentBrowserClient::~CloudBrowserContentBrowserClient() = default;

std::unique_ptr<content::BrowserMainParts>
CloudBrowserContentBrowserClient::CreateBrowserMainParts(
    bool /*is_integration_test*/) {
  // ContentMain calls this exactly once on the browser process and
  // owns the returned unique_ptr for the lifetime of the run loop.
  // Returning nullptr (the base default) means chromium runs the
  // browser process to its message-loop without ever creating a
  // BrowserContext, which leaves DevToolsAgentHost with no targets to
  // publish — see cloud_browser_browser_main_parts.h for the reasoning.
  auto parts = std::make_unique<CloudBrowserBrowserMainParts>();
  // Stash a raw pointer for CreateDevToolsManagerDelegate so it can
  // read the default BrowserContext when the delegate is constructed
  // (BUGS-529 — wires the default context that previously had to come
  // through Target.createBrowserContext as a workaround).
  main_parts_ = parts.get();
  return parts;
}

std::unique_ptr<content::DevToolsManagerDelegate>
CloudBrowserContentBrowserClient::CreateDevToolsManagerDelegate() {
  // Per content_browser_client.h:1614 the base implementation returns
  // nullptr (chromium then runs without an embedder delegate, so
  // embedder-defined CDP methods are unreachable). We return our
  // delegate so Cb.startFrameSinkCapture is dispatchable from the
  // remote-debugging endpoint enabled by --remote-debugging-port on
  // launch-chromium-phase2.sh.
  //
  // Pass main_parts_'s BrowserContext as the default so
  // Target.createTarget without an explicit browserContextId succeeds
  // out of the box. main_parts_ is set by CreateBrowserMainParts, which
  // chromium calls before this hook (BrowserMainLoop::Init runs first;
  // CreateDevToolsManagerDelegate is lazy on first GetOrCreateFor in
  // PreMainMessageLoopRun). main_parts_->browser_context() returns the
  // context built in PreMainMessageLoopRun — by the time the FIRST
  // DevToolsAgentHost is created (also from PreMainMessageLoopRun, on
  // the initial about:blank target), the context is already populated.
  //
  // Also pass main_parts_'s Aura root window so each WebContents the
  // delegate creates inherits the embedder's focus chain. Both the
  // browser context AND the aura root are populated by
  // PreMainMessageLoopRun before the first GetOrCreateFor lazily
  // triggers this hook. See CbDevToolsManagerDelegate ctor doc + the
  // BUGS-529 chain in cloud_browser_browser_main_parts.cc.
  content::BrowserContext* default_context =
      main_parts_ ? main_parts_->browser_context() : nullptr;
  aura::Window* aura_context =
      main_parts_ ? main_parts_->aura_root_window() : nullptr;
  // ChromelessV2 M2 R4 (CV2-39); CV2-69 construction-order fix
  // (2026-05-18). Pass a LAZY getter, NOT a snapshot.
  //
  // This hook fires on the first DevToolsAgentHost::GetOrCreateFor —
  // PreMainMessageLoopRun *step 3* (cloud_browser_browser_main_parts
  // .cc:320) — which is BEFORE *step 5b* (:414) constructs
  // cb_track_source_. A snapshot taken here would therefore ALWAYS be
  // nullptr: that was the prior ServerError bug — every CV2-69
  // functional re-test logged "no video track source supplied" /
  // ServerError on Cb.startFrameSinkCapture even though the ctor arg
  // was wired (the value, not the wiring, was the problem). The
  // delegate Run()s this getter at Cb.startFrameSinkCapture DISPATCH
  // time, by which point PreMainMessageLoopRun has fully returned and
  // cb_track_source_ is populated. base::Unretained is safe here:
  // main_parts out-lives the delegate's useful window (the getter is
  // only Run() during an active CDP session, never at teardown) —
  // the same lifetime assumption default_context / aura_context
  // already rely on. Empty getter (main_parts_ null) → the delegate
  // emits a ServerError envelope rather than UAFing.
  base::RepeatingCallback<CloudBrowserFrameSinkVideoTrackSource*()>
      track_source_getter;
  base::RepeatingCallback<void(content::WebContents*, viz::FrameSinkId)>
      active_capture_callback;
  // CV2-WARM — bring up the native signaling session at Cb.startNativeSession
  // dispatch time (post warm-snapshot restore). Same Unretained(main_parts_)
  // lifetime contract as the two callbacks above.
  base::RepeatingCallback<webrtc::RTCError(const NativeSessionConfig&)>
      start_native_session_callback;
  // OSS-W0 — graceful process exit at Cb.shutdown dispatch time. Same
  // Unretained(main_parts_) lifetime contract as the callbacks above.
  base::RepeatingCallback<bool()> shutdown_callback;
  if (main_parts_) {
    track_source_getter = base::BindRepeating(
        &CloudBrowserBrowserMainParts::cb_track_source,
        base::Unretained(main_parts_));
    active_capture_callback = base::BindRepeating(
        &CloudBrowserBrowserMainParts::SetActiveCapture,
        base::Unretained(main_parts_));
    start_native_session_callback = base::BindRepeating(
        &CloudBrowserBrowserMainParts::StartNativeSession,
        base::Unretained(main_parts_));
    shutdown_callback =
        base::BindRepeating(&CloudBrowserBrowserMainParts::Shutdown,
                            base::Unretained(main_parts_));
  }
  return std::make_unique<CbDevToolsManagerDelegate>(
      default_context, aura_context, std::move(track_source_getter),
      std::move(active_capture_callback),
      std::move(start_native_session_callback), std::move(shutdown_callback));
}

}  // namespace cloud_browser

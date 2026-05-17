// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserBrowserMainParts — content::BrowserMainParts subclass
// that performs the bootstrap chromium needs in the browser process
// before any pages can load:
//
//   1. Construct the CloudBrowserBrowserContext (the profile).
//   2. Create an initial WebContents on about:blank so chromium has
//      something to register with DevToolsAgentHost — without at
//      least one WebContents the /json target list is empty and the
//      remote-debugging server has no targets to vend.
//   3. Start the DevTools HTTP handler on the port supplied by
//      --remote-debugging-port (or the loopback ephemeral port if
//      omitted).
//
// This is the missing piece reported in the EOD smoke test on
// triform-wtf: the worker forks the browser/zygote/GPU/network
// processes cleanly but never opens a TCP listener, because none of
// (1)/(2)/(3) was being executed. content::ContentMain spawns the
// browser process and runs through to the message loop, but if no
// embedder hooks BrowserMainParts to create a context+contents, the
// process just sits idle.
//
// Cross-references:
//   * content/public/browser/browser_main_parts.h
//   * headless/lib/browser/headless_browser_main_parts.{h,cc}
//     (canonical server-side embedder — uses its own per-WebContents
//     WindowTreeHost subclass; we use the real ozone-X11-backed one
//     instead because cb-chromium has Xvfb on :99)
//   * content/shell/browser/shell_browser_main_parts.{h,cc}
//     + content/shell/browser/shell_platform_data_aura.{h,cc}
//     (closer reference — same shape we now use for Aura init,
//     minus the ui/aura:test_support dep that's testonly = true and
//     unreachable from a non-test executable)
//   * content/shell/browser/shell_devtools_manager_delegate.cc
//     (DevTools HTTP handler bootstrap pattern)
//
// BUGS-529 history: 9703db5 added WebContents::WasShown + Focus on
// every WebContents we create (necessary), but Focus() is a silent
// no-op on Aura without a registered FocusClient + parenting chain.
// This file's PreMainMessageLoopRun now also constructs a
// CbAuraPlatformData and pins WebContents::CreateParams::context to
// the resulting root window so WebContentsViewAura::CreateAuraWindow's
// ParentWindowWithContext call resolves into our parenting client and
// the new view lands in Aura's focus chain. With both pieces in
// place, RenderWidgetHostViewAura::HasFocus() returns true and the
// renderer-side WidgetInputHandler accepts CDP Input.dispatch* events
// instead of dropping them as background-tab input.

#ifndef CAPTURE_BUILD_INTEGRATION_CLOUD_BROWSER_BROWSER_MAIN_PARTS_H_
#define CAPTURE_BUILD_INTEGRATION_CLOUD_BROWSER_BROWSER_MAIN_PARTS_H_

#include <memory>

#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "base/functional/callback.h"
#include "content/public/browser/browser_main_parts.h"
#include "rtc_base/thread.h"

namespace content {
class BrowserContext;
class WebContents;
}  // namespace content

namespace aura {
class Window;
}  // namespace aura

namespace display {
class ScreenBase;
}  // namespace display

namespace cloud_browser {

class CbAuraPlatformData;
class CloudBrowserBrowserContext;
class CloudBrowserFrameSinkVideoTrackSource;

class CloudBrowserBrowserMainParts : public content::BrowserMainParts {
 public:
  CloudBrowserBrowserMainParts();

  CloudBrowserBrowserMainParts(const CloudBrowserBrowserMainParts&) = delete;
  CloudBrowserBrowserMainParts& operator=(const CloudBrowserBrowserMainParts&) =
      delete;

  ~CloudBrowserBrowserMainParts() override;

  // content::BrowserMainParts:
  int PreEarlyInitialization() override;
  int PreMainMessageLoopRun() override;
  void WillRunMainMessageLoop(
      std::unique_ptr<base::RunLoop>& run_loop) override;
  void PostMainMessageLoopRun() override;

  // Public read-only accessor for the default BrowserContext. Returns
  // nullptr until PreMainMessageLoopRun has executed (the context is
  // constructed there). Used by CloudBrowserContentBrowserClient to
  // hand the default context to CbDevToolsManagerDelegate at delegate-
  // construction time so Target.createTarget without an explicit
  // browserContextId picks up a sensible default. See
  // ShellBrowserMainParts::browser_context() for the analogous upstream
  // accessor in content_shell. Defined out-of-line because the
  // CloudBrowserBrowserContext → content::BrowserContext upcast needs
  // the derived class definition.
  content::BrowserContext* browser_context() const;

  // Public read-only accessor for the Aura root window owned by our
  // CbAuraPlatformData. nullptr until PreMainMessageLoopRun has set
  // aura_ up. CloudBrowserContentBrowserClient::CreateDevToolsManager
  // Delegate forwards this to CbDevToolsManagerDelegate at delegate-
  // construction time so every Target.createTarget WebContents gets
  // its CreateParams::context populated and parented under the same
  // root we use for the boot WebContents (BUGS-529 second-layer
  // closeout). Defined out-of-line so the header doesn't need to pull
  // in cb_aura_platform_data.h or ui/aura/window_tree_host.h. See
  // ShellBrowserMainParts::browser_context() for the analogous
  // upstream pattern.
  aura::Window* aura_root_window() const;

  // Public read-only accessor for the browser-process video track
  // source (ChromelessV2 M2 R4 — CV2-39). nullptr until PreMain
  // MessageLoopRun has constructed it (peer-adjacent with pcf_).
  // CloudBrowserContentBrowserClient::CreateDevToolsManagerDelegate
  // forwards this raw pointer to CbDevToolsManagerDelegate at delegate-
  // construction time so Cb.startFrameSinkCapture can route the
  // resolved FrameSinkId + producer mojo into the track source rather
  // than owning a CloudBrowserFrameSinkCapturer in the delegate. Raw
  // pointer (not scoped_refptr) because the consumer never bumps the
  // refcount — main_parts holds the only strong reference for the
  // lifetime of the worker. Defined out-of-line so the header doesn't
  // need to pull in the track-source class definition.
  CloudBrowserFrameSinkVideoTrackSource* cb_track_source() const;

 private:
  // Reads --remote-debugging-port (default 0 = ephemeral, loopback)
  // and starts content::DevToolsAgentHost::StartRemoteDebuggingServer
  // bound at 127.0.0.1:<port>. Idempotent — only called once from
  // PreMainMessageLoopRun.
  void StartDevToolsHttpHandler();

  // Symmetric counterpart called from PostMainMessageLoopRun. Calls
  // StopRemoteDebuggingServer iff StartDevToolsHttpHandler ran.
  void StopDevToolsHttpHandler();

  // Owned global display::Screen instance. chromium fatals on
  // `Check failed: Screen::Get()` from ui/display/display_observer.cc:32
  // during browser-process init when something registers a
  // DisplayObserver against a null Screen — the worker hits this even
  // though it never paints to a real surface, because internal
  // subsystems (audio, prefetch, ...) attach observers as part of
  // their startup. Constructed in PreMainMessageLoopRun before the
  // BrowserContext + initial WebContents so the registration order is
  // safe.
  std::unique_ptr<display::ScreenBase> screen_;

  // Aura subsystem (root WindowTreeHost + focus / parenting / capture
  // / activation clients + fill layout). Constructed in PreMain
  // MessageLoopRun BEFORE the initial WebContents so the boot tab
  // can pass aura_->host()->window() as its CreateParams::context
  // and get parented into a real focus chain. This is the deeper
  // root cause of BUGS-529: 9703db5 called WebContents::Focus() but
  // it was a no-op without an Aura focus client + parenting client
  // registered on the WebContents view's root window.
  //
  // INTENTIONALLY LEAKED on PostMainMessageLoopRun (release()) — the
  // CbDevToolsManagerDelegate held by content's DevToolsManager
  // singleton owns WebContents children of aura_->host()->window(),
  // and that singleton is destroyed by AtExitManager AFTER main_parts
  // dies. Tearing aura_ down here would UAF those still-live children
  // when their dtors walk their parent pointer. Process is exiting
  // within seconds; OS reclaims memory and the X11 connection cleanly.
  std::unique_ptr<CbAuraPlatformData> aura_;

  std::unique_ptr<CloudBrowserBrowserContext> browser_context_;
  std::unique_ptr<content::WebContents> initial_web_contents_;

  // ChromelessV2 M1 — browser-process PeerConnectionFactory + the 3
  // dedicated rtc::Threads it runs on. The PCF replaces the renderer-
  // side libwebrtc PCF that the M0 streamer.js path constructed; in
  // ChromelessV2 the browser process owns the entire WebRTC peer, so
  // the PCF lives here.
  //
  // CV2-26 R-thread DECISION: own 3 bare rtc::Thread members
  // (network / worker / signaling) rather than ThreadWrappers around
  // chromium task runners. Simpler lifetime ordering for the mandated
  // PCF-teardown-before-browser_context_ ordering and matches
  // webrtc/examples/peerconnection. Caveat documented on the CV2-26
  // ticket: PCF methods MUST be marshaled onto the signaling_thread_;
  // M2/M3 observe this.
  //
  // Construction order in PreMainMessageLoopRun (after browser_context_):
  //   1. Start the 3 threads.
  //   2. Build the env via webrtc::CreateEnvironment().
  //   3. Construct pcf_ = CreateCloudBrowserPcf(net, worker, signaling,
  //                                              env, default ADM).
  //   4. Log the PCF video-sender codec caps via
  //      FormatPcfVideoCodecLogLine (CV2-27 M0-R5 probe target).
  //
  // Teardown order in PostMainMessageLoopRun:
  //   * pcf_.reset() FIRST — drops the strong ref the factory holds
  //     against the threads + the encoder factory + the ADM.
  //   * Then network_thread_/worker_thread_/signaling_thread_ are
  //     Stop()ped + reset() (in reverse-construction order, per
  //     webrtc convention).
  //   * THEN initial_web_contents_.reset() and browser_context_.reset()
  //     (these were already first in the pre-M1 order; they remain
  //     last because they hold raw pointers that the PCF doesn't,
  //     so dropping the PCF first is purely additive). Mirrors the
  //     aura_.release() ordering rationale at cc:303-328.
  std::unique_ptr<webrtc::Thread> network_thread_;
  std::unique_ptr<webrtc::Thread> worker_thread_;
  std::unique_ptr<webrtc::Thread> signaling_thread_;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pcf_;

  // ChromelessV2 M2 R4 (CV2-39) — peer-adjacent browser-process video
  // track source. Owns the CloudBrowserFrameSinkCapturer + R2 frame
  // conversion + libwebrtc broadcaster (R3 — CV2-38). The
  // CbDevToolsManagerDelegate Cb.startFrameSinkCapture handler reaches
  // into this via cb_track_source() to feed it a producer mojo +
  // resolved FrameSinkId; sinks (the libwebrtc peer track from M3)
  // attach via AddOrUpdateSink. Reset BEFORE pcf_ in PostMainMessage
  // LoopRun — the broadcaster may carry sink registrations the PCF's
  // peer tracks installed; tearing PCF first would leave dangling
  // weak refs in the broadcaster's sink list. Same ordering rationale
  // as the pcf_-before-threads comment block above.
  rtc::scoped_refptr<CloudBrowserFrameSinkVideoTrackSource> cb_track_source_;

  bool devtools_http_handler_started_ = false;

  // Captured in WillRunMainMessageLoop, run in PostMainMessageLoopRun
  // (or never, if the process is killed). Today we don't have an
  // in-process trigger to call this — chromium will exit when
  // SIGTERM is delivered to the worker — but stashing it keeps the
  // shape ready for a future Cb.shutdown CDP method.
  base::OnceClosure quit_main_message_loop_;
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CLOUD_BROWSER_BROWSER_MAIN_PARTS_H_

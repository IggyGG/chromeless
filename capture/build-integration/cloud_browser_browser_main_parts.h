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
// CV2-75 — M4/M6 consumer headers. main_parts owns the unique_ptrs
// that hold the runtime-wire consumer instances. CbCursorClient (M5
// R1) is NOT included here — it's owned by CbAuraPlatformData
// (cb_aura_platform_data.cc:152-153 already constructs it + calls
// aura::client::SetCursorClient on aura_'s ctor), so the M5 R1
// runtime-wire is satisfied at the aura platform layer without any
// main_parts plumbing.
#include "capture/build-integration/cb_clipboard_relay.h"
#include "capture/build-integration/cb_file_upload_relay.h"
#include "capture/build-integration/cb_input_dispatch.h"
#include "capture/signaling/cb_offerer_driver.h"
#include "capture/signaling/cb_signaling_ws_client.h"
#include "capture/signaling/cb_wire_envelope.h"
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

// CV2-69 (M55-R5-merge-with-m3-r4-r6) — inherits BOTH
// SignalingClientObserver and OffererDriverObserver. As the
// SignalingClientObserver, main_parts forwards inbound envelopes to
// offerer_driver_->OnEnvelope (resolving the SignalingWsClient ↔
// CbOffererDriver chicken-and-egg construction-order cycle — see
// PreMainMessageLoopRun for the wiring rationale). As the
// OffererDriverObserver, main_parts logs lifecycle transitions for
// telemetry (full M5.5 R5 chain integration deferred to a follow-up
// R# that resolves the R7 reconnect-vs-R4-pointer-staleness design
// question Q2 surfaced during CV2-69 pre-implementation).
class CloudBrowserBrowserMainParts
    : public content::BrowserMainParts,
      public cloud_browser::signaling::SignalingClientObserver,
      public cloud_browser::signaling::OffererDriverObserver {
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

  // signaling::SignalingClientObserver (CV2-69) — adapter forwarding to
  // offerer_driver_ so the embedder can serve as the WS client's
  // observer at ctor time without depending on a not-yet-constructed
  // driver. OnEnvelope + OnClosed(uint16_t,string_view) are pure-
  // virtual on the base. OnConnected + OnError have default no-op;
  // we override for diagnostic LOGs.
  void OnConnected() override;
  void OnEnvelope(const cloud_browser::signaling::Envelope& envelope) override;
  void OnClosed(uint16_t code, std::string_view reason) override;  // ws path
  void OnError(std::string_view reason) override;

  // signaling::OffererDriverObserver (CV2-69) — telemetry-only LOG
  // forwards. Production-grade lifecycle relay (M5.5 R5 audio chain,
  // M6 R1 stats relay) is deferred to a follow-up R#.
  //
  // Note: OffererDriverObserver::OnClosed(string_view) has a different
  // signature than SignalingClientObserver::OnClosed(uint16_t,string_view).
  // The two-arg form is the WS-client one (RFC 6455 close code +
  // reason); the one-arg form is the offerer driver's session-ended
  // event. Both are explicit overrides to avoid C++ name-hiding.
  void OnIceConnectionStateChanged(
      webrtc::PeerConnectionInterface::IceConnectionState state) override;
  void OnRenegotiationStarted(std::string_view trigger) override;
  void OnRenegotiationCompleted() override;
  void OnClosed(std::string_view reason) override;  // offerer-driver path
  void OnFailed(std::string_view reason) override;

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
  webrtc::scoped_refptr<CloudBrowserFrameSinkVideoTrackSource> cb_track_source_;

  // ============== CV2-69 (M55-R5-merge-with-m3-r4-r6) ==============
  //
  // Subset scope per implementer judgment call (CV2-69 §3.5 Option 1):
  // ship M3 R2 + M3 R4 + M2 R3 video transceiver wiring as the
  // signaling-only Phase A subset. M3 R7 reconnect + M5.5 R5 audio
  // lifecycle + M4/M5/M6 DataChannel handler binding deferred to a
  // follow-up R# (the latter group can be added by issuing the
  // construction calls + observer binds AFTER offerer_driver_->Start()
  // without touching the SignalingWsClient/CbOffererDriver wiring
  // landed here).
  //
  // R2 ws_client construction order (resolves the ctor-observer
  // chicken-and-egg cycle without requiring a SetObserver() method
  // on either class):
  //   1. Construct ws_client_ with `this` (main_parts) as
  //      SignalingClientObserver. main_parts::OnEnvelope forwards
  //      to offerer_driver_->OnEnvelope after offerer_driver_ is
  //      constructed. Pre-driver envelopes are LOGged and dropped
  //      (broker doesn't emit envelopes until the dial completes,
  //      and offerer_driver_ is constructed before ws_client_->
  //      Connect() fires the dial).
  //   2. Construct offerer_driver_ with ws_client_.get() raw pointer.
  //      The driver IS-A SignalingClientObserver too (it implements
  //      OnEnvelope as the SDP/ICE dispatcher), but we keep main_parts
  //      as the registered observer to preserve the adapter shape +
  //      provide a single place for diagnostic logging.
  //   3. offerer_driver_->Start() — creates the PeerConnection.
  //   4. ws_client_->Connect() — opens the dial.
  //   5. Construct video_track_ from cb_track_source_ + AddTransceiver
  //      to offerer_driver_->pc() — this is the mutation that triggers
  //      OnRenegotiationNeeded, which triggers CreateOffer, which
  //      writes the first `offer` envelope onto the wire.
  //
  // Teardown in PostMainMessageLoopRun LIFO (BEFORE existing pcf_
  // teardown):
  //   * video_track_.reset() (scoped_refptr; releases transceiver
  //     binding before the PC drops)
  //   * offerer_driver_->Close("session ended") (R6 emits bye envelope
  //     if connected)
  //   * offerer_driver_.reset() (drops PC; libwebrtc handles teardown)
  //   * ws_client_->Disconnect() (graceful close)
  //   * ws_client_.reset()
  std::unique_ptr<cloud_browser::signaling::SignalingWsClient> ws_client_;
  // CbOffererDriver is plain unique_ptr-owned by the embedder. It
  // inherits only the two NON-refcounted observer interfaces
  // (SignalingClientObserver + PeerConnectionObserver). The three
  // refcounted webrtc SDP-observer callbacks are delivered through
  // three transient refcounted adapter objects the driver constructs
  // internally at each CreateOffer / SetLocalDescription /
  // SetRemoteDescription call site (see cb_offerer_driver.cc, CV2-69
  // #176). An earlier iteration made the driver itself refcounted via
  // make_ref_counted to cure an abstract-class error, but that
  // surfaced a 3-way RefCountInterface diamond — the adapter
  // refactor is the libwebrtc-idiomatic fix and lets the driver stay
  // a plain unique_ptr-owned object.
  std::unique_ptr<cloud_browser::signaling::CbOffererDriver> offerer_driver_;
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_;

  // CV2-69 F7-skinny — answerer-facing DataChannels. Created by
  // PreMainMessageLoopRun step 8 via pc->CreateDataChannelOrError;
  // held as scoped_refptr to keep alive past the create call (the
  // PC also holds a strong ref internally). The DC HANDLER BINDING
  // (M4 R1 input dispatch / M5 R6 cursor / M6 R2 clipboard relay /
  // M6 R3 file-upload relay) is deferred to a follow-up R#.
  //
  // Labels are wire-contract — must match the chromeless/client
  // TypeScript answerer's hardcoded labels EXACTLY (Trap #1):
  //   input_dc_      → channel label "input"
  //   cursor_dc_     → channel label "cursor"
  //   clipboard_dc_  → channel label "clipboard"
  //   files_dc_      → channel label "files" (NOT "file-upload" —
  //                    the FILE name is file-upload.ts but the
  //                    CHANNEL is "files"; the trap was the
  //                    file-name-vs-channel-name conflation)
  webrtc::scoped_refptr<webrtc::DataChannelInterface> input_dc_;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> cursor_dc_;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> clipboard_dc_;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> files_dc_;
  // ============== END CV2-69 ==============

  // ============== CV2-75 RUNTIME-WIRE (Ring 2) ==============
  //
  // M4/M5/M6 consumer attachment — the gap that surfaced as the
  // architectural Phase-A failure: CV2-69 created the 4 DCs but never
  // bound observers, so inbound SCTP frames sat in libwebrtc's read
  // buffer with no handler (silent no-op for any functional test).
  // CV2-75 closes that gap by instantiating one consumer per DC +
  // calling RegisterObserver in the PreMainMessageLoopRun DC-creation
  // blocks.
  //
  // PATH A choice (vs Path B via CbDataChannelHost): cb_dc_host
  // hardcodes the wire label "file-upload" in its enum but the
  // portal answerer routes on "files" (Trap #1, hard-fixed by CV2-69
  // at the four direct CreateDataChannelOrError call sites above);
  // additionally cb_dc_host creates all 5 channels including "stats"
  // unconditionally, which would scope-creep the 5th-DC wire-contract
  // change beyond CV2-75. Both are deferred to follow-up tickets:
  //   * cv2/m6-r1-stats-dc — 5th DC + CbStatsRelay + portal-spec
  //   * cv2/m3-r5-dc-host-adoption — kFiles rename DONE (CV2-77 sub-
  //     fix 4); 5-DC vs parameterized CreateOutboundChannels decision
  //     remains + enables CbCursorDcEmitter M5 R6 (which already takes
  //     `signaling::CbDataChannelHost*`)
  //
  // The 3 consumers attached here:
  //   * CbInputDispatch        → input_dc_     (M4 R1, CV2-41)
  //   * CbClipboardRelay       → clipboard_dc_ (M6 R2, CV2-34)
  //   * CbFileUploadRelay      → files_dc_     (M6 R3, CV2-35)
  //
  // Not-here-because-already-wired-elsewhere:
  //   * CbCursorClient (M5 R1, CV2-19) — owned by CbAuraPlatformData
  //     (cb_aura_platform_data.cc:152-153 constructs + registers via
  //     aura::client::SetCursorClient in aura_'s ctor). The brief's
  //     "compiled but not instantiated" claim was off-by-one for this
  //     class — true at the main_parts layer but false at aura_'s
  //     layer. Re-instantiating at main_parts would double-register.
  //
  // Deferred to follow-up tickets:
  //   * CbStatsRelay (M6 R1, CV2-33) — needs a 5th "stats" DC not in
  //     the current F7-skinny set (see cv2/m6-r1-stats-dc).
  //   * CbCursorDcEmitter (M5 R6, CV2-24) — needs cb_dc_host adoption
  //     (see cv2/m3-r5-dc-host-adoption).
  //
  // Construction-arg notes (read the consumer headers for full
  // contracts):
  //   * CbInputDispatch wants a UI task runner + a delegate. We pass
  //     content::GetUIThreadTaskRunner({}) + the R1 production
  //     stand-in CbInputLoggingDelegate (cb_input_dispatch.h:194).
  //     R3+ replaces the logging delegate with a real injector.
  //   * CbClipboardBridgeWsClient + CbFileUploadBridgeWsClient take
  //     a `label`/`url` + io_task_runner. We pass url="off" which
  //     keeps both clients in `disabled()` mode (see
  //     cb_clipboard_relay.h:207 + cb_file_upload_relay.h:267) —
  //     the WS production backend has a TODO(M6-R2-ws-backend) and
  //     the v1 production-WS choice isn't locked yet. The relay
  //     OnMessage path still fires + logs; bridge POST is
  //     short-circuited. This satisfies the observer-binding
  //     architectural requirement without forcing a premature
  //     production-WS decision.
  //
  // Teardown ordering: explicit LIFO in PostMainMessageLoopRun
  // BEFORE the F7-skinny DC ref drops. UnregisterObserver each DC
  // first (must outlive the DC ref drop to avoid use-after-free in
  // libwebrtc's late callbacks); then drop the consumer state; then
  // the existing CV2-69 LIFO drops the DCs themselves.
  std::unique_ptr<CbInputLoggingDelegate> input_delegate_;
  std::unique_ptr<CbInputDispatch> input_dispatch_;
  std::unique_ptr<CbClipboardBridgeWsClient> clipboard_ws_;
  std::unique_ptr<CbClipboardRelay> clipboard_relay_;
  std::unique_ptr<CbFileUploadBridgeWsClient> file_upload_ws_;
  std::unique_ptr<CbFileUploadRelay> file_upload_relay_;
  // ============== END CV2-75 ==============

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

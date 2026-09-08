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

#include "api/audio/audio_device.h"       // CV2-WARM — adm_for_audio_lifecycle_
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"                 // CV2-WARM — StartNativeSession return
#include "api/scoped_refptr.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"           // CV2-WARM — raw_ptr<AudioDeviceModule>
#include "base/timer/timer.h"
#include "base/time/time.h"  // base::TimeTicks — renderer-crash window
#include "base/functional/callback_helpers.h"  // CV2 — base::ScopedClosureRunner (capture_keepalive_handle_); placed after base/timer to avoid an add/add textual collision with the CV2-WARM raw_ptr.h include (functionally order-independent)
#include "components/viz/common/surfaces/frame_sink_id.h"
// CV2-75 — M4/M6 consumer headers. main_parts owns the unique_ptrs
// that hold the runtime-wire consumer instances. CbCursorClient (M5
// R1) is NOT included here — it's owned by CbAuraPlatformData
// (cb_aura_platform_data.cc:152-153 already constructs it + calls
// aura::client::SetCursorClient on aura_'s ctor), so the M5 R1
// runtime-wire is satisfied at the aura platform layer without any
// main_parts plumbing.
#include "capture/build-integration/cb_active_webcontents_resolver.h"
#include "capture/build-integration/cb_clipboard_relay.h"
#include "capture/build-integration/cb_file_upload_relay.h"
#include "capture/build-integration/cb_input_dispatch.h"
#include "capture/build-integration/cb_input_dispatch_composite.h"
#include "capture/build-integration/cb_viewport_controller.h"  // CbViewportSpec (by value)
#include "capture/signaling/cb_ice_config.h"        // CV2-WARM — NativeSessionConfig::ice
#include "capture/signaling/cb_offerer_driver.h"
#include "capture/signaling/cb_signaling_reconnect.h"  // CV2-REDIAL — the R7 wrapper
#include "capture/signaling/cb_signaling_ws_client.h"
#include "capture/signaling/cb_wire_envelope.h"
#include "api/rtp_sender_interface.h"  // CV2-KEYFRAME — video_sender_
#include "content/public/browser/browser_main_parts.h"
#include "rtc_base/thread.h"

namespace content {
class BrowserContext;
class WebContents;
}  // namespace content

namespace aura {
class Window;
}  // namespace aura

namespace cloud_browser {

namespace audio {
class CbAudioLifecycle;
}  // namespace audio

namespace cursor {
class CbCursorDcEmitter;
class EmitPolicy;
class EnvelopeAssembler;
}  // namespace cursor

namespace signaling {
class CbDataChannelHost;
}  // namespace signaling

class CbAuraPlatformData;
class CbBeginFrameDriver;  // CV2-ICE — drives renderer frame production
class CbControlChannel;    // browser-fidelity wave 1 — ask-a-human channel
class CbHeadlessScreen;  // CV2-78 (M5 R1 cursor-routing gate)
class CbCursorXyJoin;
class CloudBrowserBrowserContext;
class CloudBrowserFrameSinkVideoTrackSource;

// CV2-WARM — per-session signaling config consumed by StartNativeSession().
// Bundles the two existing aggregates the boot path reads from env
// (WsClientConfig + IceConfig) so the env path and the runtime CDP path
// (Cb.startNativeSession) build the SAME struct and feed ONE bring-up code
// path. Constructed either from LoadConfigFromEnv()+LoadIceConfigFromEnv()
// at boot, or from Cb.startNativeSession CDP params after a warm-snapshot
// restore (see cv2-warm-snapshot-cold-start design).
// Health snapshot surfaced through Cb.getCaptureStats.
//
// Free struct rather than a nested one so cb_devtools_agent.h can
// forward-declare it (the same treatment NativeSessionConfig gets) instead
// of pulling in this whole header.
struct CbSessionHealth {
  int renderer_crashes = 0;
  bool permanent_death_signaled = false;
  bool video_track_ok = false;
};

struct NativeSessionConfig {
  signaling::WsClientConfig ws;
  signaling::IceConfig ice;
};

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
      public cloud_browser::signaling::ReconnectingClientObserver,  // CV2-REDIAL
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
  // CV2-REDIAL: the same four names exist on ReconnectingClientObserver
  // (the R7 wrapper's consumer interface). `override` covers both bases;
  // the wrapper is what actually calls them now, the inner SignalingWsClient
  // reports to the wrapper. OnGaveUp is the wrapper's own.
  void OnConnected() override;
  void OnEnvelope(const cloud_browser::signaling::Envelope& envelope) override;
  void OnClosed(uint16_t code, std::string_view reason) override;  // ws path
  void OnError(std::string_view reason) override;
  void OnGaveUp(uint32_t attempts_made) override;

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
  void OnNewViewerNeedsOffer() override;
  void OnClosed(std::string_view reason) override;  // offerer-driver path
  void OnFailed(std::string_view reason) override;

  // CV2-GPU-DEATH: wired to begin_frame_driver_->SetPermanentDeathCallback().
  // Fired (on the main sequence) when the BeginFrame driver detects permanent
  // renderer/GPU death (30s+ of zero frame production, run11-class). Signals
  // physics to recycle this element (offerer_driver_->CloseUnhealthy() →
  // session_unhealthy envelope → registry.release) then quits the main message
  // loop so the dead microVM's resources free promptly and a fresh guest boots
  // on the next allocate_or_reuse. NOT an observer override — a plain callback
  // target.
  void OnGpuPermanentDeath();

  // Recovery for a dead captured renderer. Posted from the resolver's
  // PrimaryMainFrameRenderProcessGone. Reloads (bounded per window) to
  // rebuild the RenderViewHost, which fires RenderViewHostChanged and
  // re-arms capture through the path that already exists. On budget
  // exhaustion escalates to OnGpuPermanentDeath so physics recycles the
  // guest rather than watching it crash-loop.
  void OnCapturedRendererGone();

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

  // Called by CbDevToolsManagerDelegate after Cb.startFrameSinkCapture
  // successfully resolves and starts capture for a WebContents. This is
  // the shared active-target handoff for M4 typed input dispatch: the
  // input DataChannel carries input envelopes, but the frame-sink
  // capture command establishes which WebContents those envelopes
  // should target. (CV2-95 fix — forwards to
  // active_webcontents_resolver_.SetActiveCapture(), the single
  // authority named by cb_active_webcontents_resolver.h.)
  void SetActiveCapture(content::WebContents* web_contents,
                        viz::FrameSinkId frame_sink_id);

  // CV2-WARM — bring up the full native signaling session (WS client,
  // offerer driver, DataChannel host, input/cursor/clipboard/file relays,
  // sendonly audio + video transceivers) from |cfg|. This is the body that
  // PreMainMessageLoopRun historically ran inline when WEBRTC_SIGNALING_*
  // env was set; it is now a method so the Cb.startNativeSession CDP handler
  // can invoke it AFTER a warm-snapshot restore with per-session params,
  // instead of the cold-boot config-drive path.
  //
  // Idempotent: returns RTCError::OK() on the first success and sets
  // native_session_started_; a second call returns INVALID_STATE without
  // mutating state. MUST run on the UI thread. Callable from two contexts:
  //   (1) PreMainMessageLoopRun bootstrap (base sync primitives allowed), or
  //   (2) a CDP HandleCommand task (per-task DisallowBaseSyncPrimitives is
  //       active) — which is why the body wraps its synchronous
  //       signaling_thread_->BlockingCall hops in ScopedAllowBaseSyncPrimitives
  //       (H1). The env-boot path is unaffected by that scope (no-op there).
  webrtc::RTCError StartNativeSession(const NativeSessionConfig& cfg);

  // Apply a viewport (Cb.setViewport dispatch target). Returns the spec
  // ACTUALLY applied, which may be clamped — the CDP handler puts that on
  // the wire so physics can see its request was adjusted instead of
  // assuming it landed verbatim. Safe before the capture pipeline exists.
  CbViewportSpec SetViewport(const CbViewportSpec& spec);
  // CV2-REARM: rebuild the peer-connection layer IN PLACE so this process
  // can serve the next viewer without restarting Chromium — the browser
  // keeps its tabs, scroll position and in-memory logins.
  //
  // Deliberately NOT StartNativeSession(): that function also rebuilds
  // ws_client_, and on a re-arm the signaling socket is still up and must
  // stay up. This rebuilds only what died with the old PeerConnection:
  // the driver (via CbOffererDriver::Rearm + Start), the data channels and
  // their observers, the audio transceiver, and the video track.
  //
  // Returns OK when a fresh offer is on its way; an error (logged by the
  // caller) when the driver refused to re-arm, in which case exiting so
  // supervisord respawns us remains the fallback.
  // |announce_bye| is forwarded to CbOffererDriver::Rearm. Pass false when
  // re-arming FOR a viewer that is already connected and waiting — a bye would
  // be forwarded to that viewer and tear it down. See Rearm's declaration.
  webrtc::RTCError RearmSession(bool announce_bye = true);

  // CV2-REARM helpers, extracted from StartNativeSession so the initial
  // bring-up and a re-arm share ONE code path and cannot drift apart.
  // Call order is load-bearing: data channels FIRST (the SDP must carry the
  // complete native channel set), media SECOND (adding the video transceiver
  // is what triggers OnRenegotiationNeeded -> CreateOffer -> the offer).
  // Both require offerer_driver_->pc() to be live.
  void RebuildNativeDataChannels();
  void RebuildSessionMedia();

  // CV2-REARM: try to re-arm; fall back to process exit if the driver
  // refuses. The single policy point for "the viewer went away".
  // MUST be invoked from a fresh task when the caller is OnClosed — see the
  // PostTask there for why (re-entrancy into CloseInternal).
  void RearmOrShutdown(bool announce_bye = true);

  // OSS-W0 — request a graceful process exit by running the quit closure
  // parked in WillRunMainMessageLoop(). Quitting the RunLoop unwinds into
  // PostMainMessageLoopRun(), so the LIFO teardown there runs in full: the
  // offerer driver emits its `bye` envelope, the WS closes with code 1000,
  // and libwebrtc tears the PeerConnection down cleanly. That is the whole
  // point of exposing this over CDP — SIGTERM/SIGKILL skip the `bye`, so the
  // broker and the remote peer are left inferring the disconnect from a
  // socket error instead of being told.
  //
  // MUST run on the UI thread. Returns false when the closure is unavailable
  // — either the main message loop never started, or shutdown already ran
  // (base::OnceClosure is consumed on first use, so repeat calls are safe
  // and simply report false rather than double-quitting).
  bool Shutdown();

 private:
  // CV2-CAPTURE-REARM: re-arm the FrameSink capturer after a
  // cross-document navigation swapped the captured WebContents'
  // RenderWidgetHost (and thus its FrameSinkId). Invoked (via a posted
  // task) from CbActiveWebContentsResolver::RenderViewHostChanged when a
  // capture was active. Re-resolves the WebContents' current
  // primary-main-frame FrameSinkId and calls cb_track_source_->
  // StartCapture() on it, then refreshes the resolver's active target.
  // No-op if capture torn down or the track source is gone.
  //
  // |attempts_left|: if the WebContents' new RWHV/RWH/FrameSinkId is not
  // resolvable yet (a terminal navigation whose fresh renderer hasn't
  // attached its view by this task turn — no subsequent RVH swap will
  // retrigger us, and physics may not re-issue Cb.startFrameSinkCapture),
  // re-post ourselves with a short delay up to |attempts_left| times so a
  // just-committed nav still lands capture on its sink. Runs on the UI
  // thread. See cb_active_webcontents_resolver.h
  // SetRecaptureOnRvhSwapCallback.
  void RearmCaptureAfterRvhSwap(int attempts_left);

  // CV2-CAPTURE-FALLBACK — the captured tab was destroyed (a popup closed
  // itself while streamed); re-arm capture on the initial tab.
  void RearmCaptureOnInitialTab();

  // CV2-KEYFRAME — ask the video encoder for an IDR after the capturer
  // reports a wholesale content change (retarget, resize).
  void RequestVideoKeyFrame();

  // Reads --remote-debugging-port (default 0 = ephemeral, loopback)
  // and starts content::DevToolsAgentHost::StartRemoteDebuggingServer
  // bound at 127.0.0.1:<port>. Idempotent — only called once from
  // PreMainMessageLoopRun.
  void StartDevToolsHttpHandler();

  // Symmetric counterpart called from PostMainMessageLoopRun. Calls
  // StopRemoteDebuggingServer iff StartDevToolsHttpHandler ran.
  void StopDevToolsHttpHandler();

  // CV2 Gate 6 media-RTP observability: poll the PeerConnection's
  // outbound-rtp stats and LOG(INFO) packets_sent / bytes_sent /
  // frames_encoded / frames_sent / frame WxH. Started on the first
  // ICE-connected transition (OnIceConnectionStateChanged). This is the
  // ONLY way to tell, post-WS-reassembly-fix, whether the guest's encoder
  // is actually pushing RTP into the relay (packets_sent grows) vs the
  // media stalling before the wire (packets_sent stays 0) — the guest PC
  // is native libwebrtc, invisible to CDP/JS getStats, and the native
  // GetStats relay (M6 R1) is otherwise unwired.
  void PollOutboundRtpStats();

  // Owned global display::Screen instance. chromium fatals on
  // `Check failed: Screen::Get()` from ui/display/display_observer.cc:32
  // during browser-process init when something registers a
  // DisplayObserver against a null Screen — the worker hits this even
  // though it never paints to a real surface, because internal
  // subsystems (audio, prefetch, ...) attach observers as part of
  // their startup. Constructed in PreEarlyInitialization before the
  // BrowserContext + initial WebContents so the registration order is
  // safe.
  //
  // CV2-78 (M5 R1 cursor-routing gate) — type bumped from
  // display::ScreenBase to CbHeadlessScreen so aura's pre-SetCursor
  // gate (IsWindowUnderCursor) returns true and cursor routing
  // reaches CbCursorClient::SetCursor instead of short-circuiting on
  // the upstream ScreenBase stub. See cb_headless_screen.h for the
  // full rationale.
  std::unique_ptr<CbHeadlessScreen> screen_;

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

  // CV2-ICE — drives the offscreen root compositor (and, via the viz
  // frame-sink hierarchy, the captured renderer) at the target fps by issuing
  // external BeginFrames on aura_->host()->compositor(). This is THE fix for
  // the ~0.5 fps capture starvation: the FrameSinkVideoCapturer is a pull
  // consumer and does NOT request BeginFrames, and there is no real vsync on
  // Xvfb, so without a driven BeginFrameSource the captured renderer idles at
  // viz's 1s refresh. The driver's draw+swap each tick ALSO emits the
  // present-acks that drain Blink's presentation-callback deque, so it
  // subsumes the former ScheduleCompositorKeepaliveRedraw keepalive (which
  // only drove the root UI compositor and never the renderer's frame sink).
  // See cb_begin_frame_driver.h.
  //
  // Holds a raw ui::Compositor* into aura_, so it MUST be destroyed before
  // aura_ — declared AFTER aura_ (reverse-order destruction) AND explicitly
  // reset() in PostMainMessageLoopRun before aura_.release().
  std::unique_ptr<CbBeginFrameDriver> begin_frame_driver_;

  // Viewport / resize. Holds RAW pointers to screen_, aura_ and (once
  // injected) the capturer inside cb_track_source_, so it must not
  // outlive any of them. PostMainMessageLoopRun resets it explicitly
  // before all three; the declaration position here makes reverse-order
  // destruction agree with that, so the invariant survives a member
  // reshuffle. Owns nothing itself.
  std::unique_ptr<CbViewportController> viewport_controller_;

  // Renderer-crash recovery budget. Windowed, not lifetime: a crash an
  // hour into a session is unrelated to one at boot, and a lifetime
  // counter would refuse to recover from the former because of the
  // latter. See OnCapturedRendererGone.
  base::TimeTicks renderer_crash_window_start_;
  int renderer_crashes_in_window_ = 0;
  // Monotonic, for Cb.getCaptureStats — never reset by the window.
  int renderer_crashes_total_ = 0;

  // Set when the guest has decided it is permanently dead (GPU death, or a
  // renderer crash-loop that exhausted the reload budget). Exposed through
  // Cb.getCaptureStats so a poller can distinguish "this guest knows it is
  // dying" from "this guest is wedged and does not know it" — the two need
  // different responses, and today they look identical from outside.
  bool permanent_death_signaled_ = false;

 public:
  // Health snapshot for Cb.getCaptureStats. Read-only; safe at any time.
  CbSessionHealth GetSessionHealth() const;

 private:

  // Drives PollOutboundRtpStats() every 2s once ICE connects. Armed once
  // (guarded by rtp_stats_timer_armed_) on the first kIceConnectionConnected
  // / kIceConnectionCompleted transition. CV2 Gate 6 media-RTP diagnosis.
  base::RepeatingTimer rtp_stats_timer_;
  bool rtp_stats_timer_armed_ = false;

  // CV2-BYELESS: a viewer that vanishes without a `bye` never closes the
  // session, so OnClosed (and with it Shutdown, and supervisord's restart) is
  // never reached — the process is left holding a FAILED peer connection with
  // no path back. Measured 2026-08-20 (task 349025): ICE went
  // connected -> completed -> disconnected -> failed and `session closed` never
  // appeared. A lid-close, crash or network drop produces exactly this.
  //
  // ICE `failed` is the signal that the viewer is gone for good. It is NOT
  // acted on immediately: `disconnected` is often transient (a Wi-Fi blip
  // recovers), and libwebrtc can go failed -> connected on its own after an
  // ICE restart. So arm a one-shot grace timer on `failed` and only tear down
  // if it is still failed when the timer runs; any healthy state cancels it.
  base::OneShotTimer ice_failed_teardown_timer_;

  // Fires when ice_failed_teardown_timer_ elapses without ICE recovering.
  void OnIceFailedGraceElapsed();

  std::unique_ptr<CloudBrowserBrowserContext> browser_context_;
  std::unique_ptr<content::WebContents> initial_web_contents_;

  // CV2 capture-keepalive (RCA 2026-06-30). The WebContents capturer-count
  // handle. WITHOUT it the renderer applies "hidden rendering" optimizations
  // and stops emitting CompositorFrames when not visibly on-screen — even
  // though we drive external BeginFrames and call WasShown(). web_contents.h
  // (M140) is explicit: "Both internal-to-content and embedders must increment
  // the capturer count while capturing ... renderers will be configured to
  // produce compositor frames regardless of their 'backgrounded' or on-screen
  // occlusion state." This was the ~50% cold-guest RENDERER-STARVED defect
  // (BeginFrames issued+acked at 29fps but frames_received=0): the external
  // BeginFrame source reaches the renderer only when it has SUBSCRIBED, and an
  // un-pinned renderer drops its subscription. Holding this handle for the
  // session keeps the renderer producing. ScopedClosureRunner releases the
  // count on destruction; declared AFTER initial_web_contents_ so reverse-order
  // member destruction drops the handle while the WebContents is still alive.
  base::ScopedClosureRunner capture_keepalive_handle_;

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
  //
  // CV2-REDIAL (2026-09-08): ws_client_ is now the R7 reconnect wrapper, not
  // the bare R2 client. The wrapper owns and REPLACES an inner
  // SignalingWsClient on every drop (exponential backoff, 1 s → 32 s, ten
  // attempts), and IS-A SignalingTransport so the driver's raw transport
  // pointer stays valid across redials. Before this the socket was dialled
  // exactly once per process: every broker rollout dropped it with 1006,
  // nothing redialed, and the liveness probe in stack.yaml killed the
  // container ~70 s later — measured three times on 2026-09-07
  // (docs/findings/worker-signaling-no-redial.md). On reconnect see
  // OnConnected: a driver with no live session re-offers to the new broker.
  std::unique_ptr<cloud_browser::signaling::CbSignalingReconnect> ws_client_;
  std::unique_ptr<audio::CbAudioLifecycle> audio_lifecycle_;
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
  // CV2-KEYFRAME: the video RtpSender of the CURRENT PC, for
  // RequestVideoKeyFrame. Cleared alongside video_track_ (re-arm, teardown).
  webrtc::scoped_refptr<webrtc::RtpSenderInterface> video_sender_;

  // CV2-WARM — set true once StartNativeSession() succeeds. Guards against a
  // double bring-up (env boot then a stray Cb.startNativeSession, or two CDP
  // calls). Checked-and-set within one UI-thread task, so no lock is needed.
  bool native_session_started_ = false;
  // CV2-REDIAL: distinguishes the FIRST OnConnected (the boot dial — the
  // driver's own offer is about to go out) from a redial after the broker
  // dropped us. See OnConnected.
  bool signaling_connected_once_ = false;
  // CV2-REDIAL: the last ICE state the driver reported, so OnConnected can
  // tell "viewer still attached" from "nobody here".
  webrtc::PeerConnectionInterface::IceConnectionState last_ice_state_ =
      webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionNew;

  // CV2-REARM: the config the session was brought up with, kept so
  // RearmSession() can rebuild the PC with the SAME ICE servers and
  // session id without re-reading env or re-dialling the websocket.
  NativeSessionConfig native_session_cfg_;

  // CV2-WARM — non-owning ADM pointer captured at PCF construction (the
  // worker_thread_ BlockingCall in PreMainMessageLoopRun). audio_lifecycle_
  // needs it, and StartNativeSession may now run AFTER PreMainMessageLoopRun
  // (from a CDP call), so the pointer is promoted to a member instead of a
  // PreMainMessageLoopRun local. Lifetime: owned by pcf_, valid for the
  // worker's life — same as the original local `adm_debug`. raw_ptr<> per the
  // chromium-rawptr plugin (matches CbAudioLifecycle::adm_debug_).
  raw_ptr<webrtc::AudioDeviceModule> adm_for_audio_lifecycle_ = nullptr;

  // CV2-83 — answerer-facing DataChannels, owned by CbDataChannelHost
  // instead of four ad-hoc scoped_refptr members. The host creates the
  // canonical channel labels before the first offer and owns the
  // observer trampoline that fans messages/state to per-channel
  // consumers.
  //
  // Labels are wire-contract — must match the chromeless/client
  // TypeScript answerer's hardcoded labels EXACTLY (Trap #1):
  //   CbDcLabel::kInput     → channel label "input"
  //   CbDcLabel::kCursor    → channel label "cursor"
  //   CbDcLabel::kClipboard → channel label "clipboard"
  //   CbDcLabel::kFiles     → channel label "files" (NOT
  //                           "file-upload" — the FILE name is
  //                           file-upload.ts but the CHANNEL is
  //                           "files")
  std::unique_ptr<cloud_browser::signaling::CbDataChannelHost> dc_host_;
  // ============== END CV2-69 ==============

  // ============== CV2-75 RUNTIME-WIRE (Ring 2) ==============
  //
  // M4/M5/M6 consumer attachment — the gap that surfaced as the
  // architectural Phase-A failure: CV2-69 created the DCs but never
  // bound observers, so inbound SCTP frames sat in libwebrtc's read
  // buffer with no handler. CV2-75 closed input/clipboard/files;
  // CV2-83 adopts CbDataChannelHost and wires the cursor egress path.
  //
  // Consumers attached here:
  //   * CbInputDispatch        → kInput     (M4 R1, CV2-41)
  //   * CbCursorDcEmitter      → kCursor    (M5 R6, CV2-24)
  //   * CbClipboardRelay       → kClipboard (M6 R2, CV2-34)
  //   * CbFileUploadRelay      → kFiles     (M6 R3, CV2-35)
  //
  // Deferred to follow-up: CbStatsRelay (M6 R1, CV2-33). The "stats"
  // channel may open, but without a relay its inbound frames are
  // intentionally dropped by the host's unbound observer slot.
  //
  // Construction-arg notes (read the consumer headers for full
  // contracts):
  //   * CbInputDispatch wants a UI task runner + a delegate. We pass
  //     content::GetUIThreadTaskRunner({}) + the M4 typed-pipeline
  //     delegate CbInputDispatchCompositeDelegate (CV2-81), which
  //     fans the envelope into R3..R8. The composite is constructed
  //     with a pointer to active_webcontents_resolver_ (M4 R2) which
  //     CbDevToolsManagerDelegate also reaches into on each
  //     Cb.startFrameSinkCapture (single resolver instance per
  //     browser process — see cb_active_webcontents_resolver.h).
  //     CV2-75 R1 wired CbInputLoggingDelegate as a transitional
  //     stand-in; CV2-81 retired it once R2..R10 source landed.
  //   * CbClipboardRelay (CV2-CLIPBOARD, 2026-09) handles both directions of
  //     the clipboard DC directly against ui::Clipboard: an inbound envelope
  //     writes the guest clipboard and synthesises Ctrl+V, a copy gesture
  //     arms a window in which the next clipboard change is forwarded. The
  //     WebSocket bridge relay it replaced never had a backend (url="off")
  //     and dropped every paste for four months.
  //   * CbFileUploadBridgeWsClient still takes a `label`/`url` +
  //     io_task_runner and is passed url="off", which keeps it in
  //     `disabled()` mode — its WS backend is a TODO(M6-R3-ws-backend) and
  //     file transfer on the "files" DC is INERT until Batch B replaces it
  //     the same way.
  //
  // Teardown ordering: explicit LIFO in PostMainMessageLoopRun
  // BEFORE the DataChannel host drops its DC refs. Unbind each
  // observer first (must outlive the DC ref drop to avoid
  // use-after-free in libwebrtc's late callbacks); then drop the
  // consumer state; then the existing CV2-69 LIFO drops the peer.
  // CV2-81 — M4 R2 active-WebContents resolver. Single instance per
  // browser process; held as a value member so the address is stable
  // across the lifetime of `this`. Threaded into the composite delegate
  // (R3..R8) at construction + into CbDevToolsManagerDelegate's
  // SetActiveCapture call path. Must outlive both consumers; declaration
  // order here puts it BEFORE input_delegate_ so destruction is reverse
  // (delegate -> resolver), matching the "resolver_lifetime >
  // dispatcher_lifetime" contract documented on
  // cb_active_webcontents_resolver.h:123.
  CbActiveWebContentsResolver active_webcontents_resolver_;
  std::unique_ptr<CbInputDispatchCompositeDelegate> input_delegate_;
  std::unique_ptr<CbInputDispatch> input_dispatch_;
  std::unique_ptr<CbCursorXyJoin> cursor_xy_join_;
  std::unique_ptr<cursor::EmitPolicy> cursor_emit_policy_;
  std::unique_ptr<cursor::EnvelopeAssembler> cursor_envelope_assembler_;
  std::unique_ptr<cursor::CbCursorDcEmitter> cursor_dc_emitter_;
  std::unique_ptr<CbClipboardRelay> clipboard_relay_;
  std::unique_ptr<CbFileUploadBridgeWsClient> file_upload_ws_;
  std::unique_ptr<CbFileUploadRelay> file_upload_relay_;
  // ============== END CV2-75 ==============

  // Browser-fidelity wave 1 — the ask-a-human channel (kControl DC).
  // Owned here because its lifetime is the SESSION's, while its consumer
  // (the JS dialog manager, hanging off the process-lifetime
  // WebContentsDelegate singleton) outlives the session. The delegate is
  // handed a raw pointer via SetSessionContext and MUST have it cleared in
  // PostMainMessageLoopRun before this unique_ptr drops, or a dialog
  // raised during teardown would dereference freed memory.
  std::unique_ptr<CbControlChannel> control_channel_;

  bool devtools_http_handler_started_ = false;

  // Captured in WillRunMainMessageLoop, run in PostMainMessageLoopRun
  // (or never, if the process is killed). Triggered in-process by
  // Cb.shutdown and by OnClosed (a closed session is unrecoverable —
  // see the comment there).
  base::OnceClosure quit_main_message_loop_;

  // Set once teardown has begun, so OnClosed can tell "the viewer left, exit
  // so the supervisor gives us a fresh process" apart from "we are ALREADY
  // exiting and this is our own teardown talking".
  //
  // Both look identical at the observer: PostMainMessageLoopRun calls
  // offerer_driver_->Close("session ended"), which fires OnClosed. Without
  // this, exiting-on-close would re-enter Shutdown() from inside the shutdown
  // path. Shutdown() is idempotent (the OnceClosure is already consumed, so it
  // returns false with a warning) — but depending on that to be safe is the
  // kind of thing that stops being true when someone edits Shutdown(), and
  // the log line it emits reads like a defect. Guard explicitly instead.
  bool tearing_down_ = false;

  // CV2-REARM: true only while RearmSession() is running.
  //
  // RearmSession -> offerer_driver_->Rearm() -> CloseInternal() -> our
  // OnClosed(reason), which POSTS another RearmOrShutdown. Measured live
  // 2026-08-21: two RearmSession entries ~450us apart, the second one landing
  // after the driver was already rebuilt and therefore failing with
  // "offerer driver refused to re-arm" -> fallback process exit. The re-arm
  // worked and then immediately undid itself, which looks exactly like the
  // recycle it was meant to replace.
  //
  // Same shape as tearing_down_ above: an OnClosed that WE caused is not a
  // viewer leaving, so it must not trigger a fresh re-arm.
  bool rearming_ = false;
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CLOUD_BROWSER_BROWSER_MAIN_PARTS_H_

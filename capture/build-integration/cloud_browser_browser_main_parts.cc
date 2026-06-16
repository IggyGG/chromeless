// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserBrowserMainParts — see cloud_browser_browser_main_parts.h.

#include "capture/build-integration/cloud_browser_browser_main_parts.h"

#include <stdint.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "api/audio/audio_device.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/make_ref_counted.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtp_parameters.h"
#include "api/rtp_transceiver_interface.h"
#include "api/scoped_refptr.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtc_stats_report.h"
#include "api/stats/rtcstats_objects.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "capture/audio/cb_audio_lifecycle.h"
#include "capture/audio/cb_audio_options.h"
#include "capture/audio/cb_audio_track.h"
#include "capture/build-integration/cb_aura_platform_data.h"
#include "capture/build-integration/cb_begin_frame_driver.h"  // CV2-ICE
#include "capture/build-integration/cb_cursor_xy_join.h"
#include "capture/build-integration/cb_headless_screen.h"  // CV2-78
#include "capture/build-integration/cloud_browser_browser_context.h"
#include "capture/build-integration/cloud_browser_pcf.h"
#include "capture/cursor/cb_cursor_dc_emitter.h"
#include "capture/cursor/cb_cursor_emit_policy.h"
#include "capture/cursor/cb_cursor_envelope.h"
#include "capture/framesink-capturer/capturer.h"
#include "capture/framesink-capturer/cb_framesink_video_track_source.h"
#include "capture/signaling/cb_dc_host.h"
#include "capture/signaling/cb_ice_config.h"           // CV2-69
#include "capture/signaling/cb_offerer_driver.h"       // CV2-69
#include "capture/signaling/cb_signaling_ws_client.h"  // CV2-69
#include "capture/signaling/cb_wire_envelope.h"        // CV2-69
#include "components/viz/host/host_frame_sink_manager.h"
#include "content/browser/compositor/surface_utils.h"  // nogncheck — same
#include "content/public/browser/browser_thread.h"     // CV2-75
#include "content/public/browser/storage_partition.h"  // CV2-69
                                                       // visibility caveat
// as cb_devtools_agent.cc;
// patches/0005 unblock
// applies here too.
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_socket_factory.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/result_codes.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/ip_address.h"
#include "net/base/net_errors.h"
#include "net/log/net_log_source.h"
#include "net/socket/server_socket.h"
#include "net/socket/tcp_server_socket.h"
#include "rtc_base/thread.h"
#include "services/viz/privileged/mojom/compositing/frame_sink_video_capture.mojom.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/base/page_transition_types.h"
#include "ui/compositor/compositor.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace cloud_browser {

namespace {

// Listen backlog for the DevTools HTTP server socket. Matches the
// constants used by content_shell + headless.
constexpr int kBackLog = 10;

// TCP server-socket factory bound to <address>:<port>. The address
// comes from --remote-debugging-address (default 127.0.0.1).
// Required for cb-browserless deployment so the kubelet readiness
// probe + ClusterIP service routing can reach the listener — when
// hardcoded to loopback, only intra-pod curl works.
class ConfigurableTCPServerSocketFactory
    : public content::DevToolsSocketFactory {
 public:
  ConfigurableTCPServerSocketFactory(net::IPAddress address, uint16_t port)
      : address_(std::move(address)), port_(port) {}

  ConfigurableTCPServerSocketFactory(
      const ConfigurableTCPServerSocketFactory&) = delete;
  ConfigurableTCPServerSocketFactory& operator=(
      const ConfigurableTCPServerSocketFactory&) = delete;

 private:
  std::unique_ptr<net::ServerSocket> CreateForHttpServer() override {
    auto socket =
        std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
    const std::string address_str = address_.ToString();
    if (socket->ListenWithAddressAndPort(address_str, port_, kBackLog) !=
        net::OK) {
      LOG(ERROR) << "DevTools HTTP listener: failed to bind " << address_str
                 << ":" << port_;
      return nullptr;
    }
    LOG(INFO) << "DevTools HTTP listener bound on " << address_str << ":"
              << port_;
    return socket;
  }

  std::unique_ptr<net::ServerSocket> CreateForTethering(
      std::string* /*out_name*/) override {
    return nullptr;
  }

  const net::IPAddress address_;
  const uint16_t port_;
};

// Reads --remote-debugging-port from the command line. Returns 0 (=
// ephemeral) when the flag is missing or unparseable. Matches
// content_shell's behaviour exactly.
uint16_t ReadRemoteDebuggingPort() {
  const base::CommandLine& cmd = *base::CommandLine::ForCurrentProcess();
  if (!cmd.HasSwitch(::switches::kRemoteDebuggingPort)) {
    return 0;
  }
  int parsed = 0;
  const std::string value =
      cmd.GetSwitchValueASCII(::switches::kRemoteDebuggingPort);
  if (!base::StringToInt(value, &parsed) || parsed < 0 || parsed > 65535) {
    LOG(WARNING) << "Invalid --remote-debugging-port value '" << value
                 << "'; falling back to an ephemeral port.";
    return 0;
  }
  return static_cast<uint16_t>(parsed);
}

std::string FormatCodecPreferenceNamesForLog(
    const std::vector<webrtc::RtpCodecCapability>& codecs) {
  std::string out = "[";
  bool first = true;
  for (const auto& codec : codecs) {
    if (!first) {
      out += ",";
    }
    out += codec.name;
    first = false;
  }
  out += "]";
  return out;
}

// Reads --remote-debugging-address from the command line. Returns
// IPv4Localhost when the flag is missing OR malformed (matches
// content_shell's behaviour and avoids accidental "open to the
// internet" if a typo lands). Pods that need cluster-internal
// reachability (cb-browserless deployment) pass 0.0.0.0 explicitly.
net::IPAddress ReadRemoteDebuggingAddress() {
  const base::CommandLine& cmd = *base::CommandLine::ForCurrentProcess();
  if (!cmd.HasSwitch("remote-debugging-address")) {
    return net::IPAddress::IPv4Localhost();
  }
  const std::string value = cmd.GetSwitchValueASCII("remote-debugging-address");
  net::IPAddress parsed;
  if (!parsed.AssignFromIPLiteral(value)) {
    LOG(WARNING) << "Invalid --remote-debugging-address value '" << value
                 << "'; falling back to 127.0.0.1.";
    return net::IPAddress::IPv4Localhost();
  }
  return parsed;
}

}  // namespace

CloudBrowserBrowserMainParts::CloudBrowserBrowserMainParts() = default;

CloudBrowserBrowserMainParts::~CloudBrowserBrowserMainParts() = default;

content::BrowserContext* CloudBrowserBrowserMainParts::browser_context() const {
  return browser_context_.get();
}

aura::Window* CloudBrowserBrowserMainParts::aura_root_window() const {
  // aura_->host() is the WindowTreeHost; ->window() is the host's
  // root aura::Window — the same handle WebContentsViewAura needs as
  // its ParentWindowWithContext target. Returns nullptr before
  // PreMainMessageLoopRun has constructed aura_.
  if (!aura_) {
    return nullptr;
  }
  return aura_->host()->window();
}

CloudBrowserFrameSinkVideoTrackSource*
CloudBrowserBrowserMainParts::cb_track_source() const {
  // scoped_refptr<...>::get() — bare pointer for the delegate's raw_ptr
  // (the delegate never bumps the refcount; main_parts holds the only
  // strong ref). Returns nullptr until PreMainMessageLoopRun step 5b
  // has constructed cb_track_source_.
  return cb_track_source_.get();
}

void CloudBrowserBrowserMainParts::SetActiveCapture(
    content::WebContents* web_contents,
    viz::FrameSinkId frame_sink_id) {
  if (!web_contents || !frame_sink_id.is_valid()) {
    active_webcontents_resolver_.SetActiveCapture(nullptr, viz::FrameSinkId());
    LOG(WARNING) << "CV2-81: active capture cleared by invalid "
                    "SetActiveCapture input";
    return;
  }

  web_contents->Focus();
  active_webcontents_resolver_.SetActiveCapture(web_contents, frame_sink_id);
  if (screen_ && input_delegate_) {
    screen_->SetLastPointerSource(input_delegate_->last_pointer_state());
  }
  LOG(INFO) << "CV2-81: active input target set from "
               "Cb.startFrameSinkCapture, fsid="
            << frame_sink_id.ToString();
}

namespace {

// Internal default display geometry. Matches the Xvfb resolution the
// cb-chromium pod brings up (`Xvfb :99 -screen 0 1280x720x24`) so the
// virtual display the embedder reports is the same one chromium would
// have observed if it were Aura-driven on the X11 server.
constexpr int64_t kDefaultDisplayId = 1;
constexpr int kDefaultDisplayWidth = 1280;
constexpr int kDefaultDisplayHeight = 720;

}  // namespace

int CloudBrowserBrowserMainParts::PreEarlyInitialization() {
  // Global display::Screen — done at the EARLIEST available embedder
  // hook. chromium subsystems register DisplayObservers during the
  // PreCreateThreads phase (well before PreMainMessageLoopRun), and
  // without a global Screen the worker fatals at
  // `Check failed: Screen::Get()` (ui/display/display_observer.cc:32).
  // First validation attempt set the Screen in PreMainMessageLoopRun
  // and still fataled — by the time PreMainMessageLoopRun is invoked,
  // chromium has already created an observer in some service-init
  // path between PostCreateThreads and PreMainMessageLoopRun.
  // PreEarlyInitialization is the embedder's first chance to run code
  // before any of that, so the Screen lands here.
  //
  // A CbHeadlessScreen with a single 1280x720 display matches the Xvfb
  // resolution the cb-chromium pod brings up and gives chromium's
  // DisplayObservers something to attach to.
  //
  // CV2-78 (M5 R1 cursor-routing gate): CbHeadlessScreen overrides the
  // two upstream ScreenBase stubs (IsWindowUnderCursor returning false,
  // GetCursorScreenPoint returning gfx::Point() via
  // NOTIMPLEMENTED_LOG_ONCE) that closed the gate sitting UPSTREAM of
  // CbCursorClient::SetCursor. With the gate open, aura's renderer-
  // driven cursor-style changes (hover over `cursor: pointer`) reach
  // the CursorClient registered by CbAuraPlatformData and the M5 R6
  // emit chain can fire. See cb_headless_screen.h for the full
  // rationale; the display-list construction is unchanged from the
  // bare-ScreenBase predecessor.
  if (!display::Screen::HasScreen()) {
    screen_ = std::make_unique<CbHeadlessScreen>();
    display::Display default_display(
        kDefaultDisplayId,
        gfx::Rect(0, 0, kDefaultDisplayWidth, kDefaultDisplayHeight));
    default_display.set_device_scale_factor(1.0f);
    screen_->display_list().AddDisplay(default_display,
                                       display::DisplayList::Type::PRIMARY);
    display::Screen::SetScreenInstance(screen_.get());
  }
  return content::RESULT_CODE_NORMAL_EXIT;
}

int CloudBrowserBrowserMainParts::PreMainMessageLoopRun() {
  // 1. Profile.
  browser_context_ = std::make_unique<CloudBrowserBrowserContext>();

  // 1a. Aura platform data — root WindowTreeHost + focus / parenting /
  //     activation / capture clients. Constructed BEFORE the initial
  //     WebContents so the boot tab can pass aura_->host()->window()
  //     as its CreateParams::context, which makes WebContentsViewAura::
  //     CreateAuraWindow's ParentWindowWithContext call succeed (see
  //     content/browser/web_contents/web_contents_view_aura.cc:992 in
  //     pinned 7727). Without this, the WebContents view floats outside
  //     Aura's focus chain, WebContents::Focus() is a silent no-op, and
  //     the renderer-side WidgetInputHandler binds in "no focused page"
  //     state — CDP Input.dispatch{Mouse,Key}Event is then dropped on
  //     the floor by the renderer despite acking at the protocol layer.
  //     This was the deeper root cause of BUGS-529 (the second-layer
  //     fix on top of 9703db5's WebContents::WasShown + Focus calls).
  //
  //     1280x720 matches the Xvfb resolution the cb-chromium pod brings
  //     up (see infra/launch-chromium.sh + the cb-webrtc-wire-bridge-
  //     validation.yaml init container). PageRenderingViewport scales
  //     beyond this via the standard renderer-side viewport machinery;
  //     this is just the host window's initial bounds.
  aura_ = std::make_unique<CbAuraPlatformData>(gfx::Size(1280, 720));
  if (screen_) {
    screen_->SetRootWindow(aura_root_window());
  }

  // Note for CV2-75 (M5 R1 / CbCursorClient): the cursor-client is
  // ALREADY constructed + registered by CbAuraPlatformData's ctor
  // (cb_aura_platform_data.cc:152-153). main_parts MUST NOT re-create
  // or re-register here — that would either crash via double-Observe
  // on the aura ObservationManager or orphan the original registration.
  // M5 R1 runtime-wire is therefore satisfied at the aura platform
  // layer; the deferred piece is M5 R6 / CbCursorDcEmitter (the DC
  // emit binder), which requires cb_dc_host adoption and is tracked
  // in follow-up cv2/m3-r5-dc-host-adoption.

  // 2. Initial WebContents on about:blank — this is what hangs off the
  //    BrowserContext and gives DevToolsAgentHost a target to publish
  //    in /json. Without at least one WebContents, /json returns [] and
  //    the e2e test cannot attach to anything.
  content::WebContents::CreateParams create_params(browser_context_.get());
  // Pin the parenting context to the Aura root we own. WebContentsView
  // Aura::CreateAuraWindow walks |context|->GetRootWindow() and calls
  // aura::client::ParentWindowWithContext on that, which our
  // CbWindowParentingClient resolves to aura_->host()->window().
  // Without this, the WebContents view is not parented to anything
  // and falls outside the focus chain — see PreMainMessageLoopRun
  // step 1a comment for the BUGS-529 chain.
  create_params.context = aura_->host()->window();
  initial_web_contents_ = content::WebContents::Create(create_params);
  CHECK(initial_web_contents_)
      << "WebContents::Create returned null — chromium browser process "
      << "is misconfigured (renderer host process not yet up?).";

  // WebContents::WasShown() below makes Chromium treat the page as visible, but
  // it does not show the Aura container window created by WebContentsViewAura.
  // The renderer can still lay out in that state, yet Aura hit testing returns
  // no event handler because the parent container is hidden; cursor routing
  // then stops before CbCursorClient::SetCursor. Show the native view
  // explicitly so root_window->GetEventHandlerForPoint(...) can descend into
  // the RWHV child.
  if (aura::Window* native_view = initial_web_contents_->GetNativeView()) {
    native_view->Show();
  }

  // 2a. Mark the WebContents as visible + focused. Without WasShown(),
  //     chromium leaves the WebContents in Visibility::HIDDEN — the
  //     RenderWidgetHostView never receives ShowWithVisibility() and
  //     the renderer-side WidgetInputHandler is bound in a state where
  //     CDP-injected Input.dispatch{Mouse,Key}Event silently no-op
  //     because the renderer treats the page as backgrounded. Frames
  //     still render (canvas captureStream / framesink capture force
  //     the renderer to keep producing output via separate capturer
  //     refcounts), so the failure is invisible at the wire layer.
  //     This was BUGS-529.
  //
  //     Mirrors HeadlessWebContentsImpl which calls WasShown() on every
  //     contents at construction, and content_shell which gets WasShown
  //     transitively via Shell::PlatformSetContents. Without a platform
  //     window we have no implicit caller, so we do it explicitly.
  //
  //     Focus() is the analogue of Shell::PlatformSetContents'
  //     parent->AddChild + content->Show + web_contents_->Focus chain;
  //     without it, the page never receives focus and certain key
  //     event paths (those that route through the focused frame)
  //     drop input on the floor.
  initial_web_contents_->WasShown();
  initial_web_contents_->Focus();

  // CV2-ICE: start the BeginFrame driver. This is THE primary fix for the
  // ~0.5 fps capture starvation. The FrameSinkVideoCapturer is a pull
  // consumer that does NOT request BeginFrames, and cb-chromium has no real
  // vsync on Xvfb, so without a driven BeginFrameSource the captured renderer
  // only commits CompositorFrames on content damage and viz idle-refreshes
  // the last surface ~once/second. The driver issues external BeginFrames on
  // the root compositor at the target rate; the viz frame-sink hierarchy
  // propagates them to the captured renderer (a hierarchy child of the root
  // compositor frame sink), driving Blink to produce new frames the capturer
  // can deliver. Each tick's draw+swap also emits the present-acks that drain
  // Blink's LayerTreeView presentation-callback deque, so this also subsumes
  // the former ScheduleCompositorKeepaliveRedraw keepalive (which never drove
  // the renderer). 30 fps is the documented target — NOT 60: each tick is an
  // unviewed software composite + blit on the GPU-less worker, so the CPU
  // cost scales with the rate. See cb_begin_frame_driver.h.
  begin_frame_driver_ = std::make_unique<CbBeginFrameDriver>(
      aura_->host()->compositor(),
      /*target_frame_interval=*/base::Hertz(30));
  begin_frame_driver_->Start();

  // BUGS-529 diagnostic — confirms the smoking-gun pattern is closed.
  // Pre-fix expectation: HasFocus=false, ViewBounds=0x0.
  // Post-fix expectation: HasFocus=true, ViewBounds=non-zero.
  if (auto* rwhv = initial_web_contents_->GetRenderWidgetHostView()) {
    LOG(INFO) << "CloudBrowserBrowserMainParts: boot WebContents post-Focus "
                 "RWHV bounds="
              << rwhv->GetViewBounds().ToString()
              << " hasFocus=" << rwhv->HasFocus() << " visibility="
              << static_cast<int>(initial_web_contents_->GetVisibility());
  } else {
    LOG(WARNING) << "CloudBrowserBrowserMainParts: boot WebContents has "
                    "no RenderWidgetHostView yet (renderer not up?)";
  }

  content::NavigationController::LoadURLParams load_params{
      GURL(url::kAboutBlankURL)};
  load_params.transition_type = ui::PageTransitionFromInt(
      ui::PAGE_TRANSITION_TYPED | ui::PAGE_TRANSITION_FROM_ADDRESS_BAR);
  initial_web_contents_->GetController().LoadURLWithParams(load_params);

  // 3. Register the WebContents with DevToolsAgentHost. GetOrCreateFor
  //    is idempotent and returns a refcounted handle; the registration
  //    side-effect is what we actually want (the handle itself is
  //    discarded — the host keeps a global registry of all live agent
  //    hosts and that's what /json walks).
  std::ignore =
      content::DevToolsAgentHost::GetOrCreateFor(initial_web_contents_.get());

  // 4. DevTools HTTP listener — bind
  // <--remote-debugging-address>:<--remote-debugging-port>.
  StartDevToolsHttpHandler();

  // 5. Browser-process PeerConnectionFactory (ChromelessV2 M1 —
  //    CV2-26 / CV2-27).
  //
  //    Construct 3 dedicated webrtc::Threads (network / worker /
  //    signaling), build a webrtc::Environment, and hand them to
  //    CreateCloudBrowserPcf() which injects CloudBrowserVideoEncoder
  //    Factory under default Config{} (VP9 + H264 + AV1) and the M1
  //    dummy / no-audio ADM.
  //
  //    This is the seam M0-R5's assertion #3 swappable probe targets
  //    by scraping the FormatPcfVideoCodecLogLine output below from
  //    the container log. M2 will hang a VideoTrackSource off pcf_;
  //    M3 will create PeerConnections + DataChannels; M5.5 will
  //    substitute its real ADM at CreateCloudBrowserDefaultAudio
  //    DeviceModule() without touching this call site.
  //
  //    Thread setup: network thread MUST be CreateWithSocketServer
  //    (it owns libwebrtc's net socket dispatch); worker + signaling
  //    are plain Threads. Names are diagnostic-only.
  network_thread_ = webrtc::Thread::CreateWithSocketServer();
  worker_thread_ = webrtc::Thread::Create();
  signaling_thread_ = webrtc::Thread::Create();
  network_thread_->SetName("cb-pcf-net", nullptr);
  worker_thread_->SetName("cb-pcf-worker", nullptr);
  signaling_thread_->SetName("cb-pcf-signaling", nullptr);
  network_thread_->Start();
  worker_thread_->Start();
  signaling_thread_->Start();

  webrtc::Environment env = webrtc::CreateEnvironment();

  // CV2-75 Ring N+1 fix-forward — surgical thread-marshal for AudioDevice
  // construction (mirrors the CV2-69 re-test #3 signaling-thread marshal
  // for PC ops; same pattern, different webrtc subsystem).
  //
  // Background: with rtc_include_pulse_audio=true (Ring 3 v2 / f0f2782),
  // webrtc::CreateAudioDeviceModule(env, kPlatformDefaultAudio) now
  // returns an AudioDeviceLinuxPulse instance instead of a dummy ADM.
  // AudioDeviceLinuxPulse has a SequenceChecker thread_checker_ (header
  // audio_device_pulse_linux.h:285-288), and EVERY method on the class
  // calls RTC_DCHECK(thread_checker_.IsCurrent()): the ctor at :51, dtor
  // at :108, AttachAudioBuffer at :130, Init at :154, Terminate at :200,
  // Initialized at :238, InitSpeaker at :243, InitMicrophone at :281,
  // and so on. The SequenceChecker binds on first .IsCurrent() call;
  // every subsequent call must be from the same sequence.
  //
  // Per the header doc comment (line 285): "We can then use
  // RTC_DCHECK_RUN_ON(&worker_thread_checker_) to ensure that other
  // methods are called from the same thread." — the expected thread is
  // the worker thread (the PCF will subsequently invoke Init() and
  // friends from there).
  //
  // Without the marshal: ctor runs on UI thread (PreMainMessageLoopRun)
  // ⇒ thread_checker_ binds UI. Then PCF moves the ADM into the worker
  // thread for Init(). audio_device_pulse_linux.cc:154 fires
  // RTC_DCHECK_FATAL on rv3 (verified empirically). The dummy ADM never
  // hit this code path because its construction is trivial and the
  // dummy ADM has no SequenceChecker.
  //
  // Fix: invoke CreateCloudBrowserDefaultAudioDeviceModule on the
  // worker thread via BlockingCall (thread.h:328 — synchronous-blocking
  // pattern matching the CV2-69 RunOnSignalingSync template, just with
  // libwebrtc's built-in helper instead of a hand-rolled WaitableEvent).
  // The ADM is constructed on worker thread; thread_checker_ binds
  // worker; subsequent PCF Init() on the same worker thread passes.
  // BlockingCall returns the scoped_refptr to UI thread for the PCF
  // construction call — UI thread thread-safety on scoped_refptr move
  // is the same as any cross-thread refptr handoff (well-defined).
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm =
      worker_thread_->BlockingCall(
          [] { return CreateCloudBrowserDefaultAudioDeviceModule(); });
  webrtc::AudioDeviceModule* adm_debug = adm.get();
  pcf_ = CreateCloudBrowserPcf(network_thread_.get(), worker_thread_.get(),
                               signaling_thread_.get(), env, std::move(adm));
  CHECK(pcf_) << "CreateCloudBrowserPcf returned null — the browser-process "
              << "PeerConnectionFactory failed to construct. ChromelessV2 M1 "
              << "requires a non-null PCF for the M2+ pipeline.";

  // 5a. Codec-cap probe log line (CV2-27). Pure-fn output; format is
  //     LOAD-BEARING — M0 R5's assertion #3 swappable probe scrapes
  //     this line from the container log by regex. The probe-strategy
  //     selection rationale (vs. renderer-side getCapabilities or SDP
  //     inspection) is documented on the CV2-27 ticket.
  LOG(INFO) << FormatPcfVideoCodecLogLine(
      pcf_->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO).codecs);

  // 5b. Browser-process video track source (ChromelessV2 M2 R4 —
  //     CV2-39). Peer-adjacent to pcf_: M3 will hand this scoped_refptr
  //     to PeerConnectionFactoryInterface::CreateVideoTrack(...), and
  //     the CbDevToolsManagerDelegate Cb.startFrameSinkCapture handler
  //     reaches it through CloudBrowserContentBrowserClient::Create
  //     DevToolsManagerDelegate (which forwards cb_track_source() at
  //     delegate-construction time).
  //
  //     R3 (CV2-38) ships CreateCloudBrowserFrameSinkVideoTrackSource()
  //     as a factory that internally constructs the CloudBrowserFrame
  //     SinkCapturer with a callback bound to the soon-to-exist track
  //     source. Matches the CreateCloudBrowserPcf precedent above —
  //     embedder hands in the externally-allocated dep (producer mojo
  //     here / threads + ADM there), factory owns the rest.
  //
  //     GetHostFrameSinkManager() lives behind the same patches/0005
  //     visibility patch the delegate originally used; the include
  //     above carries the matching //nogncheck.
  viz::HostFrameSinkManager* manager = content::GetHostFrameSinkManager();
  CHECK(manager) << "HostFrameSinkManager unavailable in PreMainMessageLoopRun "
                 << "step 5b — the cb-chromium worker cannot construct its "
                 << "browser-process video track source without it.";
  mojo::Remote<viz::mojom::FrameSinkVideoCapturer> producer;
  manager->CreateVideoCapturer(producer.BindNewPipeAndPassReceiver());
  CHECK(producer.is_bound())
      << "FrameSinkVideoCapturer producer remote failed to bind during "
      << "browser-process video track source construction.";

  // R3 takes std::unique_ptr<CloudBrowserFrameSinkCapturer>, not the
  // raw mojo::Remote. Wrap the producer with a placeholder callback:
  // CloudBrowserFrameSinkVideoTrackSource immediately rebinds the
  // capturer to its OnCapturerFrame ingress before capture can Start().
  auto capturer = std::make_unique<CloudBrowserFrameSinkCapturer>(
      std::move(producer), base::DoNothing());

  cb_track_source_ =
      webrtc::make_ref_counted<CloudBrowserFrameSinkVideoTrackSource>(
          std::move(capturer));
  CHECK(cb_track_source_)
      << "make_ref_counted<CloudBrowserFrameSinkVideoTrackSource> "
      << "returned null — Cb.startFrameSinkCapture would fail with "
      << "ServerError on every invocation. ChromelessV2 M2 R4 (CV2-39) "
      << "requires a non-null track source for the M3 peer-track wiring.";

  // ============== CV2-69 (M55-R5-merge-with-m3-r4-r6) F5 + F6 ==============
  //
  // Native WebRTC peer wiring — bootstraps the runtime peer that
  // M3 R1-R7 + M2 R3 compile+link toward. Without this block the
  // cb-chromium worker boots, builds PCF + track source, and idles
  // in the main message loop with no peer (the cr7727-6276365
  // baseline behavior, per functional-test verdict 2026-05-17).
  //
  // Subset scope per implementer judgment: ship the offer→ICE→DC
  // handshake path (M3 R2 ws_client + M3 R4 offerer driver + M2 R3
  // video sendonly transceiver to trigger OnRenegotiationNeeded).
  // M3 R7 reconnect + M4/M5/M6 DataChannels + M5.5 audio deferred
  // to a follow-up R# (a single AddDataChannel + observer.Bind call
  // sequence after offerer_driver_->Start(), additive to this base).

  // F5 step 1 — Load env-driven signaling config. Returns nullopt
  // when WEBRTC_SIGNALING_HOST or WEBRTC_SIGNALING_SESSION_ID is
  // unset; in that case skip the signaling subsystem (worker stays
  // a CDP-only target, preserves pre-CV2-69 deployment-without-
  // signaling-envs behavior).
  std::optional<cloud_browser::signaling::WsClientConfig> ws_config =
      cloud_browser::signaling::LoadConfigFromEnv();
  if (!ws_config) {
    LOG(WARNING) << "CV2-69: WEBRTC_SIGNALING_HOST / WEBRTC_SIGNALING_"
                    "SESSION_ID unset — native signaling subsystem "
                    "disabled. Worker runs as CDP-only target. To "
                    "enable, set WEBRTC_SIGNALING_HOST=<host[:port]> "
                    "+ WEBRTC_SIGNALING_SESSION_ID=<cb:elem:attempt> "
                    "+ WEBRTC_SIGNALING_TLS=0 (for plain ws://).";
    return content::RESULT_CODE_NORMAL_EXIT;
  }
  LOG(INFO) << "CV2-69 signaling: dialing host=" << ws_config->host
            << " session=" << ws_config->session_id
            << " tls=" << (ws_config->use_tls ? "wss" : "ws");

  // F5 step 2 — NetworkContext for the WS dial. Standard chromium
  // plumbing: browser_context_->StoragePartition->NetworkContext.
  // Raw pointer; safe for worker lifetime (StoragePartition outlives
  // main_parts; main_parts.PostMainMessageLoopRun tears it down
  // last among the worker subsystems).
  network::mojom::NetworkContext* network_context =
      browser_context_->GetDefaultStoragePartition()->GetNetworkContext();
  CHECK(network_context)
      << "CV2-69: BrowserContext::GetDefaultStoragePartition()->"
         "GetNetworkContext() returned null — chromium storage "
         "partition setup is misconfigured.";

  // F5 step 3 — Construct R2 SignalingWsClient. Observer is `this`
  // (main_parts) — see header comment block on the construction-
  // order rationale. The ctor does NOT dial; Connect() at step 6
  // opens the wire after the offerer driver is ready.
  ws_client_ = std::make_unique<cloud_browser::signaling::SignalingWsClient>(
      network_context, std::move(*ws_config),
      /*observer=*/this);

  // F5 step 4 — Load ICE config (M3 R3). Defaults to a single
  // stun:stun.l.google.com:19302 entry when WEBRTC_ICE_SERVERS is
  // unset (mirrors streamer.js DEFAULT_ICE_SERVERS). Note: renamed
  // from LoadConfigFromEnv to LoadIceConfigFromEnv as part of
  // CV2-69 to disambiguate from the same-namespace function in
  // cb_signaling_ws_client.h.
  std::optional<cloud_browser::signaling::IceConfig> ice_cfg =
      cloud_browser::signaling::LoadIceConfigFromEnv();
  CHECK(ice_cfg)
      << "CV2-69: LoadIceConfigFromEnv() returned nullopt — contract "
         "violation (default-STUN fallback should never miss). Inspect "
         "cb_ice_config.cc for env-parse regression.";
  LOG(INFO)
      << "CV2-69 ICE: " << ice_cfg->summary.stun << " stun, "
      << ice_cfg->summary.turn << " turn, " << ice_cfg->summary.other
      << " other; transport_policy="
      << (ice_cfg->transport_policy ==
                  webrtc::PeerConnectionInterface::IceTransportsType::kRelay
              ? "relay"
              : "all");

  webrtc::PeerConnectionInterface::RTCConfiguration rtc_config;
  rtc_config.servers = std::move(ice_cfg->servers);
  rtc_config.type = ice_cfg->transport_policy;

  audio_lifecycle_ = std::make_unique<audio::CbAudioLifecycle>(
      /*downstream=*/this,
      /*observer=*/nullptr, base::SequencedTaskRunner::GetCurrentDefault(),
      adm_debug);

  // F5 step 5 — Construct R4 CbOffererDriver. observer is the M5.5
  // audio lifecycle, which forwards downstream to main_parts after it
  // has observed ICE/close/failure edges. ui_runner is the sequenced
  // task runner of the embedder's UI thread (this method runs on it).
  // std::make_unique — CbOffererDriver is a plain embedder-owned
  // object (inherits only the 2 non-refcounted observer interfaces;
  // its 3 refcounted webrtc SDP-observer callbacks are delivered via
  // transient adapter objects it constructs internally — see
  // cb_offerer_driver.cc, CV2-69 #176).
  //
  // CV2-69 re-test#3 fix: the driver also takes signaling_thread_ —
  // the libwebrtc signaling thread the PCF was built on (step 5
  // above). PeerConnection proxy methods (CreateOffer / SetLocal /
  // SetRemoteDescription / AddIceCandidate) issued from the driver's
  // posted-task contexts MUST originate on that thread, or the
  // proxy's blocking thread-hop trips chromium's per-task
  // DisallowBaseSyncPrimitives DCHECK and FATALs the worker (the
  // re-test#3 CreateOffer crash). signaling_thread_ is torn down
  // strictly after offerer_driver_ in PostMainMessageLoopRun, so the
  // raw pointer the driver holds stays valid for the driver's life.
  offerer_driver_ = std::make_unique<cloud_browser::signaling::CbOffererDriver>(
      pcf_, signaling_thread_.get(), ws_client_.get(), std::move(rtc_config),
      /*observer=*/audio_lifecycle_.get(),
      base::SequencedTaskRunner::GetCurrentDefault());

  // F5 step 6 — Start the offerer + open the WS dial. Order:
  // offerer_driver_->Start() first creates the PeerConnection
  // (so AddTransceiver in step 7 has a target); ws_client_->Connect()
  // opens the WS dial (so OnConnected eventually fires + the first
  // outbound offer envelope from CreateOffer can be sent).
  offerer_driver_->Start();
  ws_client_->Connect();

  // CV2-83 / cb_dc_host adoption — create and bind the native
  // DataChannels before adding media transceivers. The first media
  // AddTransceiver triggers the offer; the DCs must already exist so
  // the initial SDP carries the complete native channel set.
  if (webrtc::PeerConnectionInterface* pc = offerer_driver_->pc()) {
    dc_host_ = std::make_unique<cloud_browser::signaling::CbDataChannelHost>(
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface>(pc),
        /*host_observer=*/nullptr, signaling_thread_.get());
    const webrtc::RTCError dc_create = dc_host_->CreateOutboundChannels();
    if (!dc_create.ok()) {
      LOG(ERROR) << "CV2-83: CbDataChannelHost channel creation failed: "
                 << dc_create.message();
    } else {
      LOG(INFO) << "CV2-83: CbDataChannelHost created native outbound "
                   "DataChannels";
    }

    input_delegate_ = std::make_unique<CbInputDispatchCompositeDelegate>(
        &active_webcontents_resolver_);
    if (screen_) {
      screen_->SetLastPointerSource(input_delegate_->last_pointer_state());
    }
    input_dispatch_ = std::make_unique<CbInputDispatch>(
        content::GetUIThreadTaskRunner({}), input_delegate_.get());
    dc_host_->BindObserver(cloud_browser::signaling::CbDcLabel::kInput,
                           input_dispatch_.get());
    LOG(INFO) << "CV2-81: \"input\" DC observer = CbInputDispatch "
                 "(composite delegate = R3..R8 typed pipeline)";

    if (aura_ && aura_->cursor_client()) {
      cursor_xy_join_ = std::make_unique<CbCursorXyJoin>(
          aura_->cursor_client(), input_delegate_->mouse_dispatch());
      cursor_xy_join_->Initialize();
      cursor_emit_policy_ = std::make_unique<cursor::EmitPolicy>();
      cursor_envelope_assembler_ =
          std::make_unique<cursor::EnvelopeAssembler>();
      cursor_dc_emitter_ = std::make_unique<cursor::CbCursorDcEmitter>(
          cursor_xy_join_.get(), cursor_emit_policy_.get(),
          cursor_envelope_assembler_.get(), dc_host_.get(),
          content::GetUIThreadTaskRunner({}));
      cursor_dc_emitter_->BindAndStart();
      LOG(INFO) << "CV2-83: CbCursorXyJoin + CbCursorEmitPolicy + "
                   "CbCursorDcEmitter wired to \"cursor\" DataChannel";
    } else {
      LOG(ERROR) << "CV2-83: cannot wire cursor emitter — Aura cursor "
                    "client is missing";
    }

    clipboard_ws_ = std::make_unique<CbClipboardBridgeWsClient>(
        /*label=*/"inbound",
        /*url=*/"off", content::GetIOThreadTaskRunner({}));
    clipboard_relay_ =
        std::make_unique<CbClipboardRelay>(std::move(clipboard_ws_));
    dc_host_->BindObserver(cloud_browser::signaling::CbDcLabel::kClipboard,
                           clipboard_relay_.get());
    LOG(INFO) << "CV2-75: \"clipboard\" DC observer = "
                 "CbClipboardRelay (WS disabled / url=off)";

    file_upload_ws_ = std::make_unique<CbFileUploadBridgeWsClient>(
        /*url=*/"off", content::GetIOThreadTaskRunner({}));
    file_upload_relay_ = std::make_unique<CbFileUploadRelay>(
        std::move(file_upload_ws_), dc_host_.get());
    dc_host_->BindObserver(cloud_browser::signaling::CbDcLabel::kFiles,
                           file_upload_relay_.get());
    LOG(INFO) << "CV2-75: \"files\" DC observer = "
                 "CbFileUploadRelay (WS disabled / url=off, "
                 "outbound dc_host enabled)";
  }

  if (offerer_driver_->pc()) {
    webrtc::PeerConnectionFactoryInterface* pcf = pcf_.get();
    webrtc::PeerConnectionInterface* pc = offerer_driver_->pc();
    SendOnlyAudioTransceiver audio_bindings =
        signaling_thread_->BlockingCall([pcf, pc] {
          return AddSendOnlyAudioTransceiver(pcf, pc, BuildMediaAudioOptions(),
                                             "cb-audio-0");
        });
    if (audio_lifecycle_) {
      audio_lifecycle_->AdoptBindings(std::move(audio_bindings.source),
                                      std::move(audio_bindings.track),
                                      std::move(audio_bindings.transceiver));
    }
  } else {
    LOG(ERROR) << "CV2-82: offerer driver has no PC after Start(); "
                  "sendonly audio transceiver not attached";
  }

  // F6 step 7 — Add the M2 R3 video sendonly transceiver. THIS is
  // what triggers OnRenegotiationNeeded → CreateOffer → first
  // offer envelope onto the wire. Without this mutation, Start()
  // alone leaves the PC idle.
  video_track_ = pcf_->CreateVideoTrack(cb_track_source_, "cb-video-0");
  if (!video_track_) {
    LOG(ERROR) << "CV2-69: pcf_->CreateVideoTrack returned null — "
                  "video transceiver will not be added; "
                  "OnRenegotiationNeeded will not fire; no SDP offer "
                  "will be emitted. Worker stays alive on CDP path.";
  } else {
    webrtc::RtpTransceiverInit video_init;
    video_init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
    auto tx_result =
        offerer_driver_->pc()->AddTransceiver(video_track_, video_init);
    if (!tx_result.ok()) {
      LOG(ERROR) << "CV2-69: AddTransceiver(video, sendonly) failed: "
                 << tx_result.error().message()
                 << " — proceeding without video; OnRenegotiationNeeded "
                    "may not fire and no SDP offer will emit. Worker "
                    "stays alive on CDP path.";
    } else {
      std::vector<webrtc::RtpCodecCapability> video_codec_preferences =
          BuildFirstLightVideoCodecPreferences(
              pcf_->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO)
                  .codecs);
      webrtc::RTCError codec_preference_result =
          tx_result.value()->SetCodecPreferences(video_codec_preferences);
      if (!codec_preference_result.ok()) {
        LOG(ERROR) << "CV2-91: SetCodecPreferences(video) failed: "
                   << codec_preference_result.message()
                   << " — proceeding with libwebrtc default order; VP9 may "
                      "negotiate first and starve decoded-frame first-light.";
      } else {
        LOG(INFO) << "CV2-91: video transceiver codec preferences applied: "
                  << FormatCodecPreferenceNamesForLog(
                         video_codec_preferences);
      }
      LOG(INFO) << "CV2-69: video sendonly transceiver added; awaiting "
                   "OnRenegotiationNeeded → CreateOffer → wire emission.";
    }
  }

  // ============== END CV2-69 / CV2-83 native peer setup ==============

  return content::RESULT_CODE_NORMAL_EXIT;
}

void CloudBrowserBrowserMainParts::WillRunMainMessageLoop(
    std::unique_ptr<base::RunLoop>& run_loop) {
  // Park the quit closure for a future Cb.shutdown CDP command. Today
  // the worker exits on SIGTERM, so this closure is never run; storing
  // it costs nothing and keeps the shutdown path symmetric with
  // content_shell + headless.
  quit_main_message_loop_ = run_loop->QuitClosure();
}

void CloudBrowserBrowserMainParts::PostMainMessageLoopRun() {
  StopDevToolsHttpHandler();

  // ============== CV2-69 TEARDOWN (LIFO) ==============
  //
  // Drop the runtime-wire chain BEFORE pcf_/threads/track_source so
  // their dtors don't walk into already-freed state. Order is the
  // reverse of construction in PreMainMessageLoopRun:
  //   1. F7-skinny DCs (input/cursor/clipboard/files) — release the
  //      embedder's scoped_refptr. The PC holds an internal strong
  //      ref each one until it drops; this just releases OUR ref.
  //   2. video_track_.reset() — releases the AddTransceiver binding
  //      before the PC drops.
  //   3. offerer_driver_->Close("session ended") — fires R6 `bye`
  //      envelope to the broker (if ws is still connected). The
  //      offerer driver's dtor then drops pc_ on reset() below.
  //   4. offerer_driver_.reset() — libwebrtc handles internal PC
  //      teardown.
  //   5. ws_client_->Disconnect() — clean close (code 1000); observer
  //      eventually fires OnClosed(1000, "") via the inner client's
  //      round-trip.
  //   6. ws_client_.reset() — drops the WS state machine.
  // Note: this teardown is observer-callback-safe because main_parts
  // is the SignalingClientObserver; its dtor runs strictly after
  // ws_client_'s dtor (member init reverse order at process exit).

  // ============== CV2-75/CV2-83 TEARDOWN (LIFO, RUNS FIRST) ==============
  //
  // Unbind DC observers + drop consumer state BEFORE dc_host_ drops its
  // DataChannel refs. Same drop-the-observer-before-its-producer
  // discipline as the existing CV2-69 ordering comment.
  //
  // libwebrtc's DataChannel keeps a raw pointer back via
  // RegisterObserver/UnregisterObserver; if we drop the consumer
  // unique_ptr BEFORE calling UnregisterObserver, the next late
  // OnStateChange / OnMessage callback that races libwebrtc's
  // internal teardown lands on freed memory. UnregisterObserver MUST
  // outlive the consumer dtor.
  if (dc_host_) {
    dc_host_->BindObserver(cloud_browser::signaling::CbDcLabel::kFiles,
                           nullptr);
  }
  file_upload_relay_.reset();
  file_upload_ws_.reset();
  if (dc_host_) {
    dc_host_->BindObserver(cloud_browser::signaling::CbDcLabel::kClipboard,
                           nullptr);
  }
  clipboard_relay_.reset();
  clipboard_ws_.reset();
  cursor_dc_emitter_.reset();
  cursor_envelope_assembler_.reset();
  cursor_emit_policy_.reset();
  cursor_xy_join_.reset();
  if (dc_host_) {
    dc_host_->BindObserver(cloud_browser::signaling::CbDcLabel::kInput,
                           nullptr);
  }
  input_dispatch_.reset();
  // CV2-81: input_delegate_ is now CbInputDispatchCompositeDelegate
  // (was CbInputLoggingDelegate). Its dtor drops the six typed-
  // dispatcher unique_ptrs in reverse declaration order. The
  // active_webcontents_resolver_ value member outlives this reset()
  // (destroyed in main_parts field-destruction order), satisfying
  // the "resolver_lifetime > dispatcher_lifetime" contract.
  if (screen_) {
    screen_->SetLastPointerSource(nullptr);
    screen_->SetRootWindow(nullptr);
  }
  input_delegate_.reset();
  dc_host_.reset();
  // ============== END CV2-75/CV2-81/CV2-83 TEARDOWN ==============

  video_track_ = nullptr;
  if (offerer_driver_) {
    if (audio_lifecycle_) {
      audio_lifecycle_->PrepareForTeardown("session ended");
    }
    offerer_driver_->Close("session ended");
    // unique_ptr — reset() drops the driver. The transient SDP-observer
    // adapters are independently refcounted; if libwebrtc still holds
    // one for an in-flight callback, that adapter survives the driver
    // and its posted task is dropped via the driver's invalidated
    // WeakPtr — teardown is callback-safe.
    offerer_driver_.reset();
  }
  audio_lifecycle_.reset();
  if (ws_client_) {
    ws_client_->Disconnect();
    ws_client_.reset();
  }
  // ============== END CV2-69 TEARDOWN ==============

  // ChromelessV2 M1 — drop the PCF + its 3 webrtc::Threads BEFORE
  // browser_context_/initial_web_contents_/aura_ (the M2+ wiring
  // doesn't add raw pointers from PCF→context, so this is purely
  // additive ordering — but the discipline mirrors the aura_.release()
  // rationale at the bottom of this fn and the cc:303-328 aura
  // precedent: drop the longer-lived holder first so its dtors don't
  // walk into already-freed shorter-lived state).
  //
  // ChromelessV2 M2 R4 (CV2-39): drop the video track source FIRST,
  // before pcf_. The track source's broadcaster carries sink
  // registrations the M3 peer tracks installed via libwebrtc's
  // AddOrUpdateSink; tearing pcf_ first would invalidate those
  // weak refs while the broadcaster still expects to deliver
  // pending OnFrame() calls. Same drop-the-consumer-before-its-
  // producer rationale as the pcf_-before-threads ordering below.
  cb_track_source_ = nullptr;

  // pcf_.reset() drops the strong ref the factory holds against the
  // threads + the encoder factory + the ADM; the threads must
  // outlive that drop because the PCF destructor marshals work onto
  // them. Reverse-construction order on the threads after, per
  // webrtc convention.
  pcf_ = nullptr;
  if (signaling_thread_) {
    signaling_thread_->Stop();
    signaling_thread_.reset();
  }
  if (worker_thread_) {
    worker_thread_->Stop();
    worker_thread_.reset();
  }
  if (network_thread_) {
    network_thread_->Stop();
    network_thread_.reset();
  }

  // CV2-ICE — stop the BeginFrame driver BEFORE the WebContents and the
  // (intentionally leaked) aura_ go away. The driver holds a raw
  // ui::Compositor* into aura_ and issues BeginFrames into the frame-sink
  // hierarchy that the renderer's WebContents is part of; tearing those down
  // first would leave the driver ticking into freed/half-torn state. reset()
  // runs Stop() (invalidates the in-flight ack WeakPtr) then frees the driver.
  begin_frame_driver_.reset();

  // Drop the WebContents BEFORE the BrowserContext — the WebContents
  // holds raw pointers into the context's storage partition, so
  // reversing the order trips a CHECK in chromium.
  initial_web_contents_.reset();
  browser_context_.reset();

  // Intentionally LEAK aura_ — see header. The CbDevToolsManagerDelegate
  // owned by content's DevToolsManager singleton holds WebContents that
  // are children of aura_->host()->window(); that singleton is destroyed
  // by AtExitManager AFTER main_parts dies. Calling reset() here would
  // UAF those still-live children when their dtors walk their parent
  // pointer. The release()'d object lives until process exit; OS reclaims
  // memory + closes the X11 connection cleanly.
  std::ignore = aura_.release();

  // Tear down the global Screen last (and only if we created it —
  // observer-attached subsystems may still hold raw pointers, so
  // dropping earlier risks a UAF). SetScreenInstance(nullptr) clears
  // the global before the unique_ptr destructor frees the memory.
  if (screen_) {
    display::Screen::SetScreenInstance(nullptr);
    screen_.reset();
  }
}

void CloudBrowserBrowserMainParts::StartDevToolsHttpHandler() {
  if (devtools_http_handler_started_) {
    return;
  }
  const uint16_t port = ReadRemoteDebuggingPort();
  net::IPAddress address = ReadRemoteDebuggingAddress();
  auto factory = std::make_unique<ConfigurableTCPServerSocketFactory>(
      std::move(address), port);
  // active_port_output_directory + debug_frontend_dir intentionally
  // empty: we rely on the e2e test querying /json/version to discover
  // the port (matches the BUGS-512-style "no DevToolsActivePort file"
  // workflow we use everywhere else).
  content::DevToolsAgentHost::StartRemoteDebuggingServer(
      std::move(factory), browser_context_->GetPath(), base::FilePath());
  devtools_http_handler_started_ = true;
}

void CloudBrowserBrowserMainParts::StopDevToolsHttpHandler() {
  if (!devtools_http_handler_started_) {
    return;
  }
  content::DevToolsAgentHost::StopRemoteDebuggingServer();
  devtools_http_handler_started_ = false;
}

// ============== CV2-69 observer overrides ==============
//
// signaling::SignalingClientObserver — main_parts adapter forwarding
// inbound envelopes to offerer_driver_->OnEnvelope. Resolves the
// SignalingWsClient ↔ CbOffererDriver construction-order cycle
// without requiring a SetObserver() method on either class. See
// the header member-block comment for the full rationale.

void CloudBrowserBrowserMainParts::OnConnected() {
  LOG(INFO) << "CV2-69 ws_client: connected; ready to receive "
               "inbound envelopes (offer/answer/ice/bye).";
  // CV2-69 re-test#4 (Finding A): forward the connected edge to the
  // offerer driver. main_parts is the registered SignalingClient
  // Observer; the driver needs OnConnected to drain any offer that
  // CreateOffer produced before the WS handshake finished (without
  // this, the offer is Send()-attempted into a not-yet-open socket
  // and fails). Same forward pattern as OnEnvelope below.
  if (!offerer_driver_) {
    LOG(WARNING) << "CV2-69 OnConnected: offerer_driver_ not yet "
                    "constructed (unreachable in practice — Connect() "
                    "is called after the driver ctor in step 6).";
    return;
  }
  offerer_driver_->OnConnected();
}

void CloudBrowserBrowserMainParts::OnEnvelope(
    const cloud_browser::signaling::Envelope& envelope) {
  // Forward to the offerer driver. The driver dispatches on
  // envelope.type (offer/answer/ice/bye/request_renegotiate/
  // probe_result) per cb_offerer_driver.cc:162.
  //
  // Pre-driver envelopes (between ws_client_ ctor and offerer_driver_
  // ctor) are LOGged and dropped — but in practice this window is
  // sub-millisecond and the broker doesn't emit anything until the
  // dial completes via Connect() at PreMainMessageLoopRun step 6.
  if (!offerer_driver_) {
    LOG(WARNING) << "CV2-69 OnEnvelope: dropping envelope before "
                    "offerer_driver_ is constructed (this should be "
                    "unreachable in practice; pre-Connect window).";
    return;
  }
  offerer_driver_->OnEnvelope(envelope);
}

void CloudBrowserBrowserMainParts::OnClosed(uint16_t code,
                                            std::string_view reason) {
  // SignalingClientObserver path: WS close (RFC 6455 code + reason).
  LOG(INFO) << "CV2-69 ws_client: closed code=" << code << " reason=" << reason;
}

void CloudBrowserBrowserMainParts::OnError(std::string_view reason) {
  LOG(ERROR) << "CV2-69 ws_client: transport/handshake/codec error: " << reason
             << " — client is half-broken; offerer driver should "
                "Close() and a follow-up R# should add R7 reconnect "
                "supervision.";
}

// signaling::OffererDriverObserver — telemetry-only LOGs for the
// MVP scope. Production-grade lifecycle relay (M5.5 R5 audio chain,
// M6 R1 stats relay) is deferred to a follow-up R#.

void CloudBrowserBrowserMainParts::OnIceConnectionStateChanged(
    webrtc::PeerConnectionInterface::IceConnectionState state) {
  LOG(INFO) << "CV2-69 offerer_driver: ICE connection state -> "
            << static_cast<int>(state);

  // CV2 Gate 6 media-RTP diagnosis: once ICE connects, start polling the
  // outbound-rtp stats so the serial log shows whether the guest encoder
  // is actually pushing RTP into the (now-paired) relay transport. Armed
  // once; the RepeatingTimer is owned by `this` and torn down with it.
  if (!rtp_stats_timer_armed_ &&
      (state ==
           webrtc::PeerConnectionInterface::IceConnectionState::
               kIceConnectionConnected ||
       state ==
           webrtc::PeerConnectionInterface::IceConnectionState::
               kIceConnectionCompleted)) {
    rtp_stats_timer_armed_ = true;
    LOG(INFO) << "CV2-RTP: ICE connected — starting outbound-rtp stats poll";
    PollOutboundRtpStats();  // immediate first sample
    rtp_stats_timer_.Start(
        FROM_HERE, base::Seconds(2),
        base::BindRepeating(
            &CloudBrowserBrowserMainParts::PollOutboundRtpStats,
            base::Unretained(this)));
  }
}

namespace {
// One-shot stats sink: logs the guest's outbound-rtp counters, then
// self-releases (libwebrtc holds a ref across the async GetStats call).
class CbOutboundRtpStatsLogger : public webrtc::RTCStatsCollectorCallback {
 public:
  void OnStatsDelivered(
      const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report)
      override {
    bool any = false;
    for (const webrtc::RTCOutboundRtpStreamStats* s :
         report->GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>()) {
      any = true;
      const std::string kind = s->kind.value_or("?");
      LOG(INFO) << "CV2-RTP outbound[" << kind << "]"
                << " packets_sent=" << s->packets_sent.value_or(0)
                << " bytes_sent=" << s->bytes_sent.value_or(0)
                << " frames_encoded=" << s->frames_encoded.value_or(0)
                << " frames_sent=" << s->frames_sent.value_or(0)
                << " fps=" << s->frames_per_second.value_or(0.0) << " "
                << s->frame_width.value_or(0) << "x"
                << s->frame_height.value_or(0)
                << " target_bitrate=" << s->target_bitrate.value_or(0.0)
                << " qlim=" << s->quality_limitation_reason.value_or("?");
    }
    if (!any) {
      LOG(INFO) << "CV2-RTP outbound: (no outbound-rtp stream stats yet)";
    }
  }
};
}  // namespace

void CloudBrowserBrowserMainParts::PollOutboundRtpStats() {
  if (!offerer_driver_) {
    return;
  }
  // GetStats' callback delivery is async + thread-safe, but the
  // PeerConnection *proxy* dispatch does a blocking thread-hop to the
  // signaling thread. This poll runs on the UI thread (driven by
  // rtp_stats_timer_, armed from OnIceConnectionStateChanged which
  // CbOffererDriver delivers on the UI thread via its ui_runner_),
  // where chromium installs a per-task DisallowBaseSyncPrimitives — so
  // calling pc()->GetStats() directly from here trips the DCHECK and
  // FATALs the worker the instant ICE connects (thread_restrictions.cc:166).
  // Route through the driver, which marshals onto signaling_thread_ with
  // a scoped_refptr capture that keeps the PC alive across the async call.
  auto sink = webrtc::make_ref_counted<CbOutboundRtpStatsLogger>();
  offerer_driver_->PollOutboundStats(sink);
}

void CloudBrowserBrowserMainParts::OnRenegotiationStarted(
    std::string_view trigger) {
  LOG(INFO) << "CV2-69 offerer_driver: renegotiation started, trigger="
            << trigger;
}

void CloudBrowserBrowserMainParts::OnRenegotiationCompleted() {
  LOG(INFO) << "CV2-69 offerer_driver: renegotiation completed";
}

void CloudBrowserBrowserMainParts::OnClosed(std::string_view reason) {
  // OffererDriverObserver path: offerer-driven session-ended event
  // (distinct from the WS-client OnClosed two-arg form above).
  LOG(INFO) << "CV2-69 offerer_driver: session closed, reason=" << reason;
}

void CloudBrowserBrowserMainParts::OnFailed(std::string_view reason) {
  // Unrecoverable failure (e.g. CreateOffer rejected, SDP munging
  // error, transport teardown not surfaced by ws_client OnError).
  // R7 reconnect would handle transport-level failures in a follow-
  // up R#; for CV2-69 we log and leave chromium alive on its CDP
  // path. A future R# may add a Cb.shutdown CDP method here.
  LOG(ERROR) << "CV2-69 offerer_driver: unrecoverable failure, reason="
             << reason;
}
// ============== END CV2-69 observer overrides ==============

}  // namespace cloud_browser

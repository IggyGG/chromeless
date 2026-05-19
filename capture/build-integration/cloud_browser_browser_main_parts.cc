// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserBrowserMainParts — see cloud_browser_browser_main_parts.h.

#include "capture/build-integration/cloud_browser_browser_main_parts.h"

#include <stdint.h>

#include <memory>
#include <string>
#include <utility>

#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/make_ref_counted.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtp_parameters.h"
#include "api/rtp_transceiver_interface.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "capture/build-integration/cb_aura_platform_data.h"
#include "capture/build-integration/cloud_browser_browser_context.h"
#include "capture/build-integration/cloud_browser_pcf.h"
#include "capture/framesink-capturer/capturer.h"
#include "capture/framesink-capturer/cb_framesink_video_track_source.h"
#include "capture/signaling/cb_ice_config.h"      // CV2-69
#include "capture/signaling/cb_offerer_driver.h"  // CV2-69
#include "capture/signaling/cb_signaling_ws_client.h"  // CV2-69
#include "capture/signaling/cb_wire_envelope.h"   // CV2-69
#include "components/viz/host/host_frame_sink_manager.h"
#include "content/public/browser/browser_thread.h"     // CV2-75
#include "content/public/browser/storage_partition.h"  // CV2-69
#include "content/browser/compositor/surface_utils.h"  // nogncheck — same
                                                       // visibility caveat
                                                       // as cb_devtools_agent.cc;
                                                       // patches/0005 unblock
                                                       // applies here too.
#include "mojo/public/cpp/bindings/remote.h"
#include "services/viz/privileged/mojom/compositing/frame_sink_video_capture.mojom.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_socket_factory.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/result_codes.h"
#include "net/base/ip_address.h"
#include "net/base/net_errors.h"
#include "net/log/net_log_source.h"
#include "net/socket/server_socket.h"
#include "net/socket/tcp_server_socket.h"
#include "rtc_base/thread.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/base/page_transition_types.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/display/screen_base.h"
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
class ConfigurableTCPServerSocketFactory : public content::DevToolsSocketFactory {
 public:
  ConfigurableTCPServerSocketFactory(net::IPAddress address, uint16_t port)
      : address_(std::move(address)), port_(port) {}

  ConfigurableTCPServerSocketFactory(const ConfigurableTCPServerSocketFactory&) =
      delete;
  ConfigurableTCPServerSocketFactory& operator=(
      const ConfigurableTCPServerSocketFactory&) = delete;

 private:
  std::unique_ptr<net::ServerSocket> CreateForHttpServer() override {
    auto socket =
        std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
    const std::string address_str = address_.ToString();
    if (socket->ListenWithAddressAndPort(address_str, port_, kBackLog) != net::OK) {
      LOG(ERROR) << "DevTools HTTP listener: failed to bind "
                 << address_str << ":" << port_;
      return nullptr;
    }
    LOG(INFO) << "DevTools HTTP listener bound on "
              << address_str << ":" << port_;
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
  const std::string value =
      cmd.GetSwitchValueASCII("remote-debugging-address");
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
  // A bare ScreenBase with a single 1280x720 display matches the Xvfb
  // resolution the cb-chromium pod brings up and gives chromium's
  // DisplayObservers something to attach to.
  if (!display::Screen::HasScreen()) {
    screen_ = std::make_unique<display::ScreenBase>();
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

  // BUGS-529 diagnostic — confirms the smoking-gun pattern is closed.
  // Pre-fix expectation: HasFocus=false, ViewBounds=0x0.
  // Post-fix expectation: HasFocus=true, ViewBounds=non-zero.
  if (auto* rwhv = initial_web_contents_->GetRenderWidgetHostView()) {
    LOG(INFO) << "CloudBrowserBrowserMainParts: boot WebContents post-Focus "
                 "RWHV bounds=" << rwhv->GetViewBounds().ToString()
              << " hasFocus=" << rwhv->HasFocus()
              << " visibility="
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
  std::ignore = content::DevToolsAgentHost::GetOrCreateFor(
      initial_web_contents_.get());

  // 4. DevTools HTTP listener — bind <--remote-debugging-address>:<--remote-debugging-port>.
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
  pcf_ = CreateCloudBrowserPcf(network_thread_.get(), worker_thread_.get(),
                               signaling_thread_.get(), env,
                               std::move(adm));
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
  // raw mojo::Remote. Wrap the producer + a no-op OnFrameCallback
  // into a capturer first. Note: per R3's docstring at
  // cb_framesink_video_track_source.cc:38-72, R3 cannot rebind the
  // capturer's OnFrameCallback to its own OnCapturerFrame ingress
  // (capturer.h has no SetOnFrameCallback hook today). For the
  // current M2 R1-R4 landing, we pass base::DoNothing as the callback
  // — capture won't actually flow until M2 R5 (CV2-40) re-arch ships
  // either (A) a capturer SetOnFrameCallback hook or (B) a factory
  // that builds capturer+R3 atomically with the right binding. Build
  // structurally complete; M2 R5 is the runtime-correctness gate.
  auto capturer = std::make_unique<CloudBrowserFrameSinkCapturer>(
      std::move(producer),
      base::DoNothing());

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
  LOG(INFO) << "CV2-69 ICE: " << ice_cfg->summary.stun << " stun, "
            << ice_cfg->summary.turn << " turn, "
            << ice_cfg->summary.other << " other; transport_policy="
            << (ice_cfg->transport_policy ==
                        webrtc::PeerConnectionInterface::IceTransportsType::
                            kRelay
                    ? "relay"
                    : "all");

  webrtc::PeerConnectionInterface::RTCConfiguration rtc_config;
  rtc_config.servers = std::move(ice_cfg->servers);
  rtc_config.type = ice_cfg->transport_policy;

  // F5 step 5 — Construct R4 CbOffererDriver. observer=this is
  // OffererDriverObserver; main_parts forwards lifecycle events to
  // LOG sinks (M5.5 R5 audio chain integration deferred). ui_runner
  // is the sequenced task runner of the embedder's UI thread (this
  // method runs on it).
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
  offerer_driver_ =
      std::make_unique<cloud_browser::signaling::CbOffererDriver>(
          pcf_, signaling_thread_.get(), ws_client_.get(),
          std::move(rtc_config),
          /*observer=*/this,
          base::SequencedTaskRunner::GetCurrentDefault());

  // F5 step 6 — Start the offerer + open the WS dial. Order:
  // offerer_driver_->Start() first creates the PeerConnection
  // (so AddTransceiver in step 7 has a target); ws_client_->Connect()
  // opens the WS dial (so OnConnected eventually fires + the first
  // outbound offer envelope from CreateOffer can be sent).
  offerer_driver_->Start();
  ws_client_->Connect();

  // F6 step 7 — Add the M2 R3 video sendonly transceiver. THIS is
  // what triggers OnRenegotiationNeeded → CreateOffer → first
  // offer envelope onto the wire. Without this mutation, Start()
  // alone leaves the PC idle. Note: actual frames don't flow until
  // M2 R5 wires the FrameSinkCapturer's OnFrameCallback to the
  // adapter's OnCapturerFrame ingress; Phase A signaling completes
  // anyway (offer + ICE + DC handshake doesn't require frames).
  video_track_ = pcf_->CreateVideoTrack(cb_track_source_, "cb-video-0");
  if (!video_track_) {
    LOG(ERROR) << "CV2-69: pcf_->CreateVideoTrack returned null — "
                  "video transceiver will not be added; "
                  "OnRenegotiationNeeded will not fire; no SDP offer "
                  "will be emitted. Worker stays alive on CDP path.";
  } else {
    webrtc::RtpTransceiverInit video_init;
    video_init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
    auto tx_result = offerer_driver_->pc()->AddTransceiver(
        video_track_, video_init);
    if (!tx_result.ok()) {
      LOG(ERROR) << "CV2-69: AddTransceiver(video, sendonly) failed: "
                 << tx_result.error().message()
                 << " — proceeding without video; OnRenegotiationNeeded "
                    "may not fire and no SDP offer will emit. Worker "
                    "stays alive on CDP path.";
    } else {
      LOG(INFO) << "CV2-69: video sendonly transceiver added; awaiting "
                   "OnRenegotiationNeeded → CreateOffer → wire emission.";
    }
  }

  // F7-skinny step 8 — Create the four answerer-facing DataChannels.
  // Phase A acceptance per the v3 narrative requires DC handshake
  // (offer → ICE → DC opens). Each CreateDataChannel adds an m=
  // application line to the offer SDP; the answerer-side client
  // (chromeless/client/src/*.ts) listens for each via the canonical
  // label strings:
  //
  //   "input"      — client/src/input.ts wraps "input" channel
  //                  (RTCDataChannel for mouse/key/scroll/IME/touch).
  //   "cursor"     — client/src/cursor.ts subscribes to "cursor"
  //                  (cb_cursor_dc_emitter outbound feed).
  //   "clipboard"  — client/src/clipboard.ts wraps "clipboard"
  //                  (independent from "input"; copy/paste relay).
  //   "files"      — client/src/file-upload.ts wraps "files"
  //                  (NOT "file-upload" — the FILE is file-upload.ts
  //                  but the CHANNEL is "files"; this is Trap #1 in
  //                  the v3 narrative, hard-fixed here at source).
  //
  // The DC handler binding (M4 R1 input dispatch, M5 R6 cursor emit,
  // M6 R2 clipboard relay, M6 R3 file-upload relay) is deferred to a
  // follow-up R# — those classes need to be wired to the DataChannel
  // observer interface and to the active WebContents resolver.
  // Phase A only needs the DCs to OPEN; inbound data dispatch is
  // Phase B/full-bring-up scope.
  //
  // We hold scoped_refptr<DataChannelInterface> for each one to keep
  // the underlying libwebrtc data channel alive past this scope. The
  // PC also holds a strong ref internally, but pin them here so a
  // future handler-binding R# can grab them via main_parts accessors
  // without re-resolving via pc->GetDataChannel.
  if (offerer_driver_->pc()) {
    auto* pc = offerer_driver_->pc();
    webrtc::DataChannelInit dc_init;
    dc_init.ordered = true;
    // Per CreateDataChannelOrError return shape (libwebrtc v118+):
    // returns RTCErrorOr<scoped_refptr<DataChannelInterface>>.
    {
      auto r = pc->CreateDataChannelOrError("input", &dc_init);
      if (r.ok()) {
        input_dc_ = r.MoveValue();
        LOG(INFO) << "CV2-69 DC: created \"input\"";
        // CV2-75 (M4 R1) — bind CbInputDispatch as the DC observer.
        // Delegate = CbInputLoggingDelegate (R1 production stand-in
        // per cb_input_dispatch.h:194); R3+ swaps in a real injector
        // (RWHV / Input.imeSetComposition / touch / drag adapters).
        // CbInputDispatch hops to UI via the injected runner before
        // touching delegate state — thread discipline matches the
        // CV2-69 lessons (no BlockingCall from network thread; no
        // raw-ptr capture-at-construction for capture-lifecycle
        // objects, which CbInputDispatch's delegate isn't).
        input_delegate_ = std::make_unique<CbInputLoggingDelegate>();
        input_dispatch_ = std::make_unique<CbInputDispatch>(
            content::GetUIThreadTaskRunner({}),
            input_delegate_.get());
        input_dc_->RegisterObserver(input_dispatch_.get());
        LOG(INFO) << "CV2-75: \"input\" DC observer = CbInputDispatch "
                     "(R1 logging delegate)";
      } else {
        LOG(ERROR) << "CV2-69 DC \"input\" creation failed: "
                   << r.error().message();
      }
    }
    {
      auto r = pc->CreateDataChannelOrError("cursor", &dc_init);
      if (r.ok()) {
        cursor_dc_ = r.MoveValue();
        LOG(INFO) << "CV2-69 DC: created \"cursor\"";
        // CV2-75: cursor DC observer NOT bound at this layer. The DC
        // emit side is M5 R6 (CbCursorDcEmitter, CV2-24) which takes
        // a `signaling::CbDataChannelHost*` and is therefore deferred
        // along with the cb_dc_host adoption (see follow-up
        // cv2/m3-r5-dc-host-adoption). The aura cursor-client side
        // (M5 R1 / CbCursorClient) is ALREADY wired by
        // CbAuraPlatformData (cb_aura_platform_data.cc:152-153) — not
        // a CV2-75 deliverable.
        // In the interim, inbound frames on "cursor" are dropped by
        // libwebrtc's default (no-observer) path — which is fine for
        // CV2-75 scope because the v1 "cursor" channel is one-way EMIT
        // from browser to portal (no inbound traffic by contract per
        // cb_dc_host.h:46-50).
        LOG(INFO) << "CV2-75: \"cursor\" DC observer DEFERRED to M5 R6 "
                     "(needs cb_dc_host adoption — out of scope here)";
      } else {
        LOG(ERROR) << "CV2-69 DC \"cursor\" creation failed: "
                   << r.error().message();
      }
    }
    {
      auto r = pc->CreateDataChannelOrError("clipboard", &dc_init);
      if (r.ok()) {
        clipboard_dc_ = r.MoveValue();
        LOG(INFO) << "CV2-69 DC: created \"clipboard\"";
        // CV2-75 (M6 R2) — bind CbClipboardRelay as the DC observer.
        // WS client is constructed in `disabled()` mode (url="off")
        // per cb_clipboard_relay.h:207 — the WS production backend
        // has a TODO(M6-R2-ws-backend) and the v1 production-WS
        // choice isn't locked yet. The relay OnMessage path still
        // fires + logs; bridge POST is short-circuited.
        clipboard_ws_ = std::make_unique<CbClipboardBridgeWsClient>(
            /*label=*/"inbound",
            /*url=*/"off",
            content::GetIOThreadTaskRunner({}));
        clipboard_relay_ = std::make_unique<CbClipboardRelay>(
            std::move(clipboard_ws_));
        clipboard_dc_->RegisterObserver(clipboard_relay_.get());
        LOG(INFO) << "CV2-75: \"clipboard\" DC observer = "
                     "CbClipboardRelay (WS disabled / url=off)";
      } else {
        LOG(ERROR) << "CV2-69 DC \"clipboard\" creation failed: "
                   << r.error().message();
      }
    }
    {
      // Trap #1 hard-fix: channel label is "files" (matches
      // client/src/file-upload.ts:3 "Wraps the \"files\" RTCDataChannel"),
      // NOT "file-upload". The emitter file is file-upload.ts; the
      // channel itself is "files".
      auto r = pc->CreateDataChannelOrError("files", &dc_init);
      if (r.ok()) {
        files_dc_ = r.MoveValue();
        LOG(INFO) << "CV2-69 DC: created \"files\" (Trap #1 label-exact)";
        // CV2-75 (M6 R3) — bind CbFileUploadRelay as the DC observer.
        // Same "off"-URL pattern as clipboard above; the WS production
        // backend has a TODO(M6-R3-ws-backend). Observer-binding
        // proves the architectural runtime wire; functional WS path
        // lands in a follow-up.
        file_upload_ws_ = std::make_unique<CbFileUploadBridgeWsClient>(
            /*url=*/"off",
            content::GetIOThreadTaskRunner({}));
        // dc_host=nullptr: CV2-75 doesn't adopt cb_dc_host (see header
        // comment). cb_file_upload_relay.h:362-364 explicitly supports
        // nullptr — the inbound direction (DC → bridge) still works;
        // the outbound (bridge reply → DC) drops frames silently. With
        // WS in "off" mode no replies will arrive anyway, so the
        // dropped-no-host counter stays at 0. cb_dc_host adoption is
        // the cv2/m3-r5-dc-host-adoption follow-up.
        file_upload_relay_ = std::make_unique<CbFileUploadRelay>(
            std::move(file_upload_ws_),
            /*dc_host=*/nullptr);
        files_dc_->RegisterObserver(file_upload_relay_.get());
        LOG(INFO) << "CV2-75: \"files\" DC observer = "
                     "CbFileUploadRelay (WS disabled / url=off, "
                     "outbound dc_host=null — M5R6/cb_dc_host deferred)";
      } else {
        LOG(ERROR) << "CV2-69 DC \"files\" creation failed: "
                   << r.error().message();
      }
    }
  }
  // ============== END CV2-69 F5 + F6 + F7-skinny ==============

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

  // ============== CV2-75 TEARDOWN (LIFO, RUNS FIRST) ==============
  //
  // Unregister DC observers + drop consumer state BEFORE the F7-skinny
  // DC scoped_refptr drops below. Same drop-the-observer-before-its-
  // producer discipline as the existing CV2-69 ordering comment.
  //
  // libwebrtc's DataChannel keeps a raw pointer back via
  // RegisterObserver/UnregisterObserver; if we drop the consumer
  // unique_ptr BEFORE calling UnregisterObserver, the next late
  // OnStateChange / OnMessage callback that races libwebrtc's
  // internal teardown lands on freed memory. UnregisterObserver MUST
  // outlive the consumer dtor.
  if (files_dc_ && file_upload_relay_) {
    files_dc_->UnregisterObserver();
  }
  file_upload_relay_.reset();
  file_upload_ws_.reset();
  if (clipboard_dc_ && clipboard_relay_) {
    clipboard_dc_->UnregisterObserver();
  }
  clipboard_relay_.reset();
  clipboard_ws_.reset();
  // No cursor DC observer registered (M5 R6 deferred); nothing to
  // UnregisterObserver on cursor_dc_. CbCursorClient is owned by
  // CbAuraPlatformData (aura_); its dtor handles
  // SetCursorClient(window, nullptr) + cursor_client_.reset() on
  // aura_'s teardown later in this function.
  if (input_dc_ && input_dispatch_) {
    input_dc_->UnregisterObserver();
  }
  input_dispatch_.reset();
  input_delegate_.reset();
  // ============== END CV2-75 TEARDOWN ==============

  files_dc_ = nullptr;
  clipboard_dc_ = nullptr;
  cursor_dc_ = nullptr;
  input_dc_ = nullptr;
  video_track_ = nullptr;
  if (offerer_driver_) {
    offerer_driver_->Close("session ended");
    // unique_ptr — reset() drops the driver. The transient SDP-observer
    // adapters are independently refcounted; if libwebrtc still holds
    // one for an in-flight callback, that adapter survives the driver
    // and its posted task is dropped via the driver's invalidated
    // WeakPtr — teardown is callback-safe.
    offerer_driver_.reset();
  }
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
  LOG(INFO) << "CV2-69 ws_client: closed code=" << code
            << " reason=" << reason;
}

void CloudBrowserBrowserMainParts::OnError(std::string_view reason) {
  LOG(ERROR) << "CV2-69 ws_client: transport/handshake/codec error: "
             << reason
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
  LOG(INFO) << "CV2-69 offerer_driver: session closed, reason="
            << reason;
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

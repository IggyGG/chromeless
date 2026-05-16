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
#include "api/peer_connection_interface.h"
#include "api/rtp_parameters.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "capture/build-integration/cb_aura_platform_data.h"
#include "capture/build-integration/cloud_browser_browser_context.h"
#include "capture/build-integration/cloud_browser_pcf.h"
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
  //    Construct 3 dedicated rtc::Threads (network / worker /
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
  network_thread_ = rtc::Thread::CreateWithSocketServer();
  worker_thread_ = rtc::Thread::Create();
  signaling_thread_ = rtc::Thread::Create();
  network_thread_->SetName("cb-pcf-net", nullptr);
  worker_thread_->SetName("cb-pcf-worker", nullptr);
  signaling_thread_->SetName("cb-pcf-signaling", nullptr);
  network_thread_->Start();
  worker_thread_->Start();
  signaling_thread_->Start();

  webrtc::Environment env = webrtc::CreateEnvironment();
  pcf_ = CreateCloudBrowserPcf(network_thread_.get(), worker_thread_.get(),
                               signaling_thread_.get(), env,
                               CreateCloudBrowserDefaultAudioDeviceModule());
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

  // ChromelessV2 M1 — drop the PCF + its 3 rtc::Threads BEFORE
  // browser_context_/initial_web_contents_/aura_ (the M2+ wiring
  // doesn't add raw pointers from PCF→context, so this is purely
  // additive ordering — but the discipline mirrors the aura_.release()
  // rationale at the bottom of this fn and the cc:303-328 aura
  // precedent: drop the longer-lived holder first so its dtors don't
  // walk into already-freed shorter-lived state).
  //
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

}  // namespace cloud_browser

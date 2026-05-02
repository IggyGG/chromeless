// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserBrowserMainParts — see cloud_browser_browser_main_parts.h.

#include "capture/build-integration/cloud_browser_browser_main_parts.h"

#include <stdint.h>

#include <memory>
#include <string>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "capture/build-integration/cloud_browser_browser_context.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_socket_factory.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/result_codes.h"
#include "net/base/ip_address.h"
#include "net/base/net_errors.h"
#include "net/log/net_log_source.h"
#include "net/socket/server_socket.h"
#include "net/socket/tcp_server_socket.h"
#include "ui/base/page_transition_types.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/display/screen_base.h"
#include "ui/gfx/geometry/rect.h"
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
  if (!cmd.HasSwitch(::switches::kRemoteDebuggingAddress)) {
    return net::IPAddress::IPv4Localhost();
  }
  const std::string value =
      cmd.GetSwitchValueASCII(::switches::kRemoteDebuggingAddress);
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

  // 2. Initial WebContents on about:blank — this is what hangs off the
  //    BrowserContext and gives DevToolsAgentHost a target to publish
  //    in /json. Without at least one WebContents, /json returns [] and
  //    the e2e test cannot attach to anything.
  content::WebContents::CreateParams create_params(browser_context_.get());
  initial_web_contents_ = content::WebContents::Create(create_params);
  CHECK(initial_web_contents_)
      << "WebContents::Create returned null — chromium browser process "
      << "is misconfigured (renderer host process not yet up?).";

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
  // Drop the WebContents BEFORE the BrowserContext — the WebContents
  // holds raw pointers into the context's storage partition, so
  // reversing the order trips a CHECK in chromium.
  initial_web_contents_.reset();
  browser_context_.reset();

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

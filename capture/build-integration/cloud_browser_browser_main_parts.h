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
//     (canonical reference for a non-aura, server-side embedder)
//   * content/shell/browser/shell_browser_main_parts.{h,cc}
//     (heavier reference; uses Aura — we deliberately don't)
//   * content/shell/browser/shell_devtools_manager_delegate.cc
//     (DevTools HTTP handler bootstrap pattern)

#ifndef CAPTURE_BUILD_INTEGRATION_CLOUD_BROWSER_BROWSER_MAIN_PARTS_H_
#define CAPTURE_BUILD_INTEGRATION_CLOUD_BROWSER_BROWSER_MAIN_PARTS_H_

#include <memory>

#include "base/functional/callback.h"
#include "content/public/browser/browser_main_parts.h"

namespace content {
class WebContents;
}  // namespace content

namespace cloud_browser {

class CloudBrowserBrowserContext;

class CloudBrowserBrowserMainParts : public content::BrowserMainParts {
 public:
  CloudBrowserBrowserMainParts();

  CloudBrowserBrowserMainParts(const CloudBrowserBrowserMainParts&) = delete;
  CloudBrowserBrowserMainParts& operator=(const CloudBrowserBrowserMainParts&) =
      delete;

  ~CloudBrowserBrowserMainParts() override;

  // content::BrowserMainParts:
  int PreMainMessageLoopRun() override;
  void WillRunMainMessageLoop(
      std::unique_ptr<base::RunLoop>& run_loop) override;
  void PostMainMessageLoopRun() override;

 private:
  // Reads --remote-debugging-port (default 0 = ephemeral, loopback)
  // and starts content::DevToolsAgentHost::StartRemoteDebuggingServer
  // bound at 127.0.0.1:<port>. Idempotent — only called once from
  // PreMainMessageLoopRun.
  void StartDevToolsHttpHandler();

  // Symmetric counterpart called from PostMainMessageLoopRun. Calls
  // StopRemoteDebuggingServer iff StartDevToolsHttpHandler ran.
  void StopDevToolsHttpHandler();

  std::unique_ptr<CloudBrowserBrowserContext> browser_context_;
  std::unique_ptr<content::WebContents> initial_web_contents_;

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

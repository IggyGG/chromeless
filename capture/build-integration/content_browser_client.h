// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserContentBrowserClient — content::ContentBrowserClient
// subclass with two narrow overrides:
//
//   1. CreateBrowserMainParts() — returns a CloudBrowserBrowserMainParts
//      so the browser process actually creates a BrowserContext + an
//      initial about:blank WebContents on startup AND binds the
//      DevTools HTTP listener. Without this hook the worker forks
//      cleanly but never opens a TCP socket — see the file-header
//      comment on cloud_browser_browser_main_parts.h for the smoke-test
//      evidence.
//   2. CreateDevToolsManagerDelegate() — returns a
//      CbDevToolsManagerDelegate so the embedder-defined CDP method
//      Cb.startFrameSinkCapture is reachable from Playwright (T55
//      runtime-engagement, see cb_devtools_agent.h).
//
// The video-encoder-factory embedder hook was retired in M7-R6: the
// native worker installs CloudBrowserVideoEncoderFactory directly via
// deps.video_encoder_factory in cloud_browser_pcf.cc, and the Chromium
// base virtual that this override relied on was removed with
// patches/0001 + 0004.
//
// Everything else is the chromium default; we'll grow specific
// overrides only when the worker's behaviour demands them.
//
// Cross-references:
//   * capture/build-integration/cb_devtools_agent.h
//   * docs/internal/encoder-factory-design.md

#ifndef CAPTURE_BUILD_INTEGRATION_CONTENT_BROWSER_CLIENT_H_
#define CAPTURE_BUILD_INTEGRATION_CONTENT_BROWSER_CLIENT_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "content/public/browser/content_browser_client.h"

namespace content {
class BrowserMainParts;
class DevToolsManagerDelegate;
}  // namespace content

namespace cloud_browser {

class CloudBrowserBrowserMainParts;
class CloudBrowserFrameSinkVideoTrackSource;

class CloudBrowserContentBrowserClient : public content::ContentBrowserClient {
 public:
  CloudBrowserContentBrowserClient();

  CloudBrowserContentBrowserClient(const CloudBrowserContentBrowserClient&) =
      delete;
  CloudBrowserContentBrowserClient& operator=(
      const CloudBrowserContentBrowserClient&) = delete;

  ~CloudBrowserContentBrowserClient() override;

  // content::ContentBrowserClient:
  //
  // Constructs a CloudBrowserBrowserMainParts. The base class default
  // returns nullptr, which lets ContentMain spin up the browser
  // process without any embedder-defined startup work — useful for
  // unit tests, fatal for our worker because no BrowserContext + no
  // initial WebContents = no DevTools listener. The
  // |is_integration_test| flag is set for chromium's browser_tests
  // harness; we don't differentiate (the worker behaviour is the same
  // either way).
  std::unique_ptr<content::BrowserMainParts> CreateBrowserMainParts(
      bool is_integration_test) override;

  // Hands chromium our DevToolsManagerDelegate. The base implementation
  // returns nullptr (default chromium behaviour: no embedder-side CDP
  // extensions). We override to wire CbDevToolsManagerDelegate, which
  // adds Cb.startFrameSinkCapture so the e2e test can flip the T55
  // capture path on at runtime. See cb_devtools_agent.h.
  //
  // Threading note: chromium calls CreateBrowserMainParts very early
  // (BrowserMainLoop::Init) and CreateDevToolsManagerDelegate later
  // (lazily, on first DevToolsAgentHost::GetOrCreateFor). By the time
  // the delegate is constructed, BrowserMainParts::PreMainMessageLoopRun
  // has already created the default BrowserContext, so reading
  // |main_parts_->browser_context()| from this hook is safe.
  std::unique_ptr<content::DevToolsManagerDelegate>
  CreateDevToolsManagerDelegate() override;

 private:
  // Stashed by CreateBrowserMainParts so CreateDevToolsManagerDelegate
  // can read the default BrowserContext at delegate-construction time
  // without going through a global singleton. raw_ptr because chromium
  // owns the unique_ptr returned by CreateBrowserMainParts and keeps it
  // alive for the entire process lifetime — same lifetime model as
  // ShellContentBrowserClient::shell_browser_main_parts_.
  raw_ptr<CloudBrowserBrowserMainParts> main_parts_ = nullptr;
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CONTENT_BROWSER_CLIENT_H_

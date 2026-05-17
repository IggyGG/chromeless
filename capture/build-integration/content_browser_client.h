// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserContentBrowserClient — content::ContentBrowserClient
// subclass with three narrow overrides:
//
//   1. CreateBrowserMainParts() — returns a CloudBrowserBrowserMainParts
//      so the browser process actually creates a BrowserContext + an
//      initial about:blank WebContents on startup AND binds the
//      DevTools HTTP listener. Without this hook the worker forks
//      cleanly but never opens a TCP socket — see the file-header
//      comment on cloud_browser_browser_main_parts.h for the smoke-test
//      evidence.
//   2. GetWebRtcVideoEncoderFactory() — installs our
//      CloudBrowserVideoEncoderFactory (T19 / T35 / T36) on libwebrtc
//      via the virtual added by
//      patches/0001-expose-encoder-factory-injection.patch (T49).
//   3. CreateDevToolsManagerDelegate() — returns a
//      CbDevToolsManagerDelegate so the embedder-defined CDP method
//      Cb.startFrameSinkCapture is reachable from Playwright (T55
//      runtime-engagement, see cb_devtools_agent.h).
//
// Everything else is the chromium default; we'll grow specific
// overrides only when the worker's behaviour demands them.
//
// Cross-references:
//   * capture/encoder/encoder_factory.h
//   * capture/build-integration/cb_devtools_agent.h
//   * patches/0001-expose-encoder-factory-injection.patch
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

namespace webrtc {
class VideoEncoderFactory;
}  // namespace webrtc

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

  // Returns a freshly-constructed CloudBrowserVideoEncoderFactory. The
  // base class stores the result in a unique_ptr held by the
  // PeerConnectionFactory wiring on the renderer/browser process — see
  // the patch description for the call site.
  std::unique_ptr<webrtc::VideoEncoderFactory> GetWebRtcVideoEncoderFactory()
      override;

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

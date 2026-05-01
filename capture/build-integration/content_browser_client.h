// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserContentBrowserClient — content::ContentBrowserClient
// subclass with two narrow overrides:
//
//   1. GetWebRtcVideoEncoderFactory() — installs our
//      CloudBrowserVideoEncoderFactory (T19 / T35 / T36) on libwebrtc
//      via the virtual added by
//      patches/0001-expose-encoder-factory-injection.patch (T49).
//   2. CreateDevToolsManagerDelegate() — returns a
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

#include "content/public/browser/content_browser_client.h"

namespace content {
class DevToolsManagerDelegate;
}  // namespace content

namespace webrtc {
class VideoEncoderFactory;
}  // namespace webrtc

namespace cloud_browser {

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
  std::unique_ptr<content::DevToolsManagerDelegate>
  CreateDevToolsManagerDelegate() override;
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CONTENT_BROWSER_CLIENT_H_

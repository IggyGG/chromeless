// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserContentBrowserClient — minimal content::ContentBrowserClient
// subclass whose only job in Phase 2 is to install our
// CloudBrowserVideoEncoderFactory (T19 / T35 / T36) on libwebrtc via the
// virtual added by patches/0001-expose-encoder-factory-injection.patch
// (T49). Everything else is the chromium default; we'll grow specific
// overrides only when the worker's behaviour demands them.
//
// Cross-references:
//   * capture/encoder/encoder_factory.h
//   * patches/0001-expose-encoder-factory-injection.patch
//   * docs/internal/encoder-factory-design.md

#ifndef CAPTURE_BUILD_INTEGRATION_CONTENT_BROWSER_CLIENT_H_
#define CAPTURE_BUILD_INTEGRATION_CONTENT_BROWSER_CLIENT_H_

#include <memory>

#include "content/public/browser/content_browser_client.h"

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
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CONTENT_BROWSER_CLIENT_H_

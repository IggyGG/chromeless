// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserContentBrowserClient — see content_browser_client.h.

#include "capture/build-integration/content_browser_client.h"

#include <memory>
#include <utility>

#include "capture/encoder/encoder_factory.h"

namespace cloud_browser {

CloudBrowserContentBrowserClient::CloudBrowserContentBrowserClient() = default;

CloudBrowserContentBrowserClient::~CloudBrowserContentBrowserClient() = default;

std::unique_ptr<webrtc::VideoEncoderFactory>
CloudBrowserContentBrowserClient::GetWebRtcVideoEncoderFactory() {
  // Phase-2 default config: software path on, all HW prefer flags off.
  // The runtime probes inside nvenc/vaapi/svtav1 encoders are still
  // honoured by CreateVideoEncoder, so the factory degrades gracefully
  // when HW is unavailable. Wiring HW prefer flags to a CLI/env knob
  // is tracked in T63 / T70 / T75 follow-ups; the embedder is
  // intentionally minimal until the worker's launch path is plumbed
  // through to here.
  CloudBrowserVideoEncoderFactory::Config config;
  return std::make_unique<CloudBrowserVideoEncoderFactory>(std::move(config));
}

}  // namespace cloud_browser

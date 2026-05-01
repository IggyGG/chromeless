// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserContentBrowserClient — see content_browser_client.h.

#include "capture/build-integration/content_browser_client.h"

#include <memory>
#include <utility>

#include "capture/build-integration/cb_devtools_agent.h"
#include "capture/build-integration/cloud_browser_browser_main_parts.h"
#include "capture/encoder/encoder_factory.h"
#include "content/public/browser/browser_main_parts.h"
#include "content/public/browser/devtools_manager_delegate.h"

namespace cloud_browser {

CloudBrowserContentBrowserClient::CloudBrowserContentBrowserClient() = default;

CloudBrowserContentBrowserClient::~CloudBrowserContentBrowserClient() = default;

std::unique_ptr<content::BrowserMainParts>
CloudBrowserContentBrowserClient::CreateBrowserMainParts(
    bool /*is_integration_test*/) {
  // ContentMain calls this exactly once on the browser process and
  // owns the returned unique_ptr for the lifetime of the run loop.
  // Returning nullptr (the base default) means chromium runs the
  // browser process to its message-loop without ever creating a
  // BrowserContext, which leaves DevToolsAgentHost with no targets to
  // publish — see cloud_browser_browser_main_parts.h for the reasoning.
  return std::make_unique<CloudBrowserBrowserMainParts>();
}

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

std::unique_ptr<content::DevToolsManagerDelegate>
CloudBrowserContentBrowserClient::CreateDevToolsManagerDelegate() {
  // Per content_browser_client.h:1614 the base implementation returns
  // nullptr (chromium then runs without an embedder delegate, so
  // embedder-defined CDP methods are unreachable). We return our
  // delegate so Cb.startFrameSinkCapture is dispatchable from the
  // remote-debugging endpoint enabled by --remote-debugging-port on
  // launch-chromium-phase2.sh.
  return std::make_unique<CbDevToolsManagerDelegate>();
}

}  // namespace cloud_browser

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserContentBrowserClient — see content_browser_client.h.

#include "capture/build-integration/content_browser_client.h"

#include <memory>
#include <utility>

#include "capture/build-integration/cb_devtools_agent.h"
#include "capture/build-integration/cloud_browser_browser_main_parts.h"
#include "capture/encoder/encoder_factory.h"
#include "content/public/browser/browser_context.h"
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
  auto parts = std::make_unique<CloudBrowserBrowserMainParts>();
  // Stash a raw pointer for CreateDevToolsManagerDelegate so it can
  // read the default BrowserContext when the delegate is constructed
  // (BUGS-529 — wires the default context that previously had to come
  // through Target.createBrowserContext as a workaround).
  main_parts_ = parts.get();
  return parts;
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
  //
  // Pass main_parts_'s BrowserContext as the default so
  // Target.createTarget without an explicit browserContextId succeeds
  // out of the box. main_parts_ is set by CreateBrowserMainParts, which
  // chromium calls before this hook (BrowserMainLoop::Init runs first;
  // CreateDevToolsManagerDelegate is lazy on first GetOrCreateFor in
  // PreMainMessageLoopRun). main_parts_->browser_context() returns the
  // context built in PreMainMessageLoopRun — by the time the FIRST
  // DevToolsAgentHost is created (also from PreMainMessageLoopRun, on
  // the initial about:blank target), the context is already populated.
  //
  // Also pass main_parts_'s Aura root window so each WebContents the
  // delegate creates inherits the embedder's focus chain. Both the
  // browser context AND the aura root are populated by
  // PreMainMessageLoopRun before the first GetOrCreateFor lazily
  // triggers this hook. See CbDevToolsManagerDelegate ctor doc + the
  // BUGS-529 chain in cloud_browser_browser_main_parts.cc.
  content::BrowserContext* default_context =
      main_parts_ ? main_parts_->browser_context() : nullptr;
  aura::Window* aura_context =
      main_parts_ ? main_parts_->aura_root_window() : nullptr;
  return std::make_unique<CbDevToolsManagerDelegate>(default_context,
                                                     aura_context);
}

}  // namespace cloud_browser

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CbDevToolsManagerDelegate — DevToolsManagerDelegate subclass that
// adds a single embedder-defined CDP method:
//
//     Cb.startFrameSinkCapture
//       Constructs a CloudBrowserFrameSinkCapturer (capture/framesink-
//       capturer/capturer.h) targeted at the active tab's compositor
//       frame sink and starts it. The frame callback logs each delivery
//       at INFO so the e2e test (tests/e2e/09-cb-chromium-framesink-
//       capture.spec.ts) can scrape "OnFrameCaptured" lines from the
//       chromium pod log to assert the T55 capture path is engaged at
//       runtime.
//       Response: {"started": true, "frameSinkId": "<n:m>"} on success;
//       a DispatchResponse::ServerError envelope on failure (no
//       WebContents, invalid FrameSinkId, mojo creation failure).
//
// All other CDP methods fall through to chromium's default dispatcher
// via the NotHandledCallback — so the standard Page/Network/Runtime
// domains continue to work alongside our extension.
//
// Why a custom CDP domain? FrameSinkVideoCapturer (T55) is a piece of
// embedder-side wiring that has no chromium-default trigger — there is
// no built-in CDP method that calls it. Without an embedder hook the
// capturer code in capture/framesink-capturer/ compiles in but is
// never *called*. The e2e test needs to drive it from the outside; CDP
// is the lowest-friction way for Playwright to reach into the browser
// process and flip the switch.
//
// Cross-references:
//   * docs/protocol/CDP-extensions.md (TODO when filed)
//   * capture/framesink-capturer/capturer.h    (the consumer we drive)
//   * content/public/browser/devtools_manager_delegate.h
//   * content/shell/browser/shell_devtools_manager_delegate.{h,cc}
//     (template — minimal session-keeping pattern)
//   * headless/lib/browser/headless_devtools_manager_delegate.{h,cc}
//     (template — same shape, more handlers)

#ifndef CAPTURE_BUILD_INTEGRATION_CB_DEVTOOLS_AGENT_H_
#define CAPTURE_BUILD_INTEGRATION_CB_DEVTOOLS_AGENT_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "content/public/browser/devtools_manager_delegate.h"

namespace content {
class DevToolsAgentHostClientChannel;
}  // namespace content

namespace cloud_browser {

class CloudBrowserFrameSinkCapturer;

// Owns at most one active CloudBrowserFrameSinkCapturer at a time.
// Lifetime is tied to the DevToolsManagerDelegate, which content/
// keeps alive for the duration of remote-debugging service.
class CbDevToolsManagerDelegate : public content::DevToolsManagerDelegate {
 public:
  CbDevToolsManagerDelegate();

  CbDevToolsManagerDelegate(const CbDevToolsManagerDelegate&) = delete;
  CbDevToolsManagerDelegate& operator=(const CbDevToolsManagerDelegate&) =
      delete;

  ~CbDevToolsManagerDelegate() override;

  // content::DevToolsManagerDelegate:
  //
  // Routes any message whose Method() starts with "Cb." into our local
  // handler table; falls through to the base class (which calls
  // |callback| with the original message — chromium then dispatches
  // through its own per-domain dispatchers) for everything else.
  void HandleCommand(content::DevToolsAgentHostClientChannel* channel,
                     base::span<const uint8_t> message,
                     NotHandledCallback callback) override;

 private:
  // Implementation of the Cb.startFrameSinkCapture method. Returns the
  // CBOR-encoded response payload that should be wrapped in a
  // CreateResponse() envelope for the caller. On failure, populates
  // |out_error| and returns an empty vector — the caller MUST then
  // emit a CreateErrorResponse instead.
  std::vector<uint8_t> HandleStartFrameSinkCapture(
      content::DevToolsAgentHostClientChannel* channel,
      std::string* out_error);

  // The active capturer instance — at most one. Constructed on first
  // Cb.startFrameSinkCapture; replaced on each subsequent call (the
  // previous instance's Stop() is implicit in destructor; outstanding
  // BufferHandleScopes still call Done() through their own RAII path,
  // see capture/framesink-capturer/capturer.cc::BufferHandleScope).
  std::unique_ptr<CloudBrowserFrameSinkCapturer> active_capturer_;
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CB_DEVTOOLS_AGENT_H_

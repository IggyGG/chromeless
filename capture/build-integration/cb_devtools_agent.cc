// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CbDevToolsManagerDelegate — see cb_devtools_agent.h.
//
// TODO(T17-build-env): exercise this on a Linux box with depot_tools
// against the pinned chromium tree (docs/build/chromium-from-source.md
// §6). Authored against the documented DevToolsManagerDelegate +
// inspector_protocol APIs read out of /content/public/browser/ and
// /third_party/inspector_protocol/crdtp/.
//
// API spelling notes (recorded against chromium 7727 — re-verify on
// each roll):
//   * DevToolsManagerDelegate::HandleCommand takes
//     base::span<const uint8_t> message (CBOR encoded) and a
//     NotHandledCallback that is run with a span if we wish chromium
//     to fall through to its own dispatcher. See
//     content/public/browser/devtools_manager_delegate.h:130-135.
//   * DevToolsAgentHostClientChannel::DispatchProtocolMessageToClient
//     takes std::vector<uint8_t> in CBOR; the channel transcodes to
//     JSON if the underlying client requested non-binary mode. See
//     content/public/browser/devtools_agent_host_client_channel.h.
//   * crdtp::Dispatchable, crdtp::CreateResponse, crdtp::CreateError
//     Response, crdtp::cbor::* are the inspector_protocol primitives
//     used by every embedder dispatcher in upstream (shell + headless).
//
// HostFrameSinkManager access — known caveat:
//   content/browser/compositor/surface_utils.h is intentionally
//   under content/browser/, not content/public/. Embedders typically
//   reach the host via that path with a 'nogncheck' allowance and a
//   build-side dep on a content-internal source_set, which the
//   chromium visibility list rejects for out-of-tree consumers.
//
//   For the Phase-2 cloud-browser worker we own the chromium tree
//   (we apply our own patch series — patches/), so the planned
//   resolution path is to add a small embedder-exposure patch in
//   patches/0005-expose-host-frame-sink-manager.patch that re-
//   exports GetHostFrameSinkManager() through a public header.
//   Until that patch lands the include below is marked nogncheck
//   so gn check doesn't fail; the linker still needs the symbol
//   so the patch is the actual unblocker for autoninja.
//
//   This is intentional shape-only behavior matching patches/README.md
//   §"The patch series is currently shape-only" — first build will
//   surface the symbol-resolution gap; we add the patch then.

#include "capture/build-integration/cb_devtools_agent.h"

#include <string>
#include <utility>

#include "base/threading/thread_restrictions.h"
#include "capture/build-integration/cloud_browser_browser_context.h"

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "capture/framesink-capturer/capturer.h"
#include "components/viz/common/surfaces/frame_sink_id.h"
#include "components/viz/common/surfaces/video_capture_target.h"
#include "components/viz/host/host_frame_sink_manager.h"
#include "content/browser/compositor/surface_utils.h"  // nogncheck — see file
                                                       // header note above.
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client_channel.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "media/base/video_frame.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/viz/privileged/mojom/compositing/frame_sink_video_capture.mojom.h"
#include "third_party/inspector_protocol/crdtp/cbor.h"
#include "third_party/inspector_protocol/crdtp/dispatch.h"
#include "third_party/inspector_protocol/crdtp/serializable.h"
#include "third_party/inspector_protocol/crdtp/span.h"

namespace cloud_browser {

namespace {

// The CDP method we add. Anchored as a span<uint8_t> so we can
// SpanEquals against the Dispatchable's Method() without an extra
// std::string round-trip per command.
constexpr char kStartFrameSinkCaptureMethod[] = "Cb.startFrameSinkCapture";

// Encodes {"started": true, "frameSinkId": "<n:m>"} as a CBOR map
// inside a length-prefixed envelope. Matches the shape every
// auto-generated DomainHandler emits via inspector_protocol.
std::vector<uint8_t> EncodeStartResponse(const std::string& frame_sink_id_str) {
  std::vector<uint8_t> out;
  crdtp::cbor::EnvelopeEncoder envelope;
  envelope.EncodeStart(&out);
  out.push_back(crdtp::cbor::EncodeIndefiniteLengthMapStart());

  // "started": true
  crdtp::cbor::EncodeString8(crdtp::SpanFrom("started"), &out);
  out.push_back(crdtp::cbor::EncodeTrue());

  // "frameSinkId": "<n:m>"
  crdtp::cbor::EncodeString8(crdtp::SpanFrom("frameSinkId"), &out);
  crdtp::cbor::EncodeString8(crdtp::SpanFrom(frame_sink_id_str), &out);

  out.push_back(crdtp::cbor::EncodeStop());
  envelope.EncodeStop(&out);
  return out;
}

// Trampoline that lets us pass the OnFrameCallback as a plain function
// pointer without bouncing through an instance of the agent. The
// frame is dropped at the end of this function (refcount → 0 →
// BufferHandleScope dtor → Done() ack to the producer).
void LogReceivedFrame(scoped_refptr<media::VideoFrame> frame) {
  if (!frame) {
    return;
  }
  // INFO so the e2e test log scrape can assert the line is present.
  // Format intentionally mirrors the test's grep pattern in
  // tests/e2e/09-cb-chromium-framesink-capture.spec.ts —
  //   /CloudBrowserFrameSinkCapturer::OnFrameCaptured.*coded_size=/
  // Keep both in sync if you rename this log line.
  LOG(INFO) << "CloudBrowserFrameSinkCapturer::OnFrameCaptured "
            << "coded_size=" << frame->coded_size().ToString()
            << " timestamp=" << frame->timestamp().InMicroseconds() << "us";
}

}  // namespace

CbDevToolsManagerDelegate::CbDevToolsManagerDelegate() = default;

CbDevToolsManagerDelegate::~CbDevToolsManagerDelegate() = default;

void CbDevToolsManagerDelegate::HandleCommand(
    content::DevToolsAgentHostClientChannel* channel,
    base::span<const uint8_t> message,
    NotHandledCallback callback) {
  // Parse just enough to extract method + call_id without paying for a
  // full UberDispatcher. Dispatchable is intentionally cheap — see
  // third_party/inspector_protocol/crdtp/dispatch.h:91.
  crdtp::Dispatchable dispatchable(crdtp::SpanFrom(message));
  if (!dispatchable.ok()) {
    // Malformed CBOR — let chromium handle the error path. (It will
    // produce a ParseError response.)
    std::move(callback).Run(message);
    return;
  }

  if (!crdtp::SpanEquals(dispatchable.Method(),
                         crdtp::SpanFrom(kStartFrameSinkCaptureMethod))) {
    // Not ours — fall through to chromium's dispatcher.
    std::move(callback).Run(message);
    return;
  }

  // From here on the command is ours; we MUST send a response.
  const int call_id = dispatchable.CallId();

  std::string error;
  std::vector<uint8_t> ok_payload =
      HandleStartFrameSinkCapture(channel, &error);

  std::unique_ptr<crdtp::Serializable> response;
  if (!ok_payload.empty()) {
    response = crdtp::CreateResponse(
        call_id, crdtp::Serializable::From(std::move(ok_payload)));
  } else {
    response = crdtp::CreateErrorResponse(
        call_id, crdtp::DispatchResponse::ServerError(std::move(error)));
  }
  channel->DispatchProtocolMessageToClient(response->Serialize());
}

std::vector<uint8_t> CbDevToolsManagerDelegate::HandleStartFrameSinkCapture(
    content::DevToolsAgentHostClientChannel* channel,
    std::string* out_error) {
  DCHECK(out_error);

  // 1. Resolve the active tab's WebContents from the channel.
  content::DevToolsAgentHost* agent_host = channel->GetAgentHost();
  if (!agent_host) {
    *out_error = "Cb.startFrameSinkCapture: no DevToolsAgentHost on channel";
    return {};
  }
  content::WebContents* web_contents = agent_host->GetWebContents();
  if (!web_contents) {
    *out_error = "Cb.startFrameSinkCapture: no active WebContents";
    return {};
  }

  // 2. Resolve the FrameSinkId via the
  //    WebContents → RWHV → RWH → FrameSinkId chain. Each link can be
  //    null in transient states (renderer crashed, view detached);
  //    handle them all rather than DCHECK.
  content::RenderWidgetHostView* rwhv =
      web_contents->GetRenderWidgetHostView();
  if (!rwhv) {
    *out_error = "Cb.startFrameSinkCapture: no RenderWidgetHostView";
    return {};
  }
  content::RenderWidgetHost* rwh = rwhv->GetRenderWidgetHost();
  if (!rwh) {
    *out_error = "Cb.startFrameSinkCapture: no RenderWidgetHost";
    return {};
  }
  const viz::FrameSinkId frame_sink_id = rwh->GetFrameSinkId();
  if (!frame_sink_id.is_valid()) {
    *out_error = "Cb.startFrameSinkCapture: frame sink id not valid";
    return {};
  }

  // 3. Bind a producer-side mojo::Remote to the host frame sink
  //    manager's freshly-created FrameSinkVideoCapturer. The receiver
  //    is sent off to viz; we keep the Remote and pass it to our
  //    consumer.
  //
  //    GetHostFrameSinkManager() lives in content/browser/compositor/
  //    surface_utils.h — see the file-header note for the visibility
  //    caveat and the planned patch.
  viz::HostFrameSinkManager* manager = content::GetHostFrameSinkManager();
  if (!manager) {
    *out_error = "Cb.startFrameSinkCapture: HostFrameSinkManager unavailable";
    return {};
  }
  mojo::Remote<viz::mojom::FrameSinkVideoCapturer> producer;
  manager->CreateVideoCapturer(producer.BindNewPipeAndPassReceiver());
  if (!producer.is_bound()) {
    *out_error = "Cb.startFrameSinkCapture: producer remote failed to bind";
    return {};
  }

  // 4. Construct the consumer. Replace any previous active capturer —
  //    the e2e test only ever issues one start; in steady state this
  //    keeps the agent stateless from a "max 1 in flight" perspective.
  active_capturer_ = std::make_unique<CloudBrowserFrameSinkCapturer>(
      std::move(producer), base::BindRepeating(&LogReceivedFrame));

  // 5. Start it on the resolved target. The capturer's defaults
  //    (1280x720 NV12 @ 60Hz from capturer.h:136-138) are correct
  //    for the Phase-2 streaming use case — no Configure() override
  //    necessary.
  active_capturer_->Start(viz::VideoCaptureTarget(frame_sink_id));

  LOG(INFO) << "Cb.startFrameSinkCapture: started capture on "
            << frame_sink_id.ToString();

  return EncodeStartResponse(frame_sink_id.ToString());
}


// ============================================================================
// BrowserContext lifecycle
// ============================================================================

content::BrowserContext* CbDevToolsManagerDelegate::CreateBrowserContext() {
  // CloudBrowserBrowserContext's ctor does blocking I/O — creates the
  // profile dir, wires storage-partition state, registers URL loader
  // factories. At startup PreMainMessageLoopRun allows blocking, but
  // this override fires from a CDP handler thread that has
  // tls_blocking_disallowed=1, so an unwrapped construction SIGABRTs
  // on the DCHECK in base/threading/thread_restrictions.cc:62.
  // ScopedAllowBlocking marks the scope as intentionally permissive.
  base::ScopedAllowBlocking allow_blocking;
  auto context = std::make_unique<CloudBrowserBrowserContext>();
  content::BrowserContext* raw = context.get();
  contexts_.push_back(std::move(context));
  LOG(INFO) << "CbDevToolsManagerDelegate: created browser context #"
            << contexts_.size() << " ptr=" << raw;
  return raw;
}

std::vector<content::BrowserContext*>
CbDevToolsManagerDelegate::GetBrowserContexts() {
  std::vector<content::BrowserContext*> out;
  out.reserve(contexts_.size());
  for (const auto& ctx : contexts_) {
    out.push_back(ctx.get());
  }
  return out;
}

content::BrowserContext*
CbDevToolsManagerDelegate::GetDefaultBrowserContext() {
  return default_browser_context_;
}

void CbDevToolsManagerDelegate::DisposeBrowserContext(
    content::BrowserContext* context,
    DisposeCallback callback) {
  // Refuse to dispose the default — main_parts owns it.
  if (context == default_browser_context_) {
    std::move(callback).Run(false,
                            "Default browser context cannot be disposed");
    return;
  }
  for (auto it = contexts_.begin(); it != contexts_.end(); ++it) {
    if (it->get() == context) {
      contexts_.erase(it);  // unique_ptr dtor destroys the context
      std::move(callback).Run(true, "");
      return;
    }
  }
  std::move(callback).Run(false, "Browser context not found");
}

void CbDevToolsManagerDelegate::SetDefaultBrowserContext(
    content::BrowserContext* context) {
  default_browser_context_ = context;
}

}  // namespace cloud_browser

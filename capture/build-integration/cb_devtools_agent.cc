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
//   ChromelessV2 M2 R4 (CV2-39) moved the GetHostFrameSinkManager()
//   call out of this file. It now lives in
//   cloud_browser_browser_main_parts.cc step 5b, which constructs the
//   producer mojo + capturer + CloudBrowserFrameSinkVideoTrackSource
//   at boot time and hands the track source to this delegate via
//   CloudBrowserContentBrowserClient::CreateDevToolsManagerDelegate.
//   The patches/0005-expose-host-frame-sink-manager.patch + the
//   matching //nogncheck escape still apply — they just apply over
//   there now, not here. The shape-only-until-first-build caveat
//   described in patches/README.md §"The patch series is currently
//   shape-only" is unchanged in substance.

#include "capture/build-integration/cb_devtools_agent.h"

#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/threading/thread_restrictions.h"
#include "capture/build-integration/cb_web_contents_delegate.h"
#include "capture/build-integration/cloud_browser_browser_context.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

#include "base/functional/callback.h"
#include "base/json/json_reader.h"   // CV2-WARM — Cb.startNativeSession params
#include "base/logging.h"
#include "base/values.h"             // CV2-WARM — base::Value::Dict params
#include "capture/build-integration/cloud_browser_browser_main_parts.h"  // CV2-WARM — NativeSessionConfig
#include "capture/framesink-capturer/cb_framesink_video_track_source.h"
#include "capture/signaling/cb_ice_config.h"          // CV2-WARM — ICE builders
#include "capture/signaling/cb_signaling_ws_client.h"  // CV2-WARM — WsClientConfig
#include "components/viz/common/surfaces/frame_sink_id.h"
#include "components/viz/common/surfaces/video_capture_target.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client_channel.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "third_party/inspector_protocol/crdtp/cbor.h"
#include "third_party/inspector_protocol/crdtp/dispatch.h"
#include "third_party/inspector_protocol/crdtp/json.h"  // CV2-WARM — ConvertCBORToJSON
#include "third_party/inspector_protocol/crdtp/serializable.h"
#include "third_party/inspector_protocol/crdtp/span.h"
#include "third_party/inspector_protocol/crdtp/status.h"  // CV2-WARM — crdtp::Status
#include "ui/aura/window.h"

namespace cloud_browser {

namespace {

// The CDP method we add. Anchored as a span<uint8_t> so we can
// SpanEquals against the Dispatchable's Method() without an extra
// std::string round-trip per command.
constexpr char kStartFrameSinkCaptureMethod[] = "Cb.startFrameSinkCapture";

// CV2-WARM — bring up the native signaling session at runtime (post warm-
// snapshot restore) with per-session params, instead of the cold-boot env.
constexpr char kStartNativeSessionMethod[] = "Cb.startNativeSession";

// CV2-CAPTURE-STATS — read-only poll of the FrameSink video-frame-production
// counter. The isolator polls this while deciding whether to publish a warm-
// snapshot golden: a HEALTHY renderer has frames_received_from_capturer
// growing (> 0), whereas a wedged RENDERER-STARVED renderer sits flat at 0
// (the FrameSinkVideoCapturer is a pull-consumer — with no BeginFrame/vsync
// the compositor never produces, so the counter never advances). Publishing a
// golden captured from a starved renderer POISONS every cold-start restored
// from it (permanently-frozen 0x0 video), so the gate must be able to tell the
// two apart BEFORE the snapshot is taken. This method exposes exactly that
// counter and nothing else — no side effects, no capture start.
constexpr char kGetCaptureStatsMethod[] = "Cb.getCaptureStats";

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

// CV2-WARM — encodes {"started": true, "sessionId": "<id>"} as a CBOR map
// inside a length-prefixed envelope (same shape as EncodeStartResponse).
std::vector<uint8_t> EncodeStartNativeSessionResponse(
    const std::string& session_id) {
  std::vector<uint8_t> out;
  crdtp::cbor::EnvelopeEncoder envelope;
  envelope.EncodeStart(&out);
  out.push_back(crdtp::cbor::EncodeIndefiniteLengthMapStart());

  crdtp::cbor::EncodeString8(crdtp::SpanFrom("started"), &out);
  out.push_back(crdtp::cbor::EncodeTrue());

  crdtp::cbor::EncodeString8(crdtp::SpanFrom("sessionId"), &out);
  crdtp::cbor::EncodeString8(crdtp::SpanFrom(session_id), &out);

  out.push_back(crdtp::cbor::EncodeStop());
  envelope.EncodeStop(&out);
  return out;
}

// CV2-CAPTURE-STATS — encodes {"framesReceived": <n>} as a CBOR map inside a
// length-prefixed envelope (same shape as EncodeStartResponse). This is the
// FrameSink video-frame-production counter the warm-golden publish gate polls.
//
// Encoder caveat: crdtp's cbor.h (third_party/inspector_protocol/crdtp/cbor.h,
// resolved from the chromium tree — not vendored here) exposes only
// EncodeInt32 for integral CBOR values; there is no unsigned/int64 primitive
// (the only other numeric encoder, EncodeDouble, would surface the count as a
// JSON float on the JSON-transcoding client path). The source counter is a
// uint64_t, but for any realistic warmup window it fits in int32 with room to
// spare (INT32_MAX ≈ 2.1e9 frames = ~800 days at 30fps), so we saturate to
// INT32_MAX before encoding rather than take a lossy wrap. The gate only reads
// this as "0 vs growing", so saturation is immaterial to the decision.
std::vector<uint8_t> EncodeCaptureStatsResponse(uint64_t frames_received) {
  std::vector<uint8_t> out;
  crdtp::cbor::EnvelopeEncoder envelope;
  envelope.EncodeStart(&out);
  out.push_back(crdtp::cbor::EncodeIndefiniteLengthMapStart());

  // "framesReceived": <n>  (saturated to int32 — see caveat above)
  crdtp::cbor::EncodeString8(crdtp::SpanFrom("framesReceived"), &out);
  const int32_t frames_i32 =
      frames_received > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())
          ? std::numeric_limits<int32_t>::max()
          : static_cast<int32_t>(frames_received);
  crdtp::cbor::EncodeInt32(frames_i32, &out);

  out.push_back(crdtp::cbor::EncodeStop());
  envelope.EncodeStop(&out);
  return out;
}

// NOTE (ChromelessV2 M2 R4 — CV2-39): the LogReceivedFrame
// trampoline that previously lived here was the drop-and-log shape
// used to prove the capturer was reachable from a CDP-driven trigger
// (T55 runtime engagement). With R3's CloudBrowserFrameSinkVideoTrack
// Source landing, frames flow into the track source's ingest callback
// → R2 conversion → broadcaster → libwebrtc peer track instead. The
// trampoline is physically removed from the binary; M0-R3's CI gate
// asserts `nm` / grep cannot find LogReceivedFrame in the linked
// cloud_browser_worker (load-bearing physical-absence assertion;
// matches the M0 contract for "no streamer-side leftovers").

}  // namespace

CbDevToolsManagerDelegate::CbDevToolsManagerDelegate(
    content::BrowserContext* default_browser_context,
    aura::Window* aura_context_window,
    base::RepeatingCallback<CloudBrowserFrameSinkVideoTrackSource*()>
        track_source_getter,
    base::RepeatingCallback<void(content::WebContents*, viz::FrameSinkId)>
        active_capture_callback,
    base::RepeatingCallback<webrtc::RTCError(const NativeSessionConfig&)>
        start_native_session_callback)
    : track_source_getter_(std::move(track_source_getter)),
      active_capture_callback_(std::move(active_capture_callback)),
      start_native_session_callback_(std::move(start_native_session_callback)),
      default_browser_context_(default_browser_context),
      aura_context_window_(aura_context_window) {
  // NOTE: we deliberately do NOT Run() the getter here. CV2-69
  // close-out: this ctor fires from PreMainMessageLoopRun step 3
  // (DevToolsAgentHost::GetOrCreateFor, cloud_browser_browser_main_
  // parts.cc:320) which is BEFORE step 5b (main_parts.cc:414)
  // constructs cb_track_source_ — resolving now would always yield
  // nullptr (the original ServerError bug). Resolution is deferred to
  // HandleStartFrameSinkCapture (CDP dispatch time), by which point
  // cb_track_source_ is populated. We only report whether a getter
  // was wired at all.
  if (track_source_getter_) {
    LOG(INFO) << "CbDevToolsManagerDelegate: constructed with a lazy track-"
                 "source getter — Cb.startFrameSinkCapture resolves the "
                 "browser-process video track source at dispatch time "
                 "(ChromelessV2 M2 R4; CV2-69 construction-order fix).";
  } else {
    LOG(WARNING) << "CbDevToolsManagerDelegate: no track-source getter "
                    "supplied — Cb.startFrameSinkCapture will fail with a "
                    "ServerError. Check CloudBrowserContentBrowserClient::"
                    "CreateDevToolsManagerDelegate wiring against "
                    "CloudBrowserBrowserMainParts::cb_track_source().";
  }
  if (default_browser_context_) {
    LOG(INFO) << "CbDevToolsManagerDelegate: constructed with default "
                 "browser context ptr="
              << default_browser_context_.get();
  } else {
    LOG(WARNING) << "CbDevToolsManagerDelegate: constructed with no default "
                    "browser context — Target.createTarget without an "
                    "explicit browserContextId will fail until "
                    "Target.createBrowserContext has been called.";
  }
  if (aura_context_window_) {
    LOG(INFO) << "CbDevToolsManagerDelegate: aura context window ptr="
              << aura_context_window_.get()
              << " — created targets will parent into the embedder's "
                 "Aura focus chain (BUGS-529 closeout).";
  } else {
    LOG(WARNING) << "CbDevToolsManagerDelegate: no aura context window "
                    "supplied — created targets will exhibit BUGS-529's "
                    "Input.dispatch* drop symptom; check "
                    "CloudBrowserContentBrowserClient::CreateDevToolsManager"
                    "Delegate wiring.";
  }
}

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

  const bool is_frame_sink_capture = crdtp::SpanEquals(
      dispatchable.Method(), crdtp::SpanFrom(kStartFrameSinkCaptureMethod));
  const bool is_native_session = crdtp::SpanEquals(
      dispatchable.Method(), crdtp::SpanFrom(kStartNativeSessionMethod));
  const bool is_get_capture_stats = crdtp::SpanEquals(
      dispatchable.Method(), crdtp::SpanFrom(kGetCaptureStatsMethod));
  if (!is_frame_sink_capture && !is_native_session && !is_get_capture_stats) {
    // Not ours — fall through to chromium's dispatcher.
    std::move(callback).Run(message);
    return;
  }

  // From here on the command is ours; we MUST send a response.
  const int call_id = dispatchable.CallId();

  std::string error;
  std::vector<uint8_t> ok_payload;
  if (is_frame_sink_capture) {
    ok_payload = HandleStartFrameSinkCapture(channel, &error);
  } else if (is_native_session) {
    ok_payload = HandleStartNativeSession(dispatchable, &error);
  } else {  // is_get_capture_stats
    ok_payload = HandleGetCaptureStats(channel, &error);
  }

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

  // 0. LAZILY resolve the browser-process video track source
  //    (ChromelessV2 M2 R4 — CV2-39; CV2-69 construction-order fix).
  //    The delegate ctor fires from PreMainMessageLoopRun step 3, before
  //    step 5b builds cb_track_source_ — so we MUST resolve here at
  //    dispatch time (CDP-invoked, long after PreMainMessageLoopRun
  //    returned), not from a ctor snapshot. main_parts owns the
  //    scoped_refptr; the getter hands us the bare pointer. Null getter
  //    or null result → structured ServerError on the wire, not a UAF
  //    in step 4.
  CloudBrowserFrameSinkVideoTrackSource* track_source =
      track_source_getter_ ? track_source_getter_.Run() : nullptr;
  if (!track_source) {
    *out_error =
        "Cb.startFrameSinkCapture: no CloudBrowserFrameSinkVideoTrackSource "
        "resolved at dispatch (check CloudBrowserBrowserMainParts::"
        "cb_track_source() — it must be non-null by PreMainMessageLoopRun "
        "step 5b; getter wired via CloudBrowserContentBrowserClient::"
        "CreateDevToolsManagerDelegate)";
    return {};
  }

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

  // CV2-ICE-v2/v3: force the captured view SHOWING. This is the decisive fix for
  // sustained capture fps, isolated by the CbBeginFrameDriver self-diagnostic
  // on live firecracker (2026-06-16): with the external BeginFrame driver
  // running at 28-30fps and capture correctly targeting this FrameSinkId, the
  // VERDICT was BURSTING at ~0.6fps with rwhv=HIDDEN — even though
  // WebContents::GetVisibility() reported VISIBLE. The renderer's cc::Scheduler
  // gates client_needs_begin_frame_ on the WIDGET/RWHV visibility, NOT the
  // WebContents visibility: while RenderWidgetHostView::IsShowing() is false the
  // renderer treats itself as hidden, stops continuous rAF, and only subscribes
  // to our external BeginFrameSource intermittently (the ~0.6fps burst).
  //
  // CRITICAL (branch-heads/7727 render_widget_host_view_aura.cc):
  // RenderWidgetHostViewAura::IsShowing() returns window_->IsVisible(), which is
  // HIERARCHY-based (true only if the window AND every aura ancestor up to the
  // WindowTreeHost root are Show()n). RWHV::Show() calls window_->Show() on the
  // RWHV's OWN window, but if any ANCESTOR (the WebContentsViewAura content
  // window, or an intermediate) is hidden, IsVisible() — and thus IsShowing() —
  // stays false and the renderer stays throttled. WebContents::WasShown() + the
  // boot native-view Show() did not cover the full chain in this offscreen
  // setup. So we walk the captured view's aura window to the root and Show()
  // every hidden ancestor, then Show() the RWHV — guaranteeing IsVisible() flips
  // true. Each Show() is idempotent (no-op on an already-visible window); the
  // walk honors the Show()/Hide()-in-pairs contract by only Show()ing windows
  // that are currently hidden. With IsShowing() true the renderer requests
  // continuous BeginFrames and each external tick (with
  // --run-all-compositor-stages-before-draw) yields a fresh frame the capturer
  // delivers.
  if (!rwhv->IsShowing()) {
    int ancestors_shown = 0;
    // On Aura, RenderWidgetHostView::GetNativeView() returns the RWHV's
    // aura::Window (gfx::NativeView is aura::Window* here); we hold it as
    // aura::Window* directly so no extra gfx header dep is needed (ui/aura is
    // already a dep; ui/gfx/native_widget_types.h is not on this target).
    for (aura::Window* w = rwhv->GetNativeView(); w; w = w->parent()) {
      if (!w->IsVisible()) {
        w->Show();
        ++ancestors_shown;
      }
    }
    rwhv->Show();
    LOG(INFO) << "Cb.startFrameSinkCapture: forced captured view SHOWING (was "
                 "HIDDEN); Show()'d "
              << ancestors_shown
              << " hidden aura ancestor window(s) + the RWHV so the renderer "
                 "requests continuous BeginFrames; IsShowing() now "
              << rwhv->IsShowing();
  }

  // 3. Drive the boot-constructed capturer at the resolved target via
  //    R3's pass-through StartCapture(). The producer-mojo + capturer
  //    construction dance that used to live here moved to CloudBrowser
  //    BrowserMainParts step 5b — R3's factory
  //    (CreateCloudBrowserFrameSinkVideoTrackSource) owns it now.
  //    Defaults (1280x720 I420 @ 60Hz from capturer.h:136-141) apply
  //    unless R5's auto-start policy overrides via
  //    track_source->Configure() before we land here.
  //
  // Re-invoking StartCapture with a DIFFERENT FrameSinkId (a new
  // WebContents, or the same WebContents after a cross-document
  // navigation swapped its RenderWidgetHost) RE-TARGETS the running
  // capturer: CloudBrowserFrameSinkCapturer::Start's already-started
  // branch (capturer.cc:138-164) re-applies the pinned format/resolution
  // and calls producer_->ChangeTarget(new_fsid). It is a no-op ONLY when
  // the target is unchanged. So this call is safe to repeat and is the
  // mechanism CloudBrowserBrowserMainParts::RearmCaptureAfterRvhSwap uses
  // to self-heal capture after a nav's RWH swap (CV2-CAPTURE-REARM).
  track_source->StartCapture(viz::VideoCaptureTarget(frame_sink_id));

  web_contents->Focus();

  // CV2-95: tell the active-WebContents resolver which WebContents is now
  // being captured. This is the call site cb_active_webcontents_resolver.h
  // names as "the single authority that calls SetActiveCapture()" — and
  // which, prior to this fix, did not exist ANYWHERE in the tree. Without
  // it the resolver's active_ WebContents stays null, so the M4 input
  // dispatchers (R3 mouse … R8 clipboard) all hit their
  // GetActiveWebContents()==nullptr "dropped — no active WebContents"
  // branch and silently discard every event. The capture-start path is
  // exactly where the resolver expects to be told (capture-selection is
  // M2's call; the resolver is told the answer). The callback routes
  // through CloudBrowserBrowserMainParts::SetActiveCapture (same
  // Unretained(main_parts_) lifetime contract as track_source_getter_);
  // an absent callback simply skips this — capture still runs.
  if (active_capture_callback_) {
    active_capture_callback_.Run(web_contents, frame_sink_id);
  } else {
    LOG(WARNING) << "Cb.startFrameSinkCapture: no active-capture callback "
                    "wired — input dispatch will drop events (no active "
                    "WebContents). Check CreateDevToolsManagerDelegate "
                    "against CloudBrowserBrowserMainParts::SetActiveCapture.";
  }

  LOG(INFO) << "Cb.startFrameSinkCapture: track-source pass-through started "
            << "capture on " << frame_sink_id.ToString();

  return EncodeStartResponse(frame_sink_id.ToString());
}

std::vector<uint8_t> CbDevToolsManagerDelegate::HandleGetCaptureStats(
    content::DevToolsAgentHostClientChannel* /*channel*/,
    std::string* out_error) {
  DCHECK(out_error);

  // CV2-CAPTURE-STATS — read-only capture-health probe for the warm-golden
  // publish gate. Resolve the browser-process video track source exactly the
  // way HandleStartFrameSinkCapture does (lazy getter at dispatch time — the
  // ctor fires before step 5b builds cb_track_source_, so a ctor snapshot is
  // always null; CV2-69 construction-order fix). A null getter or null result
  // → structured ServerError on the wire, never a UAF.
  CloudBrowserFrameSinkVideoTrackSource* track_source =
      track_source_getter_ ? track_source_getter_.Run() : nullptr;
  if (!track_source) {
    *out_error =
        "Cb.getCaptureStats: no CloudBrowserFrameSinkVideoTrackSource resolved "
        "at dispatch (check CloudBrowserBrowserMainParts::cb_track_source() — "
        "it must be non-null by PreMainMessageLoopRun step 5b; getter wired via "
        "CloudBrowserContentBrowserClient::CreateDevToolsManagerDelegate)";
    return {};
  }

  // GetStats() is a lock-free plain copy of the monotonic counters
  // (cb_framesink_video_track_source.cc:83-89) — safe to read off the capturer
  // sequence. frames_received_from_capturer is the capturer→ingress delivery
  // count: > 0 and growing ⇒ the renderer is PRODUCING; flat at 0 ⇒ RENDERER-
  // STARVED (do NOT publish a golden from this VM).
  const uint64_t frames = track_source->GetStats().frames_received_from_capturer;

  // Load-bearing marker for the deploy strings-gate: `grep -a CV2-CAPTURE-STATS
  // <binary>` proves the build carries this method. Also a useful liveness
  // breadcrumb in the guest serial log when the isolator polls the gate.
  LOG(INFO) << "CV2-CAPTURE-STATS: Cb.getCaptureStats framesReceived=" << frames;

  return EncodeCaptureStatsResponse(frames);
}

std::vector<uint8_t> CbDevToolsManagerDelegate::HandleStartNativeSession(
    const crdtp::Dispatchable& dispatchable,
    std::string* out_error) {
  DCHECK(out_error);

  // 0. The callback must be wired (BindRepeating to MainParts::
  //    StartNativeSession). A null/empty callback is a wiring bug — surface a
  //    ServerError rather than silently no-op.
  if (!start_native_session_callback_) {
    *out_error =
        "Cb.startNativeSession: no start-native-session callback wired (check "
        "CloudBrowserContentBrowserClient::CreateDevToolsManagerDelegate "
        "against CloudBrowserBrowserMainParts::StartNativeSession)";
    return {};
  }

  // 1. Decode the params. Dispatchable::Params() is a crdtp::span<uint8_t> of
  //    CBOR (the params sub-blob of the command envelope). Convert it to a JSON
  //    string (crdtp::json::ConvertCBORToJSON) and parse it into a dict with
  //    base::JSONReader::ReadDict — robust typed access without a hand-rolled
  //    CBOR map walker. ReadDict parses + extracts the top-level object in one
  //    call (returns nullopt if the JSON isn't an object), so we don't name the
  //    dict type directly (it differs across chromium revs) nor depend on
  //    JSONReader::Read's option-arg defaults. An absent Params() (empty span)
  //    means "no params" → nullopt → fails the required-field check below.
  crdtp::span<uint8_t> params = dispatchable.Params();
  std::string params_json;
  if (params.size() > 0) {
    crdtp::Status status = crdtp::json::ConvertCBORToJSON(params, &params_json);
    if (!status.ok()) {
      *out_error = "Cb.startNativeSession: params CBOR→JSON conversion failed";
      return {};
    }
  }
  // ReadDict(json, options) returns std::optional<Dict> (the dict type,
  // whatever its spelling in this chromium rev) — nullopt on empty input,
  // parse failure, or a non-object top-level. `auto` avoids naming the type.
  // JSON_PARSE_RFC = strict (no comments/trailing commas); our input is
  // machine-generated from CBOR so RFC is exactly right.
  auto dict = base::JSONReader::ReadDict(params_json, base::JSON_PARSE_RFC);
  if (!dict) {
    *out_error =
        "Cb.startNativeSession: missing, malformed, or non-object params "
        "(required: signalingHost, signalingSessionId)";
    return {};
  }

  // 2. Required signaling identity.
  const std::string* signaling_host = dict->FindString("signalingHost");
  const std::string* signaling_session_id =
      dict->FindString("signalingSessionId");
  if (!signaling_host || signaling_host->empty()) {
    *out_error = "Cb.startNativeSession: missing required param signalingHost";
    return {};
  }
  if (!signaling_session_id || signaling_session_id->empty()) {
    *out_error =
        "Cb.startNativeSession: missing required param signalingSessionId";
    return {};
  }

  // 3. Build the WsClientConfig (mirrors LoadConfigFromEnv's output shape).
  signaling::WsClientConfig ws;
  ws.host = *signaling_host;
  ws.session_id = *signaling_session_id;
  if (const std::string* token = dict->FindString("signalingToken")) {
    ws.token = *token;
  }
  // useTls defaults true (matches WsClientConfig::use_tls default), only the
  // explicit false from the isolator forces plain ws://.
  ws.use_tls = dict->FindBool("useTls").value_or(true);

  // 4. Build the IceConfig from the iceServers JSON + transport policy,
  //    reusing the SAME parsers the env path uses so the two paths are
  //    semantically identical. iceServers arrives as the streamer.js JSON
  //    string (the isolator already stores ice_servers_json as a string and
  //    passes it through verbatim); absent/unparsable → default single-STUN.
  signaling::IceConfig ice;
  std::optional<std::vector<webrtc::PeerConnectionInterface::IceServer>>
      servers;
  if (const std::string* ice_servers_json = dict->FindString("iceServers")) {
    servers = signaling::ParseIceServersJson(*ice_servers_json);
  }
  ice.servers =
      servers.has_value() ? std::move(*servers) : signaling::BuildDefaultIceServers();
  if (const std::string* policy = dict->FindString("iceTransportPolicy")) {
    ice.transport_policy = signaling::ParseIceTransportPolicy(*policy);
  }  // else leaves the IceConfig default (kAll).
  ice.summary = signaling::SummariseIceServers(ice.servers);

  // 5. Bring up the session on the UI thread (we are already on it — this is a
  //    posted HandleCommand task). StartNativeSession wraps its blocking hops
  //    in ScopedAllowBaseSyncPrimitives (H1) so the per-task disallow does not
  //    FATAL. The RTCError is mapped to a ServerError on failure (e.g.
  //    INVALID_STATE when a session is already started).
  NativeSessionConfig cfg{std::move(ws), std::move(ice)};
  webrtc::RTCError result = start_native_session_callback_.Run(cfg);
  if (!result.ok()) {
    *out_error =
        std::string("Cb.startNativeSession: ") + result.message();
    return {};
  }

  LOG(INFO) << "Cb.startNativeSession: native session brought up for session="
            << *signaling_session_id << " host=" << *signaling_host;
  return EncodeStartNativeSessionResponse(*signaling_session_id);
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
  base::ScopedAllowBlockingForTesting allow_blocking;
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
      for (auto web_contents_it = web_contents_holders_.begin();
           web_contents_it != web_contents_holders_.end();) {
        if ((*web_contents_it)->GetBrowserContext() == context) {
          web_contents_it = web_contents_holders_.erase(web_contents_it);
        } else {
          ++web_contents_it;
        }
      }
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


// ============================================================================
// Target.createTarget — spawn a WebContents in our delegate-owned context
// ============================================================================

scoped_refptr<content::DevToolsAgentHost>
CbDevToolsManagerDelegate::CreateNewTarget(
    const GURL& url,
    content::DevToolsManagerDelegate::TargetType target_type,
    bool /*new_window*/) {
  // Pick a BrowserContext: most recently created (matches the linear
  // createBrowserContext+createTarget pattern physics emits at session
  // setup), or fall back to default if main_parts has registered one.
  content::BrowserContext* browser_context = nullptr;
  if (!contexts_.empty()) {
    browser_context = contexts_.back().get();
  } else if (default_browser_context_) {
    browser_context = default_browser_context_;
  } else {
    LOG(ERROR) << "CbDevToolsManagerDelegate::CreateNewTarget: no context "
                  "available (no createBrowserContext yet, and the default "
                  "context wasn't passed via the constructor — check "
                  "CloudBrowserContentBrowserClient::"
                  "CreateDevToolsManagerDelegate wiring)";
    return nullptr;
  }

  content::WebContents::CreateParams create_params(browser_context);
  // BUGS-529 closeout — pin the parenting context to the embedder's
  // Aura root so WebContentsViewAura::CreateAuraWindow's
  // ParentWindowWithContext call resolves through our parenting client
  // and parents the new view under the same root the boot WebContents
  // uses. Without this, the WebContents view is not attached to any
  // aura tree and falls outside the focus chain — WebContents::Focus()
  // below would be a silent no-op on Aura, the renderer-side
  // WidgetInputHandler binds in "no focused page" state, and CDP
  // Input.dispatch* are dropped on the floor. See cloud_browser_browser
  // _main_parts.cc PreMainMessageLoopRun step 1a for the fuller chain.
  create_params.context = aura_context_window_;
  auto web_contents = content::WebContents::Create(create_params);
  if (!web_contents) {
    LOG(ERROR) << "CbDevToolsManagerDelegate::CreateNewTarget: WebContents::"
                  "Create returned nullptr";
    return nullptr;
  }

  // Mark the WebContents visible + focused so the renderer-side
  // WidgetInputHandler is wired into the visible/focused page-input
  // pipeline. Without this, CDP-injected Input.dispatch{Mouse,Key}Event
  // is silently no-op'd by the renderer because it treats the page as
  // a hidden background tab. Frame production is unaffected (the
  // streamer page's canvas captureStream + framesink capturer keep
  // the renderer awake), so the failure mode is invisible at the wire
  // layer — exactly the BUGS-529 symptom. Matches HeadlessWebContentsImpl
  // and content_shell's Shell::PlatformSetContents semantics.
  //
  // NOTE: WasShown() + Focus() were necessary but not sufficient on
  // their own — see CreateParams::context above. Both layers (this
  // call + the parenting context) are required for renderer-side
  // input to land. The 9703db5 patch did the WasShown / Focus half;
  // this patch closes the parenting half.
  // Same delegate the boot WebContents gets. Without it, a tab created
  // through CDP would have working input and pixels but no popups, no JS
  // dialogs, no file chooser and no fullscreen — a subtly different
  // browser depending on how the tab was born. Attach before WasShown/
  // Focus so first-script dialogs land correctly.
  web_contents->SetDelegate(GetCloudBrowserWebContentsDelegate());

  web_contents->WasShown();
  web_contents->Focus();

  // BUGS-529 diagnostic — confirms the smoking-gun pattern is closed
  // for delegate-spawned targets too. Both the boot WebContents (in
  // main_parts) and these are children of the same Aura root window;
  // both should report RWHV bounds=non-zero, HasFocus=true post-Focus.
  if (auto* rwhv = web_contents->GetRenderWidgetHostView()) {
    LOG(INFO) << "CbDevToolsManagerDelegate::CreateNewTarget: post-Focus "
                 "RWHV bounds=" << rwhv->GetViewBounds().ToString()
              << " hasFocus=" << rwhv->HasFocus()
              << " visibility=" << static_cast<int>(web_contents->GetVisibility());
  }

  content::NavigationController::LoadURLParams load_params(url);
  load_params.transition_type = ui::PAGE_TRANSITION_TYPED;
  web_contents->GetController().LoadURLWithParams(load_params);

  scoped_refptr<content::DevToolsAgentHost> agent_host =
      target_type == content::DevToolsManagerDelegate::kTab
          ? content::DevToolsAgentHost::GetOrCreateForTab(web_contents.get())
          : content::DevToolsAgentHost::GetOrCreateFor(web_contents.get());

  // Retain the WebContents — DevToolsAgentHost holds only a weak ref.
  web_contents_holders_.push_back(std::move(web_contents));

  LOG(INFO) << "CbDevToolsManagerDelegate::CreateNewTarget: url="
            << url.spec() << " target_id=" << agent_host->GetId()
            << " target_type=" << static_cast<int>(target_type);
  return agent_host;
}

}  // namespace cloud_browser

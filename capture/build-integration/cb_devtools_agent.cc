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

#include <string>
#include <utility>

#include "base/threading/thread_restrictions.h"
#include "capture/build-integration/cloud_browser_browser_context.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

#include "base/functional/callback.h"
#include "base/logging.h"
#include "capture/framesink-capturer/cb_framesink_video_track_source.h"
#include "components/viz/common/surfaces/frame_sink_id.h"
#include "components/viz/common/surfaces/video_capture_target.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client_channel.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "third_party/inspector_protocol/crdtp/cbor.h"
#include "third_party/inspector_protocol/crdtp/dispatch.h"
#include "third_party/inspector_protocol/crdtp/serializable.h"
#include "third_party/inspector_protocol/crdtp/span.h"
#include "ui/aura/window.h"

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
        active_capture_callback)
    : track_source_getter_(std::move(track_source_getter)),
      active_capture_callback_(std::move(active_capture_callback)),
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

  // 3. Drive the boot-constructed capturer at the resolved target via
  //    R3's pass-through StartCapture(). The producer-mojo + capturer
  //    construction dance that used to live here moved to CloudBrowser
  //    BrowserMainParts step 5b — R3's factory
  //    (CreateCloudBrowserFrameSinkVideoTrackSource) owns it now.
  //    Defaults (1280x720 I420 @ 60Hz from capturer.h:136-141) apply
  //    unless R5's auto-start policy overrides via
  //    track_source->Configure() before we land here.
  //
  // TODO(M2-R4-MULTI-TAB): the boot-time one-capturer model means
  // Cb.startFrameSinkCapture invocations for a DIFFERENT WebContents
  // (different FrameSinkId) hit capturer.h:87's idempotent Start
  // no-op path and silently miss the new target. The M0 e2e gate
  // only ever drives one start, so this is fine for the R4 unblock;
  // the multi-tab story belongs to a later R# under CV2-39's
  // "auto-start policy (R5)" non-goal carve-out (which already names
  // R5 as the lifecycle owner).
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

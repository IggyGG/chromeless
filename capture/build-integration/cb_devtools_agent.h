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

#include "api/rtc_error.h"  // CV2-WARM — Cb.startNativeSession callback return
#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "components/viz/common/surfaces/frame_sink_id.h"
#include "content/public/browser/devtools_manager_delegate.h"

namespace aura {
class Window;
}  // namespace aura

namespace content {
class BrowserContext;
class DevToolsAgentHost;
class DevToolsAgentHostClientChannel;
class WebContents;
}  // namespace content

namespace crdtp {
class Dispatchable;  // CV2-WARM — HandleStartNativeSession param (CBOR dispatch)
}  // namespace crdtp

class GURL;

namespace cloud_browser {

class CloudBrowserBrowserContext;
class CloudBrowserFrameSinkVideoTrackSource;
struct NativeSessionConfig;  // CV2-WARM — cloud_browser_browser_main_parts.h

// Routes the Cb.startFrameSinkCapture CDP method into the
// browser-process-owned CloudBrowserFrameSinkVideoTrackSource (held by
// CloudBrowserBrowserMainParts, peer-adjacent to the PeerConnection
// Factory). The delegate no longer owns the capturer — see ChromelessV2
// M2 R4 (CV2-39) for the ownership move.
//
// Lifetime is tied to the DevToolsManagerDelegate, which content/
// keeps alive for the duration of remote-debugging service.
class CbDevToolsManagerDelegate : public content::DevToolsManagerDelegate {
 public:
  // |default_browser_context| is the BrowserContext owned by main_parts;
  // we hold a raw_ptr because main_parts outlives the delegate. May be
  // nullptr in tests / paths where main_parts hasn't created a context
  // yet — Target.createTarget callers in that state must pass an
  // explicit browserContextId via Target.createBrowserContext first.
  //
  // |aura_context_window| is the embedder's Aura root (owned by main_
  // parts via CbAuraPlatformData; intentionally leaked at process exit
  // so this raw pointer never dangles). Each WebContents we create in
  // CreateNewTarget uses this as CreateParams::context so the resulting
  // WebContentsViewAura is parented under the same root the boot tab
  // uses — which is what makes WebContents::Focus() actually move
  // focus and lets the renderer-side WidgetInputHandler treat the page
  // as foreground/focused. Without this, CDP Input.dispatch* are
  // silently dropped by the renderer (BUGS-529 second-layer). nullptr
  // is tolerated for safety; the targets created in that path will
  // exhibit the original BUGS-529 symptom (input drops) but the
  // delegate itself stays functional.
  //
  // |track_source_getter| LAZILY resolves the browser-process-owned
  // video track source (ChromelessV2 M2 R3/R4 — CV2-38/CV2-39) at
  // Cb.startFrameSinkCapture DISPATCH time, NOT at construction.
  //
  // Why lazy (CV2-69 close-out, 2026-05-18): the delegate is built by
  // CloudBrowserContentBrowserClient::CreateDevToolsManagerDelegate,
  // which fires lazily on the FIRST DevToolsAgentHost::GetOrCreateFor —
  // that is PreMainMessageLoopRun *step 3* (cloud_browser_browser_main_
  // parts.cc:320). But cb_track_source_ is not constructed until *step
  // 5b* (main_parts.cc:414), ~4 setup steps later. A raw-pointer
  // snapshot taken in this ctor is therefore ALWAYS nullptr, which is
  // why every CV2-69 functional re-test logged "no video track source
  // supplied" + returned ServerError on Cb.startFrameSinkCapture even
  // though the ctor arg was wired. Resolving through a getter at
  // dispatch time (CDP-invoked, long after PreMainMessageLoopRun has
  // fully returned and cb_track_source_ is populated) is correct
  // regardless of delegate-construction order.
  //
  // The getter is base::BindRepeating(&CloudBrowserBrowserMainParts::
  // cb_track_source, base::Unretained(main_parts_)). main_parts
  // out-lives the delegate's useful window (the getter is only Run()
  // inside an active CDP session — never at teardown), matching the
  // same lifetime assumption the default_browser_context_ /
  // aura_context_window_ raw snapshots already rely on. A null/empty
  // getter, or a getter that returns nullptr, yields a ServerError
  // envelope rather than a UAF.
  // |active_capture_callback| notifies main_parts at Cb.startFrameSink
  // Capture DISPATCH time (CV2-95) after a successful StartCapture,
  // carrying the resolved (web_contents, frame_sink_id). main_parts
  // forwards it to CbActiveWebContentsResolver::SetActiveCapture —
  // closing the gap where the M4 input dispatchers'
  // GetActiveWebContents() always returned nullptr (no call site
  // populated the resolver) and every input event was dropped. Uses the
  // SAME Unretained(main_parts_) lifetime contract as
  // track_source_getter. A null/empty callback simply skips the
  // SetActiveCapture call (capture still starts) rather than UAFing.
  // |start_native_session_callback| (CV2-WARM) brings up the native WebRTC
  // signaling session at Cb.startNativeSession DISPATCH time with per-session
  // params, instead of the cold-boot env path. It is
  // base::BindRepeating(&CloudBrowserBrowserMainParts::StartNativeSession,
  // base::Unretained(main_parts_)) — same Unretained(main_parts_) lifetime
  // contract as track_source_getter_ (main_parts out-lives the delegate's
  // useful window; the callback only runs inside an active CDP session). A
  // null/empty callback yields a ServerError on the wire rather than a UAF.
  explicit CbDevToolsManagerDelegate(
      content::BrowserContext* default_browser_context = nullptr,
      aura::Window* aura_context_window = nullptr,
      base::RepeatingCallback<CloudBrowserFrameSinkVideoTrackSource*()>
          track_source_getter = {},
      base::RepeatingCallback<void(content::WebContents*, viz::FrameSinkId)>
          active_capture_callback = {},
      base::RepeatingCallback<webrtc::RTCError(const NativeSessionConfig&)>
          start_native_session_callback = {});

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

  // Implements Target.createBrowserContext. The default impl in
  // content::DevToolsManagerDelegate returns nullptr — chromium then
  // emits 'Failed to create browser context.' to the caller. We
  // override to spawn a fresh CloudBrowserBrowserContext, retain
  // ownership in |contexts_|, and hand back the raw pointer that
  // chromium uses as the lookup key for subsequent
  // Target.createTarget{browserContextId} calls.
  //
  // Each context is fully isolated — own profile dir, own cookie
  // store, own localStorage. Matches the per-element session model
  // physics uses: every chromeless element gets its own context so
  // cross-element cookie / storage bleed doesn't happen.
  content::BrowserContext* CreateBrowserContext() override;

  // Returns every context this delegate has created via
  // CreateBrowserContext, in insertion order. Excludes the default
  // browser context (owned by main_parts, registered via
  // SetDefaultBrowserContext). Used by chromium's auto-attach
  // bookkeeping to enumerate all contexts that should be torn down
  // on shutdown.
  std::vector<content::BrowserContext*> GetBrowserContexts() override;

  // Default context is the one main_parts creates at startup —
  // owns about:blank tabs, gets exposed as the |targetInfos| in
  // Target.getTargets. Wired in via the constructor from
  // CloudBrowserContentBrowserClient::CreateDevToolsManagerDelegate,
  // which reads CloudBrowserBrowserMainParts::browser_context().
  // SetDefaultBrowserContext() can override post-construction (kept
  // for parity with chromium's interface, currently unused).
  content::BrowserContext* GetDefaultBrowserContext() override;

  // Removes the named context from |contexts_| (which destroys it
  // via unique_ptr) and runs |callback| with success=true. If the
  // context wasn't created by us (i.e. it's the default), runs
  // |callback| with success=false + an error message — main_parts
  // owns the default's lifecycle, the delegate must not destroy it.
  void DisposeBrowserContext(content::BrowserContext* context,
                             DisposeCallback callback) override;

  // Implements Target.createTarget. Default impl returns nullptr —
  // chromium's TargetHandler then emits 'Not supported' to the caller.
  // We override to create a WebContents in the most recently-created
  // BrowserContext (or default if none exist) and wrap it in a
  // DevToolsAgentHost.
  //
  // SINGLE-TENANT NOTE: chromium's content-layer TargetHandler does
  // NOT pass browserContextId to this hook (only the URL + target
  // type), so multi-context routing has to be inferred. We pick the
  // most recently created context as a best-effort proxy. Concurrent
  // callers across multiple physics replicas would race on this; for
  // cb-browserless (replicas=1) this is fine.
  scoped_refptr<content::DevToolsAgentHost> CreateNewTarget(
      const GURL& url,
      content::DevToolsManagerDelegate::TargetType target_type,
      bool new_window) override;

  // Replace the default BrowserContext stored at construction. Stored
  // as a raw_ptr because main_parts owns the lifetime — the delegate
  // outlives the default context only during chromium teardown, and
  // we never deref the pointer past PostMainMessageLoopRun. Currently
  // unused; the canonical path is to pass the context via the
  // constructor (set up by CloudBrowserContentBrowserClient::
  // CreateDevToolsManagerDelegate).
  void SetDefaultBrowserContext(content::BrowserContext* context);

 private:
  // Implementation of the Cb.startFrameSinkCapture method. Returns the
  // CBOR-encoded response payload that should be wrapped in a
  // CreateResponse() envelope for the caller. On failure, populates
  // |out_error| and returns an empty vector — the caller MUST then
  // emit a CreateErrorResponse instead.
  std::vector<uint8_t> HandleStartFrameSinkCapture(
      content::DevToolsAgentHostClientChannel* channel,
      std::string* out_error);

  // CV2-WARM — implementation of Cb.startNativeSession. Parses the per-session
  // signaling params out of |dispatchable|'s CBOR Params(), builds a
  // NativeSessionConfig, and invokes start_native_session_callback_. Returns
  // the CBOR response payload ({"started":true,"sessionId":"..."}) on success;
  // on a missing param / already-started / unwired callback, populates
  // |out_error| and returns an empty vector (caller emits CreateErrorResponse).
  std::vector<uint8_t> HandleStartNativeSession(
      const crdtp::Dispatchable& dispatchable,
      std::string* out_error);

  // Lazy resolver for the browser-process video track source
  // (ChromelessV2 M2 R3/R4). Run() at Cb.startFrameSinkCapture dispatch
  // time — NOT snapshotted at construction (see the ctor doc for the
  // step-3-vs-step-5b construction-order rationale; CV2-69 close-out).
  // An empty callback, or one returning nullptr, is treated as a
  // ServerError on the wire.
  //
  // Ownership move rationale (CV2-39 §"Ownership recommendation"):
  // active_capturer_ used to live here; it now lives inside the track
  // source (along with the ingest callback that feeds the broadcaster),
  // so the delegate is reduced to a thin "resolve FrameSinkId +
  // forward the producer remote" trampoline.
  base::RepeatingCallback<CloudBrowserFrameSinkVideoTrackSource*()>
      track_source_getter_;

  // Notifies main_parts after Cb.startFrameSinkCapture successfully
  // resolves a WebContents + FrameSinkId (CV2-95). This is the M4
  // resolver handoff: input DataChannel dispatchers need the same active
  // capture target that the frame-sink capturer just started using;
  // main_parts forwards to CbActiveWebContentsResolver::SetActiveCapture.
  base::RepeatingCallback<void(content::WebContents*, viz::FrameSinkId)>
      active_capture_callback_;

  // CV2-WARM — brings up the native signaling session at Cb.startNativeSession
  // dispatch time. base::BindRepeating(&CloudBrowserBrowserMainParts::
  // StartNativeSession, base::Unretained(main_parts_)). Same lifetime contract
  // as track_source_getter_. Null/empty → ServerError on the wire.
  base::RepeatingCallback<webrtc::RTCError(const NativeSessionConfig&)>
      start_native_session_callback_;

  // Default context registered by main_parts. NOT owned — main_parts
  // owns the unique_ptr; we hold a raw_ptr for GetDefaultBrowser
  // Context().
  raw_ptr<content::BrowserContext> default_browser_context_ = nullptr;

  // Aura root for parenting WebContents we create. NOT owned —
  // CbAuraPlatformData (held by main_parts) is intentionally leaked
  // at PostMainMessageLoopRun so this raw pointer outlives both
  // main_parts and this delegate. Used in CreateNewTarget to populate
  // CreateParams::context so the new WebContents view is parented into
  // Aura's focus chain. See ctor doc + BUGS-529 chain for the why.
  raw_ptr<aura::Window> aura_context_window_ = nullptr;

  // Contexts created via Target.createBrowserContext, owned by us.
  // unique_ptr because each must be destroyed when DisposeBrowser
  // Context is called; vector preserves insertion order so
  // GetBrowserContexts returns a stable enumeration.
  std::vector<std::unique_ptr<CloudBrowserBrowserContext>> contexts_;

  // WebContents created via CreateNewTarget. DevToolsAgentHost holds
  // only a weak ref to the underlying WebContents — if we don't keep
  // the unique_ptr, the WebContents is destroyed and the host is
  // immediately invalid. Same lifetime model as content_shell's
  // ShellBrowserMainParts::CreateAndShowWebContents pattern.
  std::vector<std::unique_ptr<content::WebContents>> web_contents_holders_;
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CB_DEVTOOLS_AGENT_H_

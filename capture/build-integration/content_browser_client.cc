// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserContentBrowserClient — see content_browser_client.h.

#include "capture/build-integration/content_browser_client.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "capture/build-integration/cb_devtools_agent.h"
#include "capture/build-integration/cloud_browser_browser_main_parts.h"
#include "capture/build-integration/cb_viewport_controller.h"
#include "capture/framesink-capturer/cb_framesink_video_track_source.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_main_parts.h"
#include "content/public/browser/devtools_manager_delegate.h"
// CV2-CERT: the ask-a-human path for a TLS error.
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "capture/build-integration/cb_control_channel.h"
#include "capture/build-integration/cb_login_delegate.h"
#include "capture/build-integration/cb_web_contents_delegate.h"
// CERTIFICATE_REQUEST_RESULT_TYPE_* — its own header, not pulled in by
// content_browser_client.h (verified in the tree, 2026-09-09).
#include "content/public/browser/certificate_request_result_type.h"
// net::SSLInfo::cert is a scoped_refptr<X509Certificate>; subject()/issuer()
// return CertPrincipal, whose GetDisplayName() is in x509_cert_types.h.
#include "net/cert/x509_certificate.h"
#include "net/cert/x509_cert_types.h"
#include "net/ssl/ssl_info.h"
#include "url/gurl.h"

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
  // ChromelessV2 M2 R4 (CV2-39); CV2-69 construction-order fix
  // (2026-05-18). Pass a LAZY getter, NOT a snapshot.
  //
  // This hook fires on the first DevToolsAgentHost::GetOrCreateFor —
  // PreMainMessageLoopRun *step 3* (cloud_browser_browser_main_parts
  // .cc:320) — which is BEFORE *step 5b* (:414) constructs
  // cb_track_source_. A snapshot taken here would therefore ALWAYS be
  // nullptr: that was the prior ServerError bug — every CV2-69
  // functional re-test logged "no video track source supplied" /
  // ServerError on Cb.startFrameSinkCapture even though the ctor arg
  // was wired (the value, not the wiring, was the problem). The
  // delegate Run()s this getter at Cb.startFrameSinkCapture DISPATCH
  // time, by which point PreMainMessageLoopRun has fully returned and
  // cb_track_source_ is populated. base::Unretained is safe here:
  // main_parts out-lives the delegate's useful window (the getter is
  // only Run() during an active CDP session, never at teardown) —
  // the same lifetime assumption default_context / aura_context
  // already rely on. Empty getter (main_parts_ null) → the delegate
  // emits a ServerError envelope rather than UAFing.
  base::RepeatingCallback<CloudBrowserFrameSinkVideoTrackSource*()>
      track_source_getter;
  base::RepeatingCallback<void(content::WebContents*, viz::FrameSinkId)>
      active_capture_callback;
  // CV2-WARM — bring up the native signaling session at Cb.startNativeSession
  // dispatch time (post warm-snapshot restore). Same Unretained(main_parts_)
  // lifetime contract as the two callbacks above.
  base::RepeatingCallback<webrtc::RTCError(const NativeSessionConfig&)>
      start_native_session_callback;
  // Live resize at Cb.setViewport dispatch time. Same contract again.
  base::RepeatingCallback<CbViewportSpec(const CbViewportSpec&)>
      set_viewport_callback;
  // Health counters for Cb.getCaptureStats. Same contract again.
  base::RepeatingCallback<CbSessionHealth()> session_health_getter;
  // OSS-W0 — graceful process exit at Cb.shutdown dispatch time. Same
  // Unretained(main_parts_) lifetime contract as the callbacks above.
  base::RepeatingCallback<bool()> shutdown_callback;
  base::RepeatingCallback<std::vector<std::string>()>
      video_sender_codecs_getter;
  if (main_parts_) {
    track_source_getter = base::BindRepeating(
        &CloudBrowserBrowserMainParts::cb_track_source,
        base::Unretained(main_parts_));
    active_capture_callback = base::BindRepeating(
        &CloudBrowserBrowserMainParts::SetActiveCapture,
        base::Unretained(main_parts_));
    start_native_session_callback = base::BindRepeating(
        &CloudBrowserBrowserMainParts::StartNativeSession,
        base::Unretained(main_parts_));
    set_viewport_callback = base::BindRepeating(
        &CloudBrowserBrowserMainParts::SetViewport,
        base::Unretained(main_parts_));
    session_health_getter = base::BindRepeating(
        &CloudBrowserBrowserMainParts::GetSessionHealth,
        base::Unretained(main_parts_));
    video_sender_codecs_getter = base::BindRepeating(
        &CloudBrowserBrowserMainParts::GetVideoSenderCodecs,
        base::Unretained(main_parts_));
    shutdown_callback =
        base::BindRepeating(&CloudBrowserBrowserMainParts::Shutdown,
                            base::Unretained(main_parts_));
  }
  return std::make_unique<CbDevToolsManagerDelegate>(
      default_context, aura_context, std::move(track_source_getter),
      std::move(active_capture_callback),
      std::move(start_native_session_callback),
      std::move(set_viewport_callback), std::move(session_health_getter),
      std::move(shutdown_callback), std::move(video_sender_codecs_getter));
}

// CV2-CERT — a TLS error, put to the viewer instead of silently cancelled.
//
// chromium's default cancels and the page shows a bare network error, which
// for an unattended worker is correct: nobody is there to judge a
// certificate. For a person driving a browser it is not — a real browser
// offers an interstitial and a choice, and this is the only place the
// embedder can offer one.
//
// CANCEL remains the default on EVERY path that is not an explicit yes: no
// control channel, a closed one, a timeout, a malformed answer, or a viewer
// who declines. Proceeding is opt-in, once, per error.
void CloudBrowserContentBrowserClient::AllowCertificateError(
    content::WebContents* web_contents,
    int cert_error,
    const net::SSLInfo& ssl_info,
    const GURL& request_url,
    bool is_primary_main_frame_request,
    bool strict_enforcement,
    base::OnceCallback<void(content::CertificateRequestResultType)> callback) {
  CbControlChannel* channel =
      GetCloudBrowserWebContentsDelegate()->control_channel();

  // strict_enforcement is HSTS and friends: the site itself has said its
  // certificate must be valid, so there is no legitimate "proceed anyway".
  // Offering the choice would be offering the user a way to be wrong.
  if (!channel || strict_enforcement) {
    std::move(callback).Run(content::CERTIFICATE_REQUEST_RESULT_TYPE_CANCEL);
    return;
  }

  base::DictValue payload;
  payload.Set("url", request_url.possibly_invalid_spec());
  payload.Set("cert_error", cert_error);
  // net error codes are negative ints; the client maps the common ones to
  // readable text and falls back to the number, which is still greppable.
  payload.Set("is_main_frame", is_primary_main_frame_request);
  if (ssl_info.cert) {
    payload.Set("subject", ssl_info.cert->subject().GetDisplayName());
    payload.Set("issuer", ssl_info.cert->issuer().GetDisplayName());
  }
  channel->SendRequest(
      "cert_error", std::move(payload), base::Minutes(2),
      base::BindOnce(
          [](base::OnceCallback<void(content::CertificateRequestResultType)> cb,
             base::DictValue response) {
            // An empty dict is the channel's "no answer" — a closed channel,
            // a timeout, or teardown. Everything but an explicit true
            // cancels.
            const std::optional<bool> proceed = response.FindBool("proceed");
            std::move(cb).Run(
                proceed.value_or(false)
                    ? content::CERTIFICATE_REQUEST_RESULT_TYPE_CONTINUE
                    : content::CERTIFICATE_REQUEST_RESULT_TYPE_CANCEL);
          },
          std::move(callback)));
}

// CV2-LOGIN — hand the 401 to the viewer instead of cancelling it.
//
// Everything this needs is in the delegate; the factory's only job is to
// build one and hand ownership to //content, whose destruction of it IS the
// cancellation signal (see cb_login_delegate.h).
std::unique_ptr<content::LoginDelegate>
CloudBrowserContentBrowserClient::CreateLoginDelegate(
    const net::AuthChallengeInfo& auth_info,
    content::WebContents* /*web_contents*/,
    content::BrowserContext* /*browser_context*/,
    const content::GlobalRequestID& /*request_id*/,
    bool /*is_request_for_primary_main_frame_navigation*/,
    bool /*is_request_for_navigation*/,
    const GURL& url,
    scoped_refptr<net::HttpResponseHeaders> /*response_headers*/,
    bool first_auth_attempt,
    content::GuestPageHolder* /*guest_page_holder*/,
    content::LoginDelegate::LoginAuthRequiredCallback auth_required_callback) {
  return std::make_unique<CbLoginDelegate>(
      auth_info, url, first_auth_attempt, std::move(auth_required_callback));
}

}  // namespace cloud_browser

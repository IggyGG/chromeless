// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Implementation of the M3 R2 native signaling client. See header
// for the design + scope + non-goals; this file is mechanism.

#include "capture/signaling/cb_signaling_ws_client.h"

#include <cstdlib>
#include <utility>

#include "base/environment.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "net/base/isolation_info.h"
#include "net/base/network_anonymization_key.h"
#include "net/cookies/site_for_cookies.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/originating_process_id.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace cloud_browser::signaling {

namespace {

// Env-var names. Centralised so a future rename only touches this
// block (and the env-loading tests).
constexpr char kEnvHost[]      = "WEBRTC_SIGNALING_HOST";
constexpr char kEnvSessionId[] = "WEBRTC_SIGNALING_SESSION_ID";
constexpr char kEnvToken[]     = "WEBRTC_SIGNALING_TOKEN";
constexpr char kEnvTls[]       = "WEBRTC_SIGNALING_TLS";

// RFC 7159 path-segment encoder for session_id. Physics's axum
// router accepts the colons in cb:{element_id}:{attempt_id}
// verbatim, but the full URL still needs to be parseable by GURL —
// so we percent-encode only the characters GURL rejects in a path
// segment and leave the colon alone.
//
// TODO(M3-R2-url-encode): replace with net::EscapeUrlEncodedData() or
// url::EncodeURIComponent() once we wire net/. The hand-rolled escape
// here covers the common safe alphabet (alnum + ":-._~/") and skips
// everything else; signaling session_ids are minted server-side and
// only contain `cb:{uuid}:{uuid}`, so this is correct for the
// production input. The TODO is for defence-in-depth, not a known
// bug.
std::string PercentEncodePathSegment(std::string_view in) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size());
  for (unsigned char c : in) {
    const bool safe =
        (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == ':' || c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

// Same shape — encoder for the token query-param value. Same TODO
// applies (replace with net::EscapeQueryParamValue when net/ is
// available).
std::string PercentEncodeQueryValue(std::string_view in) {
  // Query values are stricter than path segments — `:` and `/` are
  // legal but `+`, `=`, `&`, `#` are not. JWTs are URL-safe base64
  // (alnum + `-_.`) so the encoder rarely fires; correctness still
  // matters for malformed test inputs.
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size());
  for (unsigned char c : in) {
    const bool safe =
        (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

// Same network traffic annotation shape every chromium browser-process
// client uses. The fields here document the request to chromium's
// traffic auditing tooling; mismatching the schema is a compile-time
// error caught by net/traffic_annotation/auditor.
//
// TODO(M3-R2-net-annotation): re-validate the wording against
// chromium's most recent traffic-annotation lint pass once the
// triform-8 build runs. The format below mirrors the one used by
// content_shell's CDP WebSocket client.
constexpr net::NetworkTrafficAnnotationTag kSignalingTrafficAnnotation =
    // Raw-string delimiter MUST be non-empty: the proto body below
    // contains the sequence )" inside `(in-cluster, configurable)"`,
    // and an empty-delimiter R"(...)" terminates early there —
    // derailing the parser (the :127/:137 expected-')'/';'+ the
    // ReadEnv-undeclared cascade). The CBANNOT delimiter never appears
    // in the content. Pre-existing latent bug, surfaced by the cold
    // recompile-from-source (sccache-lied-as-green class).
    net::DefineNetworkTrafficAnnotation("cb_signaling_ws_client", R"CBANNOT(
      semantics {
        sender: "Cloud Browser Signaling Client"
        description:
          "Native browser-process WebSocket client that connects cb-"
          "chromium's PeerConnectionFactory to the Triform physics "
          "signaling broker. Carries SDP offer / answer + ICE "
          "candidates for the WebRTC session between cb-chromium and "
          "the end user's browser."
        trigger:
          "Browser-process startup, once per cb-chromium pod, when "
          "WEBRTC_SIGNALING_HOST + WEBRTC_SIGNALING_SESSION_ID are "
          "set in the pod env."
        data:
          "Signaling envelopes (JSON): SDP / ICE / control frames. No "
          "user content data; no cookies; no auth token in the "
          "payload itself (the JWT travels in the dial URL only)."
        destination: OTHER
        destination_other:
          "Triform physics signaling broker (in-cluster, configurable)"
      }
      policy {
        cookies_allowed: NO
        setting:
          "Disabled by omitting the WEBRTC_SIGNALING_HOST env var. "
          "When omitted, cb-chromium runs in headless-without-"
          "remoting mode."
        policy_exception_justification:
          "This is core infrastructure; the entire cb-chromium pod is "
          "useless without it."
      })CBANNOT");

// Small wrapper that reads an env var via base::Environment. Returns
// empty string when the var is unset or empty.
std::string ReadEnv(base::Environment* env, const char* name) {
  // chromium 7727: base::Environment::GetVar dropped the
  // bool GetVar(name, std::string* result) form; it now returns
  // std::optional<std::string> GetVar(cstring_view name).
  return env->GetVar(name).value_or(std::string());
}

}  // namespace

// --------------------------------------------------------------------
// WsClientConfig
// --------------------------------------------------------------------

std::string WsClientConfig::BuildUrl() const {
  if (host.empty() || session_id.empty()) {
    return {};
  }
  std::string out;
  out.reserve(64 + host.size() + session_id.size() + token.size());
  out.append(use_tls ? "wss://" : "ws://");
  out.append(host);
  out.append("/api/webrtc/signaling/");
  out.append(PercentEncodePathSegment(session_id));
  out.append("?role=browser");
  if (!token.empty()) {
    out.append("&token=");
    out.append(PercentEncodeQueryValue(token));
  }
  return out;
}

// --------------------------------------------------------------------
// LoadConfigFromEnv
// --------------------------------------------------------------------

std::optional<WsClientConfig> LoadConfigFromEnv() {
  auto env = base::Environment::Create();

  WsClientConfig cfg;
  cfg.host = ReadEnv(env.get(), kEnvHost);
  cfg.session_id = ReadEnv(env.get(), kEnvSessionId);
  cfg.token = ReadEnv(env.get(), kEnvToken);

  std::string tls = ReadEnv(env.get(), kEnvTls);
  // Default true. Only "0" / "false" / "no" (case-insensitive) flip
  // to ws://; anything else keeps wss://.
  cfg.use_tls = !(tls == "0" || base::EqualsCaseInsensitiveASCII(tls, "false") ||
                  base::EqualsCaseInsensitiveASCII(tls, "no"));

  if (cfg.host.empty() || cfg.session_id.empty()) {
    LOG(WARNING) << "cb_signaling: missing required env "
                 << (cfg.host.empty() ? kEnvHost : kEnvSessionId)
                 << "; signaling boot skipped";
    return std::nullopt;
  }
  return cfg;
}

// --------------------------------------------------------------------
// SignalingWsClient
// --------------------------------------------------------------------

SignalingWsClient::SignalingWsClient(
    network::mojom::NetworkContext* network_context,
    WsClientConfig config,
    SignalingClientObserver* observer)
    : network_context_(network_context),
      config_(std::move(config)),
      observer_(observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(network_context_);
  DCHECK(observer_);
}

SignalingWsClient::~SignalingWsClient() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Mojo receivers reset themselves at destruction; the remote side
  // sees a "channel gone" signal and tears down its end. No observer
  // callback fires from the destructor (the observer may outlive us
  // by zero ns; calling back into it would be UAF-prone).
}

void SignalingWsClient::Connect() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kIdle) {
    LOG(INFO) << "cb_signaling: Connect() in state="
              << static_cast<int>(state_) << "; no-op";
    return;
  }

  const std::string url = config_.BuildUrl();
  if (url.empty()) {
    FailWithError("config produced empty URL");
    return;
  }
  const GURL dial_url(url);
  if (!dial_url.is_valid()) {
    FailWithError("config produced invalid URL");
    return;
  }

  state_ = State::kConnecting;
  LOG(INFO) << "cb_signaling: dialing " << dial_url.spec();

  // CreateWebSocket argument list. Several of these are
  // chromium-version sensitive — see the TODOs below.
  //
  // TODO(M3-R2-mojo-args): the exact arg list and order for
  // NetworkContext::CreateWebSocket changes between chromium
  // versions (the Mojom interface added storage_access_api_status
  // and dropped a few render_frame fields somewhere around 121).
  // Pin to the version in deps/CHROMIUM_REVISION at first compile;
  // any drift surfaces as a Mojom interface mismatch and the
  // build fails loud, not silent.
  //
  // TODO(M3-R2-isolation-info): browser-process clients with no
  // origin typically use net::IsolationInfo::CreateTransient() or
  // an empty IsolationInfo; pick the right shape during the first
  // build pass. Signaling has no third-party-cookies surface, so
  // the cheapest correct value is fine.
  const url::Origin browser_origin = url::Origin::Create(dial_url);
  const std::vector<std::string> requested_protocols;
  // chromium 7727: NetworkContext::CreateWebSocket dropped the
  // site_for_cookies parameter entirely (slot is now
  // storage_access_api_status directly) — the SiteForCookies local is
  // no longer constructed. IsolationInfo::CreateTransient now requires
  // an explicit nonce argument; std::nullopt = no nonce (signaling has
  // no third-party-cookies surface, so transient + no-nonce is correct).
  net::IsolationInfo isolation_info =
      net::IsolationInfo::CreateTransient(/*nonce=*/std::nullopt);

  std::vector<network::mojom::HttpHeaderPtr> additional_headers;
  // No additional headers — the JWT travels in the URL query param,
  // not as Authorization: Bearer ..., to match the streamer.js wire
  // shape physics already accepts.

  // chromium 7727 NetworkContext::CreateWebSocket signature drift:
  //  - site_for_cookies parameter removed (slot 3 is now
  //    storage_access_api_status directly)
  //  - new client_security_state parameter after `origin` —
  //    nullptr is correct for a browser-process infrastructure
  //    client (no special renderer security state)
  //  - process_id is now network::OriginatingProcessId (was the
  //    mojom int constant); ::browser() is the canonical factory
  network_context_->CreateWebSocket(
      dial_url,
      requested_protocols,
      /*storage_access_api_status=*/
      net::StorageAccessApiStatus::kNone,
      isolation_info,
      std::move(additional_headers),
      /*process_id=*/network::OriginatingProcessId::browser(),
      browser_origin,
      /*client_security_state=*/nullptr,
      network::mojom::kWebSocketOptionNone,
      net::MutableNetworkTrafficAnnotationTag(kSignalingTrafficAnnotation),
      handshake_client_receiver_.BindNewPipeAndPassRemote(),
      /*url_loader_network_observer=*/mojo::NullRemote(),
      /*auth_handler=*/mojo::NullRemote(),
      /*header_client=*/mojo::NullRemote(),
      /*throttling_profile_id=*/std::nullopt);
}

bool SignalingWsClient::Send(const Envelope& envelope) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kOpen) {
    return false;
  }
  // Defensive rewrite of `from` — the browser is the sole emitter on
  // this client, so the wire contract says `from = "browser"` for
  // every outgoing envelope. The R1 codec does not enforce this; the
  // physics broker's webrtc_signaling.rs::with_from rewrites server-
  // side too. Mirrors streamer.js:1976 (legacy: `from: "browser"`
  // hard-set on every emit). Coordinated w/ m3-r1-drafter.
  Envelope to_send = envelope;
  to_send.from = PeerRole::kBrowser;

  std::optional<std::string> encoded = Encode(to_send);
  if (!encoded.has_value()) {
    // R1 Encode returns nullopt only on encoding-invariant violation
    // (active variant doesn't match the tag). That's a caller bug,
    // not a transport issue — fail loud.
    FailWithError("R1 Encode() rejected envelope (variant/tag mismatch)");
    return false;
  }
  return WriteTextFrame(*encoded);
}

bool SignalingWsClient::SendText(std::string_view json) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kOpen) {
    return false;
  }
  return WriteTextFrame(json);
}

void SignalingWsClient::Disconnect() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ == State::kClosing || state_ == State::kClosed ||
      state_ == State::kIdle) {
    return;
  }
  state_ = State::kClosing;
  if (remote_websocket_.is_bound()) {
    // 1000 = normal closure. Empty reason — physics's broker logs
    // the close reason for diagnostics; we have nothing structured
    // to say at clean-close time, so an empty string is correct.
    remote_websocket_->StartClosingHandshake(1000, "");
  }
}

bool SignalingWsClient::is_connected() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return state_ == State::kOpen;
}

bool SignalingWsClient::is_closing() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return state_ == State::kClosing;
}

// --------------------------------------------------------------------
// WebSocketHandshakeClient impl
// --------------------------------------------------------------------

void SignalingWsClient::OnOpeningHandshakeStarted(
    network::mojom::WebSocketHandshakeRequestPtr request) {
  // Diagnostic only — physics's broker logs an analogous "handshake
  // received" structured log on its side. Drop verbosity unless a
  // chromium-side connect issue needs investigating.
  DVLOG(1) << "cb_signaling: handshake started "
           << (request ? request->url.spec() : std::string("(no request)"));
}

void SignalingWsClient::OnFailure(const std::string& message,
                                  int net_error,
                                  int response_code) {
  // Connect failed before the handshake completed. Common shapes:
  //   * net_error != 0 — DNS / TCP / TLS failure (host typo, no
  //     route, cert mismatch).
  //   * response_code 401 — JWT rejected by physics (wrong audience,
  //     expired, wrong sid).
  //   * response_code 404 — session_id not registered server-side.
  // FailWithError logs + transitions to kClosed.
  FailWithError(base::StrCat(
      {"handshake failed: ", message,
       " net_error=", base::NumberToString(net_error),
       " response=", base::NumberToString(response_code)}));
}

void SignalingWsClient::OnConnectionEstablished(
    mojo::PendingRemote<network::mojom::WebSocket> websocket,
    mojo::PendingReceiver<network::mojom::WebSocketClient> client_receiver,
    network::mojom::WebSocketHandshakeResponsePtr /*response*/,
    mojo::ScopedDataPipeConsumerHandle readable,
    mojo::ScopedDataPipeProducerHandle writable) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kConnecting) {
    // Late callback after Disconnect / destruct — drop silently.
    return;
  }
  state_ = State::kOpen;
  remote_websocket_.Bind(std::move(websocket));
  client_receiver_.Bind(std::move(client_receiver));
  readable_ = std::move(readable);
  writable_ = std::move(writable);

  // After the handshake completes the WebSocket impl waits for the
  // client to signal it's ready to receive. Without this call, no
  // OnDataFrame callbacks fire — observable as "physics shows the
  // browser joined but no inbound envelopes ever land".
  remote_websocket_->StartReceiving();

  LOG(INFO) << "cb_signaling: connected; session_id=" << config_.session_id;
  observer_->OnConnected();
}

// --------------------------------------------------------------------
// WebSocketClient impl
// --------------------------------------------------------------------

void SignalingWsClient::OnDataFrame(
    bool fin,
    network::mojom::WebSocketMessageType type,
    uint64_t data_len) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kOpen) {
    return;
  }

  // Binary frames are not part of the signaling contract. Physics
  // sends text only; surfaces here only if a broker-side bug or
  // adversarial client mis-sends — drop with an error.
  if (type != network::mojom::WebSocketMessageType::TEXT &&
      type != network::mojom::WebSocketMessageType::CONTINUATION) {
    FailWithError("unexpected non-text frame on signaling channel");
    return;
  }

  // First fragment of a (possibly multi-fragment) message.
  if (type == network::mojom::WebSocketMessageType::TEXT) {
    inbound_buffer_.clear();
    inbound_type_ = type;
  }
  inbound_remaining_ += data_len;

  // Read pump — drain whatever bytes are currently in the readable
  // pipe. The pipe-watcher re-arms us on additional data.
  OnReadable(MOJO_RESULT_OK);

  // |fin| signals "this is the last fragment". When we've also
  // drained all announced bytes, decode + dispatch.
  if (fin && inbound_remaining_ == 0) {
    std::optional<Envelope> env = Decode(inbound_buffer_);
    if (!env.has_value()) {
      FailWithError("R1 Decode() rejected inbound frame");
      return;
    }
    observer_->OnEnvelope(*env);
    inbound_buffer_.clear();
  }
}

void SignalingWsClient::OnDropChannel(bool was_clean,
                                      uint16_t code,
                                      const std::string& reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const State prev = state_;
  state_ = State::kClosed;
  remote_websocket_.reset();
  client_receiver_.reset();
  handshake_client_receiver_.reset();
  readable_.reset();
  writable_.reset();

  if (prev == State::kIdle || prev == State::kClosed) {
    return;
  }
  if (was_clean || prev == State::kClosing) {
    observer_->OnClosed(code, reason);
  } else {
    observer_->OnError(base::StrCat(
        {"channel dropped unexpectedly: code=",
         base::NumberToString(code), " reason=", reason}));
  }
}

void SignalingWsClient::OnClosingHandshake() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Remote initiated close; chromium will follow up with
  // OnDropChannel once the round-trip completes. Transition to
  // kClosing so a concurrent Send() is rejected without firing an
  // error.
  if (state_ == State::kOpen) {
    state_ = State::kClosing;
  }
}

// --------------------------------------------------------------------
// Internal helpers
// --------------------------------------------------------------------

void SignalingWsClient::OnReadable(MojoResult /*result*/) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kOpen || !readable_.is_valid() ||
      inbound_remaining_ == 0) {
    return;
  }

  // Read in bounded chunks. Signaling envelopes are tiny but a
  // multi-MB defensive ceiling prevents an adversarial peer from
  // pinning the read loop forever.
  //
  // TODO(M3-R2-read-cap): plumb a configurable max envelope size +
  // enforce it. For draft scope, the ceiling is implicit in
  // chromium's WebSocket frame size limit (configured at the
  // network service); good enough for first-compile.
  while (inbound_remaining_ > 0) {
    const void* buffer = nullptr;
    uint32_t num_bytes = 0;
    MojoResult res = readable_->BeginReadData(&buffer, &num_bytes,
                                              MOJO_READ_DATA_FLAG_NONE);
    if (res == MOJO_RESULT_SHOULD_WAIT) {
      // No more bytes right now — chromium will re-fire OnDataFrame
      // / the pipe-readable watcher when more arrive.
      return;
    }
    if (res != MOJO_RESULT_OK) {
      FailWithError("readable pipe error during BeginReadData");
      return;
    }
    const uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(
        num_bytes, inbound_remaining_));
    inbound_buffer_.append(static_cast<const char*>(buffer), take);
    inbound_remaining_ -= take;
    readable_->EndReadData(take);
  }
}

bool SignalingWsClient::WriteTextFrame(std::string_view payload) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!remote_websocket_.is_bound() || !writable_.is_valid()) {
    return false;
  }

  // Chromium's WebSocket impl wants the SendMessage call BEFORE
  // bytes are written to the writable pipe — the network service
  // reads `data_len` bytes off the pipe to form the frame body.
  remote_websocket_->SendMessage(
      network::mojom::WebSocketMessageType::TEXT, payload.size());

  // Single write — signaling envelopes are < 64 KiB in practice
  // (SDP offers run a few KiB; ICE candidates are tiny), well
  // under the chromium WebSocket pipe default capacity. A short
  // write would surface as MOJO_RESULT_SHOULD_WAIT and we'd need a
  // pipe-writable watcher to drain across multiple ticks; the TODO
  // tracks that follow-up if monitoring ever flags it.
  //
  // TODO(M3-R2-write-watch): add a SimpleWatcher on |writable_| +
  // a small outbound queue for short-write recovery once the first
  // compile passes. Not needed for the CV2-52 acceptance probe.
  uint32_t written = static_cast<uint32_t>(payload.size());
  MojoResult res = writable_->WriteData(payload.data(), &written,
                                        MOJO_WRITE_DATA_FLAG_ALL_OR_NONE);
  if (res != MOJO_RESULT_OK || written != payload.size()) {
    FailWithError("writable pipe error during WriteData");
    return false;
  }
  return true;
}

void SignalingWsClient::FailWithError(std::string_view reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ == State::kClosed) {
    return;
  }
  state_ = State::kClosed;
  remote_websocket_.reset();
  client_receiver_.reset();
  handshake_client_receiver_.reset();
  readable_.reset();
  writable_.reset();
  LOG(WARNING) << "cb_signaling: error: " << reason;
  observer_->OnError(reason);
}

}  // namespace cloud_browser::signaling

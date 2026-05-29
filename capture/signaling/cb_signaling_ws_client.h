// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Native browser-process WebSocket signaling client — M3 R2 (CV2-52).
//
// Replaces streamer.js's WebSocket-to-broker (capture/streamer-page/
// streamer.js, lines 1840-1850) with a native C++ client that runs
// directly in cb-chromium's browser process. The page-based streamer
// is being torn out wholesale; this client + the M1 PCF + the M3 R3-R7
// SDP / ICE / reconnect deliverables together replace it.
//
// Scope (CV2-52):
//   * Dial wss://{host}/api/webrtc/signaling/{session_id}?role=browser&token=<JWT>.
//     Physics learns the peer role from the URL query param (see
//     physics/src/api/handlers/webrtc_signaling.rs around the
//     "signaling: browser peer joined" info log) — so the first frame
//     from this client can be the SDP offer directly. No hello frame.
//   * Read + write pumps over the duplex websocket.
//   * Inbound frames decoded via M3 R1's Envelope codec; outbound
//     frames encoded the same way.
//   * Clean close on Disconnect() — code 1000, no reason text.
//
// Out of scope:
//   * SDP offer / answer construction — R4 (CV2-54).
//   * ICE candidate generation / trickle wiring — R4 / R5.
//   * Reconnect / backoff / session-state replay — R7 (CV2-57).
//
// Configuration source — streamer.js sourced session_id / token / host
// as page-URL query params; the page is going away, so this client
// reads them from POD ENV at startup:
//
//   WEBRTC_SIGNALING_HOST         e.g. "physics.svc:80" or "triform.dev".
//                                 Scheme is inferred (wss:// if
//                                 WEBRTC_SIGNALING_TLS is unset or "1",
//                                 ws:// when "0" — dev compose only).
//   WEBRTC_SIGNALING_SESSION_ID   "cb:{element_id}:{attempt_id}" as
//                                 minted by physics's launch path. The
//                                 JWT's `sid` claim must equal this.
//   WEBRTC_SIGNALING_TOKEN        HS256 JWT, claims
//                                 {sub, sid, role:"browser",
//                                  aud:"chromeless-streamer", exp}.
//                                 Signed with the physics-side secret
//                                 WEBRTC_SIGNALING_BROWSER_JWT_SECRET
//                                 (legacy fallback CHROMELESS_JWT_
//                                 SECRET). When the physics side
//                                 secret env is unset, physics runs in
//                                 dev auth-disabled mode — this client
//                                 still sends the token if present, or
//                                 omits the query param entirely if not.
//
// LoadConfigFromEnv() is the env-reading helper; the embedder in
// cloud_browser_browser_main_parts.{cc,h} owns one SignalingWsClient
// instance for the cb-chromium browser process lifetime.
//
// Threading model — the client is owned + driven on the UI thread (the
// embedder's thread; same thread that owns the PCF threads from M1).
// The underlying chromium network::mojom::WebSocket runs on the
// network service process; all observer callbacks (OnConnected /
// OnEnvelope / OnClosed / OnError) are dispatched back to the UI
// thread by the mojo runtime, so observer impls do not need to hop.
//
// Cross-references:
//   * capture/signaling/cb_wire_envelope.{h,cc} (M3 R1 — codec)
//   * capture/build-integration/cloud_browser_pcf.{h,cc} (M1 — PCF)
//   * capture/streamer-page/streamer.js (legacy — protocol reference)
//   * physics/src/api/handlers/webrtc_signaling.rs (broker)

#ifndef CAPTURE_SIGNALING_CB_SIGNALING_WS_CLIENT_H_
#define CAPTURE_SIGNALING_CB_SIGNALING_WS_CLIENT_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/websocket.mojom.h"

// M3 R1 codec — Envelope struct + Encode/Decode free functions.
// Aligned with the API drafted in cb_wire_envelope.h on
// cv2/m3-wire-envelope-codec (commit f51d5c4).
#include "capture/signaling/cb_wire_envelope.h"

namespace cloud_browser::signaling {

// Runtime configuration. Construct via LoadConfigFromEnv() in the
// embedder, or by hand in unit tests (cb_signaling_ws_client_test.cc).
struct WsClientConfig {
  // host[:port]. No scheme, no path. Required.
  std::string host;

  // "cb:{element_id}:{attempt_id}" — the JWT's sid claim must equal
  // this. The URL encoder treats this as one path segment (the
  // colons are NOT percent-encoded; physics's axum router accepts
  // them in the {session_id} match per the cb%3A test fixture at
  // physics/src/api/middleware/auth.rs:1569). Required.
  std::string session_id;

  // HS256 JWT or empty. Empty is allowed for dev / physics-auth-
  // disabled mode: the `&token=` query param is omitted entirely
  // rather than sent as the empty string.
  std::string token;

  // true → wss://, false → ws:// (dev compose only). Default true.
  bool use_tls = true;

  // Build the dial URL deterministically. Returns the empty string
  // if host or session_id is empty (the embedder logs + bails).
  std::string BuildUrl() const;
};

// Reads WEBRTC_SIGNALING_HOST / _SESSION_ID / _TOKEN / _TLS from the
// process env (getenv(3)). Returns std::nullopt if WEBRTC_SIGNALING_HOST
// or WEBRTC_SIGNALING_SESSION_ID is missing — the embedder must then
// log + skip the signaling boot. Token may be missing; TLS defaults
// to true.
//
// TODO(M3-R2-env-source): chromium's preferred env-reading idiom in
// the browser process is base::Environment::Create() ->
// GetVar(name, &value) — not raw getenv(3). Likely cleaner. Confirm
// during first-build pass on triform-8 and swap if so.
std::optional<WsClientConfig> LoadConfigFromEnv();

// Observer for connection lifecycle + inbound frames.
//
// All callbacks fire on the thread that constructed the
// SignalingWsClient (= the UI thread when the embedder owns the
// client). Observer impls are free to call back into
// SignalingWsClient::Send / Disconnect from any callback.
class SignalingClientObserver {
 public:
  virtual ~SignalingClientObserver() = default;

  // The websocket handshake completed; the channel is now duplex.
  // Physics emits its "signaling: browser peer joined" structured
  // log on this transition (CV2-52 acceptance criterion).
  virtual void OnConnected() {}

  // A complete envelope arrived inbound. R4 / R5 / R6 consumers
  // dispatch on envelope.type ("offer" / "answer" / "ice" / "bye")
  // and feed the SDP / ICE payloads into the PCF.
  virtual void OnEnvelope(const Envelope& envelope) = 0;

  // The remote closed the channel, or our Disconnect() completed
  // round-trip. |code| follows RFC 6455 (1000 = normal). The
  // observer should consider the client unusable; reconnect is
  // owned by R7.
  virtual void OnClosed(uint16_t code, std::string_view reason) = 0;

  // A transport / handshake / encode / decode error occurred. The
  // client is half-broken on error; the observer should Disconnect()
  // and (R7+) trigger reconnect via a separate driver. |reason| is
  // a human-readable string for logs, not for the wire.
  virtual void OnError(std::string_view reason) {}
};

// Native browser-process WebSocket client to the physics signaling
// broker.
//
// Lifecycle:
//   1. Construct on the UI thread, passing the embedder's
//      network::mojom::NetworkContext* (from
//      cloud_browser_browser_context's StoragePartition).
//   2. Call Connect(). The constructor itself does NOT dial — the
//      separation lets the embedder construct the client at
//      PreMainMessageLoopRun and dial later when the PCF is ready.
//   3. Observer fires OnConnected() / OnEnvelope(...) callbacks.
//   4. Send(envelope) writes through R1's codec to the wire.
//   5. Disconnect() initiates a clean close (code 1000); observer
//      fires OnClosed(1000, "") once the round-trip completes.
//   6. Destruct on the UI thread. Destruction at any state is safe;
//      a mid-handshake destruction silently aborts the dial without
//      firing observer callbacks.
class SignalingWsClient
    : public network::mojom::WebSocketHandshakeClient,
      public network::mojom::WebSocketClient {
 public:
  SignalingWsClient(
      network::mojom::NetworkContext* network_context,
      WsClientConfig config,
      SignalingClientObserver* observer);

  SignalingWsClient(const SignalingWsClient&) = delete;
  SignalingWsClient& operator=(const SignalingWsClient&) = delete;

  ~SignalingWsClient() override;

  // Initiate the WebSocket handshake. Idempotent: a second call
  // while connecting or connected is a no-op (logged, not an error).
  void Connect();

  // Encode |envelope| via R1's codec and send as a single text
  // websocket frame. The `from` field is overwritten to
  // PeerRole::kBrowser before encode — the browser is the sole
  // emitter on this client, so it's the only correct value (mirrors
  // streamer.js's hard-set `from: "browser"` on every outgoing
  // envelope; the physics broker rewrites server-side too). Returns
  // false if not currently connected or if the codec rejected the
  // envelope (observer.OnError() also fires on codec rejection).
  bool Send(const Envelope& envelope);

  // Lower-level escape hatch: send |json| as a single text frame
  // without going through R1's encoder. Tests use this to inject
  // malformed envelopes; production code goes through Send(...) so
  // codec contract drift is caught at the encoder.
  bool SendText(std::string_view json);

  // Initiate a clean close (code 1000, empty reason). Idempotent:
  // a second call while closing or closed is a no-op. Observer
  // fires OnClosed(1000, "") once the round-trip completes; on
  // remote-initiated close the code/reason from the remote is
  // forwarded instead.
  void Disconnect();

  // State accessors. Safe from the UI thread only.
  bool is_connected() const;
  bool is_closing() const;

 private:
  // Internal state machine. Linear in normal operation; Disconnect()
  // or remote-close can fire from any state >= kConnecting.
  enum class State : uint8_t {
    kIdle,        // Constructed; Connect() not yet called.
    kConnecting,  // CreateWebSocket() in flight; awaiting handshake.
    kOpen,        // Handshake complete; reading + writing.
    kClosing,     // Disconnect() called; awaiting OnDropChannel.
    kClosed,      // Terminal. No further callbacks will fire.
  };

  // network::mojom::WebSocketHandshakeClient (chromium calls these
  // on us as the handshake progresses; the network service is the
  // peer).
  void OnOpeningHandshakeStarted(
      network::mojom::WebSocketHandshakeRequestPtr request) override;
  void OnFailure(const std::string& message,
                 int net_error,
                 int response_code) override;
  void OnConnectionEstablished(
      mojo::PendingRemote<network::mojom::WebSocket> websocket,
      mojo::PendingReceiver<network::mojom::WebSocketClient> client_receiver,
      network::mojom::WebSocketHandshakeResponsePtr response,
      mojo::ScopedDataPipeConsumerHandle readable,
      mojo::ScopedDataPipeProducerHandle writable) override;

  // network::mojom::WebSocketClient (post-handshake frame +
  // lifecycle callbacks).
  void OnDataFrame(bool fin,
                   network::mojom::WebSocketMessageType type,
                   uint64_t data_len) override;
  void OnDropChannel(bool was_clean,
                     uint16_t code,
                     const std::string& reason) override;
  void OnClosingHandshake() override;

  // Drain the readable data pipe into |inbound_buffer_|, then attempt
  // a single envelope decode when |inbound_remaining_| reaches zero.
  // Re-armed by the OnDataFrame callback; the watcher fires on
  // pipe-readable signal between frames.
  void OnReadable(MojoResult result);

  // Write |payload| to the websocket as a single text message. The
  // chromium API is two-stage: SendMessage(type, len) on the WebSocket
  // remote, then bytes into the writable data pipe. Returns false if
  // the writable pipe is closed or the write would block (the latter
  // shouldn't happen for signaling traffic — envelopes are tiny —
  // but if it does, the client drops to kClosed and observer fires
  // OnError).
  bool WriteTextFrame(std::string_view payload);

  // Transition + fire OnError, then move to kClosed without sending
  // a close frame (errors mean the channel is half-broken).
  void FailWithError(std::string_view reason);

  // Helpers — constructed once at OnConnectionEstablished and held
  // alive for the open lifetime of the channel.
  raw_ptr<network::mojom::NetworkContext> network_context_;
  const WsClientConfig config_;
  raw_ptr<SignalingClientObserver> observer_;

  State state_ = State::kIdle;

  // Filled by CreateWebSocket(...). The handshake_client_receiver_ is
  // the chromium-side endpoint we hand into CreateWebSocket; chromium
  // then calls the WebSocketHandshakeClient methods above on us via
  // it. The remote_websocket_ is the chromium-side WebSocket handle
  // we get back at OnConnectionEstablished and use to drive Send +
  // Close from our side.
  mojo::Receiver<network::mojom::WebSocketHandshakeClient>
      handshake_client_receiver_{this};
  mojo::Receiver<network::mojom::WebSocketClient> client_receiver_{this};
  mojo::Remote<network::mojom::WebSocket> remote_websocket_;
  mojo::ScopedDataPipeConsumerHandle readable_;
  mojo::ScopedDataPipeProducerHandle writable_;

  // Inbound assembly. WebSocket frames can be fragmented; we
  // accumulate payload bytes until a frame's announced data_len has
  // arrived in full, then decode the envelope and reset. Signaling
  // traffic is small JSON (low-kB envelopes), so a single growing
  // std::string is the right shape.
  std::string inbound_buffer_;
  uint64_t inbound_remaining_ = 0;
  network::mojom::WebSocketMessageType inbound_type_ =
      network::mojom::WebSocketMessageType::TEXT;
  // CV2 Gate 6 fix: set true in OnDataFrame when the final fragment
  // (|fin|) of a message has been announced. The decode-dispatch reads
  // it at the END of OnReadable so a message whose last bytes arrive via
  // the pipe-readable watcher (a re-fired OnReadable, NOT a fresh
  // OnDataFrame) is still decoded. Previously the decode lived only at
  // the bottom of OnDataFrame, so a fragmented frame (e.g. the ~2.5 kB
  // SDP answer) whose drain completed under the watcher was never
  // decoded — or its buffer was clobbered by the next message's TEXT
  // frame — and Decode() saw partial/concatenated JSON → R1 rejection.
  bool inbound_fin_pending_ = false;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<SignalingWsClient> weak_factory_{this};
};

}  // namespace cloud_browser::signaling

#endif  // CAPTURE_SIGNALING_CB_SIGNALING_WS_CLIENT_H_

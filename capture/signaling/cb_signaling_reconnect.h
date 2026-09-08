// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Reconnect + idle-timeout driver wrapping the M3 R2 signaling client —
// M3 R7 (CV2-57).
//
// R2 (cb_signaling_ws_client.{h,cc}) deliberately stopped at a clean
// duplex channel with no recovery story: a remote close or transport
// error fires OnClosed/OnError once and the consumer is on its own.
// That separation kept R2 testable in isolation; R7 is the missing
// recovery layer.
//
// Scope (CV2-57):
//   * Exponential backoff reconnect on close OR error.
//   * Bounded reconnect attempts — give up after `max_attempts` and
//     fire OnGaveUp() to the consumer once the channel is terminally
//     dead. Default 10 attempts (≈ 1s + 2s + 4s + 8s + 16s + 32s ×5
//     = ≈ 3min ceiling with the default schedule).
//   * Idle-inbound timeout — if no inbound frame for `idle_timeout`,
//     treat the channel as silently dead, force a close + reconnect.
//     This is the keepalive substitute (see "Why no WS-level pings"
//     below).
//   * Successful (re)connect resets the backoff and the attempt
//     counter — consumers see one OnConnected per recovered channel,
//     not a synthetic "reconnected" event.
//
// Out of scope:
//   * Session-state replay (re-sending the buffered SDP / ICE that
//     was in flight when the channel died). The R4 offerer driver
//     (CV2-54) owns its own state machine and will re-drive the
//     handshake from scratch on OnConnected — R7 doesn't need to
//     replay envelopes from R2's outbound queue.
//   * Token refresh on 401 / handshake rejection. If the JWT expires
//     mid-session, R7's reconnect will just keep failing handshake;
//     a future R8 (or an env-refresh hook on the embedder) owns
//     refreshing the WEBRTC_SIGNALING_TOKEN env var. R7 surfaces this
//     as "handshake failed N times in a row → OnGaveUp"; the
//     embedder logs the give-up and exits the cb-chromium process,
//     and the controller (physics) restarts the pod with a fresh
//     token.
//
// # Why no WS-level pings
//
// RFC 6455 control frames (opcode 0x9 ping / 0xA pong) are the
// canonical WebSocket keepalive mechanism. The R7 brief originally
// asked for "ping/pong keepalive" — and we'd take it gladly if the
// chromium network service exposed the surface.
//
// It doesn't. `network::mojom::WebSocket` (services/network/public/
// mojom/websocket.mojom) has SendMessage / StartReceiving / Close /
// StartClosingHandshake. There is no SendPing equivalent and no
// inbound-ping observer. The network service auto-responds to remote
// pings at a layer below mojo, so the *remote* can keep us alive (and
// physics's tungstenite-based broker does emit pings; see
// physics/src/api/handlers/webrtc_signaling.rs around the
// `axum::extract::ws::Message::Ping` arm), but the cb-chromium
// embedder cannot emit pings of its own.
//
// We could push pings into the envelope codec — but the codec is
// deliberately closed (six pinned tags: offer / answer / ice / bye /
// request_renegotiate / probe_result; see cb_wire_envelope.h's
// "complete accept-list" comment + the `sdp_offer` negative test that
// locks the close). Adding a kPing tag is a contract change that
// would have to land via M3 R1 (codec) and the physics broker — out
// of scope for R7.
//
// So R7's "are we alive?" detector is timing-based: if we see no
// inbound bytes for `idle_timeout`, the channel is considered dead.
// In practice physics's broker emits at least one ping every 30s on
// idle channels (tungstenite default), so a 60–120s idle_timeout
// distinguishes "broker silent because no traffic" from "transport
// broken". The default below is 90s; consumers can tighten when their
// traffic profile expects more chatter.
//
// TODO(M3-R7-ws-ping-mojo): file a chromium upstream issue (or check
// for an existing one) asking for outbound SendPing / OnPing on the
// network::mojom::WebSocket interface. If/when that lands, swap the
// idle-timeout path for real RFC 6455 keepalive and drop this note.
//
// # Threading
//
// Same model as R2 — owned + driven on the UI thread (the embedder's
// thread). The backoff timer and the idle-check timer are both
// base::OneShotTimer<this> instances, which fire callbacks on the
// thread they were Start()ed on (the UI thread). No locking, no
// PostTask hops; the SEQUENCE_CHECKER catches misuse.
//
// # Cross-references:
//   * capture/signaling/cb_signaling_ws_client.{h,cc}  (M3 R2 — inner client)
//   * capture/signaling/cb_wire_envelope.{h,cc}         (M3 R1 — codec)
//   * physics/src/api/handlers/webrtc_signaling.rs      (broker — ping source)

#ifndef CAPTURE_SIGNALING_CB_SIGNALING_RECONNECT_H_
#define CAPTURE_SIGNALING_CB_SIGNALING_RECONNECT_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "services/network/public/mojom/network_context.mojom.h"

#include "capture/signaling/cb_signaling_transport.h"  // CV2-REDIAL
#include "capture/signaling/cb_signaling_ws_client.h"
#include "capture/signaling/cb_wire_envelope.h"

namespace cloud_browser::signaling {

// Reconnect schedule. Default values were picked to bracket the
// physics broker's restart envelope: a Flux-driven rollout of physics
// typically gaps the websocket endpoint for ~10–40s end-to-end, and
// the default schedule covers that with room (1s + 2s + 4s + 8s = 15s
// of patient retries, then 16s + 32s × 6 = the long tail before we
// give up at ≈3.5min total).
struct ReconnectConfig {
  // Initial backoff. The first reconnect attempt waits this long.
  base::TimeDelta initial_backoff = base::Seconds(1);

  // Backoff multiplier — each attempt waits `multiplier × previous`.
  // 2.0 → exponential doubling. 1.0 → fixed-interval (allowed but
  // generally too aggressive; consumers usually want exponential).
  double multiplier = 2.0;

  // Maximum backoff between two attempts. After hitting the cap the
  // schedule plateaus at this value until max_attempts is exhausted.
  base::TimeDelta max_backoff = base::Seconds(32);

  // How many reconnect attempts to make before giving up and firing
  // OnGaveUp() to the consumer. The initial Connect() is attempt 0;
  // subsequent reconnects are 1..max_attempts. Once max_attempts have
  // failed, no further reconnects fire.
  //
  // 0 disables reconnect entirely — the wrapper becomes a thin
  // pass-through with idle-timeout still active. Useful for tests.
  uint32_t max_attempts = 10;

  // Idle-inbound timeout. If no inbound envelope arrives for this
  // long while the channel is open, the wrapper treats the channel
  // as dead, force-closes, and reconnects (counts toward
  // max_attempts).
  //
  // Set to base::TimeDelta() (zero) to disable the idle-timeout path
  // entirely — the wrapper then only reacts to remote-driven
  // OnClosed/OnError. Useful when the consumer has its own
  // application-level liveness signal.
  base::TimeDelta idle_timeout = base::Seconds(90);
};

// Consumer-facing observer. Same lifecycle shape as
// SignalingClientObserver from R2, plus OnGaveUp for the terminal-
// give-up case.
//
// The reconnect wrapper translates inner-client lifecycle into this
// observer's vocabulary:
//
//   inner OnConnected            → observer OnConnected
//   inner OnEnvelope             → observer OnEnvelope
//   inner OnClosed (clean,1000)  → observer OnClosed (terminal,
//                                  reconnect not attempted; matches
//                                  R2 semantics for consumer-driven
//                                  Disconnect)
//   inner OnClosed (other code)  → schedule reconnect; observer is
//                                  NOT notified (the wrapper hides
//                                  intermediate disconnects to keep
//                                  the consumer's state machine
//                                  simple)
//   inner OnError                → schedule reconnect; same as above
//   reconnect attempts exhausted → observer OnGaveUp
//   idle_timeout expired         → force inner Disconnect, then
//                                  treat as OnClosed (non-1000) →
//                                  schedule reconnect
class ReconnectingClientObserver {
 public:
  virtual ~ReconnectingClientObserver() = default;

  // Channel up. Fires once per successful (re)connect — consumers
  // should re-drive any state that depends on a fresh channel here
  // (the R4 offerer driver, for instance, restarts the SDP handshake
  // from scratch on each OnConnected).
  virtual void OnConnected() {}

  // Inbound envelope decoded by R2 + R1's codec.
  virtual void OnEnvelope(const Envelope& envelope) = 0;

  // Terminal clean close — the consumer called Disconnect() on the
  // wrapper, OR the remote closed with code 1000. No reconnect will
  // be attempted; the wrapper is done.
  virtual void OnClosed(uint16_t code, std::string_view reason) = 0;

  // The wrapper exhausted max_attempts reconnects. The channel is
  // terminally dead from the wrapper's perspective; the consumer
  // should treat this as a fatal signaling error. The embedder
  // typically logs + tears down the cb-chromium browser process so
  // the controller (physics) can restart the pod with a fresh
  // token / fresh broker target.
  virtual void OnGaveUp(uint32_t attempts_made) {}
};

// Reconnecting wrapper around SignalingWsClient.
//
// Lifecycle mirrors R2:
//   1. Construct on the UI thread, passing the NetworkContext, a
//      WsClientConfig (used for every inner client instance — the
//      wrapper does NOT re-read env between attempts; the embedder
//      owns env-refresh policy), a ReconnectConfig, and the
//      consumer's observer.
//   2. Call Connect(). The first inner SignalingWsClient is
//      constructed + Connect()ed inline; reconnects construct fresh
//      inner clients on each scheduled retry.
//   3. Send(...) forwards to the current inner client when connected.
//      Returns false if not currently connected (the wrapper does
//      NOT buffer outbound envelopes — buffering is a session-state-
//      replay feature, explicitly out of R7 scope).
//   4. Disconnect() initiates a clean close on the inner client AND
//      cancels any pending reconnect. Terminal.
//   5. Destruct on the UI thread. Destruction at any state safely
//      tears down the inner client, the backoff timer, and the idle
//      timer without firing observer callbacks.
// CV2-REDIAL: also a SignalingTransport, so CbOffererDriver can hold the
// WRAPPER as its transport and keep sending across redials — the inner
// SignalingWsClient is replaced on every reconnect, so a raw pointer to it
// would dangle after the first drop.
class CbSignalingReconnect : public SignalingClientObserver,
                             public SignalingTransport {
 public:
  CbSignalingReconnect(
      network::mojom::NetworkContext* network_context,
      WsClientConfig config,
      ReconnectConfig reconnect_config,
      ReconnectingClientObserver* observer);

  CbSignalingReconnect(const CbSignalingReconnect&) = delete;
  CbSignalingReconnect& operator=(const CbSignalingReconnect&) = delete;

  ~CbSignalingReconnect() override;

  // Initiate the first connection attempt. Idempotent: a second call
  // while connecting, connected, or backing-off is a no-op (logged).
  void Connect();

  // Forward `envelope` to the current inner client. Returns false if
  // not currently connected (mid-backoff, mid-handshake, given up,
  // or never connected). The wrapper does NOT queue outbound — the
  // consumer is responsible for retrying after the next OnConnected.
  bool Send(const Envelope& envelope) override;  // SignalingTransport

  // Clean shutdown. Cancels pending backoff, cancels idle timer,
  // tells the inner client to Disconnect(). Observer eventually
  // fires OnClosed(1000, "") via the inner client's clean-close
  // round-trip. Idempotent.
  void Disconnect();

  // State accessors. Safe from the UI thread only.
  bool is_connected() const;
  bool is_reconnecting() const;
  uint32_t attempts_made() const;

 private:
  // Wrapper state machine. Disjoint from the inner client's State —
  // tracks the reconnect supervisor's view.
  enum class State : uint8_t {
    kIdle,           // Constructed; Connect() not yet called.
    kConnecting,     // Inner client dialing for the first time.
    kOpen,           // Inner client OnConnected, channel duplex.
    kBackingOff,     // Inner client dead; backoff timer running.
    kReconnecting,   // Backoff fired; inner client re-dialing.
    kGaveUp,         // max_attempts exhausted; OnGaveUp fired.
    kClosed,         // Consumer-driven Disconnect; terminal.
  };

  // SignalingClientObserver — inner client calls these on us.
  void OnConnected() override;
  void OnEnvelope(const Envelope& envelope) override;
  void OnClosed(uint16_t code, std::string_view reason) override;
  void OnError(std::string_view reason) override;

  // Build + dial a fresh inner SignalingWsClient. Resets the inbound-
  // assembly state implicitly (new instance). Used by Connect() for
  // the initial dial and by OnBackoffFired() for retries.
  void StartInnerDial();

  // Compute the next backoff delay using ReconnectConfig::multiplier
  // capped at max_backoff. Pure function of `current_backoff_`.
  base::TimeDelta NextBackoff() const;

  // Schedule a reconnect attempt OR fire OnGaveUp if max_attempts
  // exhausted. Called from OnClosed (non-1000), OnError, and the
  // idle-timeout path.
  void HandleChannelDead(std::string_view why);

  // Backoff timer fired — bump attempt counter, transition to
  // kReconnecting, dial.
  void OnBackoffFired();

  // Idle timer fired — no inbound for `idle_timeout`. Treat as dead.
  void OnIdleFired();

  // (Re)arm the idle timer with the configured timeout. No-op if
  // idle_timeout is zero (consumer opted out).
  void ArmIdleTimer();

  // Cancel any pending timers and tear down the inner client. Used
  // by Disconnect() and the dtor.
  void TearDown();

  raw_ptr<network::mojom::NetworkContext> network_context_;
  const WsClientConfig config_;
  const ReconnectConfig reconnect_config_;
  raw_ptr<ReconnectingClientObserver> observer_;

  State state_ = State::kIdle;

  // CV2-REDIAL: set by Disconnect(). A close arriving while this is false is
  // the BROKER's doing, whatever its code, and must be redialed. Before this
  // the wrapper treated ANY code-1000 close as "the consumer hung up" — so a
  // broker closing cleanly on its own shutdown (a rollout) would have parked
  // the worker in kClosed for good, which is precisely the outage this
  // wrapper exists to end.
  bool disconnect_requested_ = false;

  // Reconnect bookkeeping.
  uint32_t attempts_made_ = 0;
  base::TimeDelta current_backoff_ = base::TimeDelta();

  // Timers. base::OneShotTimer fires its callback on the sequence it
  // was Start()ed on (UI thread). On destruction the timer cancels
  // its pending fire — safe to free during a pending wait.
  base::OneShotTimer backoff_timer_;
  base::OneShotTimer idle_timer_;

  // Inner client. Recreated on each reconnect attempt — R2 doesn't
  // support re-dialing the same instance (its State machine is
  // single-shot: kIdle → kConnecting → kOpen → kClosing → kClosed,
  // no path back to kIdle). Wrapping it with fresh instances per
  // attempt keeps R2 unchanged and lets each attempt have an
  // independent mojo receiver / inbound buffer.
  std::unique_ptr<SignalingWsClient> inner_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<CbSignalingReconnect> weak_factory_{this};
};

}  // namespace cloud_browser::signaling

#endif  // CAPTURE_SIGNALING_CB_SIGNALING_RECONNECT_H_

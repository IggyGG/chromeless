// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Native browser-process offerer handshake driver — M3 R4 (CV2-54),
// extended by M3 R6 (CV2-56) with renegotiation + explicit teardown.
//
// Bridges the four ChromelessV2 native-peer pieces into a single SDP+ICE
// dance owner:
//   * M1 — webrtc::PeerConnectionFactoryInterface (cloud_browser_pcf).
//   * M3 R1 — wire-envelope codec (cb_wire_envelope).
//   * M3 R2 — SignalingWsClient + SignalingClientObserver
//             (cb_signaling_ws_client).
//   * M3 R3 — webrtc::PeerConnectionInterface::RTCConfiguration produced
//             by the ICE-server config loader (cb_ice_config — drafted
//             in parallel on cv2/m3-r3-ice-config).
//
// The browser peer is ALWAYS the offerer; the portal client is the
// answerer. Renegotiation IS supported here (R6 amendment, CV2-56):
// the dance re-enters from kIceInFlight, traverses the same
// kCreatingOffer → kSettingLocal → kAwaitingAnswer → kSettingRemote →
// kIceInFlight loop, and emits a fresh `offer` envelope. R6 also adds
// the `bye`-emitting public teardown path + the inbound
// `request_renegotiate` handler (which in R4 was a terminal failure).
//
// # Sequence (offerer dance)
//
//   1. Construct CbOffererDriver(pcf, ws_client, ice_config, observer,
//      ui_runner). The driver wires itself onto ws_client as the
//      SignalingClientObserver in Start().
//   2. Start() calls pcf->CreatePeerConnection(ice_config, this) on the
//      signaling thread; the returned PeerConnectionInterface fans
//      observer callbacks (OnIceCandidate / OnRenegotiationNeeded /
//      OnIceConnectionChange / ...) back to us on libwebrtc's
//      signaling thread — we PostTask onto signaling_task_runner_
//      before touching state.
//   3. The embedder then adds M2's video transceiver + M4 input DC +
//      M5 cursor DC onto pc(). The first OnRenegotiationNeeded()
//      callback fires when those adds settle; we respond by calling
//      pc->CreateOffer(this). When CreateOffer's success arrives we
//      emit the `offer` envelope and then SetLocalDescription.
//   4. SignalingClientObserver::OnEnvelope("answer", sdp) →
//      SetRemoteDescription(this, answer). The answerer SDP is now
//      pinned and ICE can begin.
//   5. PeerConnectionObserver::OnIceCandidate(candidate) → Send()
//      an `ice` envelope per candidate. When PC fires
//      OnIceGatheringChange(kIceGatheringComplete), Send() the
//      end-of-candidates marker (`ice` envelope with null `data`).
//   6. SignalingClientObserver::OnEnvelope("ice", payload) → if
//      payload.is_end_of_candidates is false, AddIceCandidate(...);
//      otherwise AddIceCandidate(nullptr) to flush the remote pool.
//
// # Replay-buffer interaction (physics T96 / T104)
//
// Physics's webrtc_signaling.rs broker maintains a per-session replay
// buffer: the offer + up to 32 ICE candidates, 60 s TTL. R4 sends
// envelopes eagerly the moment the PC produces them — there is NO
// client-presence wait on this side. The broker delivers buffered
// envelopes to the portal client the moment it joins. The portal-side
// answerer must accept offer-before-join + ICE-before-answer ordering;
// that's the portal's contract, not ours.
//
// # Threading
//
// Constructed + driven on the UI thread (same thread that owns the
// SignalingWsClient + the PCF embedder). All
// PeerConnectionObserver callbacks fire on libwebrtc's signaling thread
// (the third thread injected into the PCF dependencies via M1); the
// driver PostTask()s back to the UI thread before calling into the ws
// client or mutating state_. CreateSessionDescriptionObserver +
// SetSessionDescriptionObserver completions also fire on the signaling
// thread; same hop applies. SignalingClientObserver callbacks already
// arrive on the UI thread (the ws client's contract), so no hop is
// needed for inbound envelopes.
//
// # Lifetime
//
// The driver does NOT own the PCF or the ws client — both are owned
// by the embedder (CloudBrowserBrowserMainParts) and outlive the
// driver. The driver owns its scoped_refptr<PeerConnectionInterface>
// after Start(); destruction in any state safely tears the PC down by
// dropping that ref (libwebrtc handles teardown internally). A
// WeakPtrFactory guards every signaling-thread hop so a destroyed
// driver no-ops late callbacks rather than UAF'ing.
//
// # Error model
//
// Any failure on the offerer side (CreateOffer rejected, SetLocal /
// RemoteDescription rejected, ws send failed, protocol-violating
// inbound envelope, etc.) routes to FailWithReason — the driver's
// terminal failure path. R7 (CV2-57) will wire reconnect on top; for
// now we log + drop the PC + fire observer_->OnFailed. The PC is
// dropped on terminal failure to surface the failure to oncall via
// the iceConnectionState=closed transition.
//
// Clean teardown (R6) takes the parallel path: SendByeEnvelope() →
// transition to kClosed → drop PC → observer_->OnClosed. Inbound `bye`
// from the portal client follows the same path EXCEPT the bye is NOT
// echoed back on the wire (one-way close envelope; the broker forwards
// per the wire contract).
//
// # Renegotiation contract (R6)
//
// Three trigger sites converge on BeginRenegotiation():
//   1. Embedder calls RequestRenegotiation() — explicit nudge after
//      mutating transceivers / DataChannels on pc().
//   2. libwebrtc fires OnRenegotiationNeeded() AFTER the initial dance
//      completed (i.e. while in kIceInFlight). R4's latch — now
//      renamed initial_renegotiation_consumed_ — is no longer a
//      "no more negotiations allowed" gate; it's a one-shot marker
//      for "have we left kCreatingPc". Once set, subsequent
//      OnRenegotiationNeeded fires route to BeginRenegotiation()
//      instead of the initial-dance branch.
//   3. Inbound `request_renegotiate` envelope from the portal client.
//      In R4 this was a terminal-fail; in R6 it kicks off CreateOffer.
//
// Concurrency: renegotiation is BEGIN-FROM-kIceInFlight ONLY. Triggers
// that fire mid-dance (e.g. embedder calling RequestRenegotiation()
// while we're already in kCreatingOffer) are coalesced: a single
// pending_renegotiation_ flag re-fires the dance on the next return to
// kIceInFlight. This avoids the SetLocalDescription / SetRemote
// description observer-thrash that interleaved CreateOffer calls would
// produce.
//
// Cross-references:
//   * capture/build-integration/cloud_browser_pcf.{h,cc}     (M1)
//   * capture/signaling/cb_wire_envelope.{h,cc}              (M3 R1)
//   * capture/signaling/cb_signaling_ws_client.{h,cc}        (M3 R2)
//   * capture/signaling/cb_ice_config.{h,cc}                 (M3 R3)
//   * physics/src/api/handlers/webrtc_signaling.rs           (broker)

#ifndef CAPTURE_SIGNALING_CB_OFFERER_DRIVER_H_
#define CAPTURE_SIGNALING_CB_OFFERER_DRIVER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "api/jsep.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/rtp_receiver_interface.h"
#include "api/rtp_transceiver_interface.h"
#include "api/scoped_refptr.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "capture/signaling/cb_signaling_ws_client.h"
#include "capture/signaling/cb_wire_envelope.h"

namespace cloud_browser::signaling {

// Driver state machine. Linear in the happy path; any state >= kIdle
// can transition to kFailed on terminal error.
enum class OffererState : uint8_t {
  kIdle,            // Constructed; Start() not yet called.
  kCreatingPc,      // PC under construction; CreateOffer not yet issued.
  kCreatingOffer,   // CreateOffer in flight on signaling thread.
  kSettingLocal,    // SetLocalDescription in flight.
  kAwaitingAnswer,  // Local SDP set; `offer` sent; waiting for inbound
                    // `answer` envelope.
  kSettingRemote,   // SetRemoteDescription in flight.
  kIceInFlight,     // Remote SDP set; ICE candidates flowing both ways.
                    // Steady state until iceConnectionState reaches
                    // connected/failed/disconnected.
  kClosed,          // Bye/teardown happened cleanly.
  kFailed,          // Terminal failure; PC is dropped.
};

// Optional terminal-failure + ICE-state + lifecycle observer. The
// embedder may pass nullptr to use VLOG / LOG(ERROR) only. R7 (CV2-57)
// wires reconnect on top of OnFailed.
class OffererDriverObserver {
 public:
  virtual ~OffererDriverObserver() = default;

  // ICE connection state transitions surfaced verbatim — the embedder
  // uses kIceConnectionConnected to gate "session is live" metrics
  // and the M6 stats relay's start signal. Fires on the driver's UI
  // thread.
  virtual void OnIceConnectionStateChanged(
      webrtc::PeerConnectionInterface::IceConnectionState state) {}

  // Renegotiation lifecycle hooks (R6). The embedder uses these to
  // gate UI signals (e.g. brief "renegotiating" indicator) and to
  // suspend non-essential mutations to pc() while the dance is in
  // flight. Both fire on the driver's UI thread.
  //
  // OnRenegotiationStarted fires at the kIceInFlight → kCreatingOffer
  // transition; OnRenegotiationCompleted fires at the kSettingRemote
  // → kIceInFlight transition on the renegotiated dance. The pair
  // does NOT fire for the initial dance — that's signaled implicitly
  // by the first OnIceConnectionStateChanged(kIceConnectionConnected).
  virtual void OnRenegotiationStarted(std::string_view trigger) {}
  virtual void OnRenegotiationCompleted() {}

  // Clean teardown (R6). Fires when either:
  //   * Embedder called Close() — `reason` is the embedder-supplied
  //     reason string (default "session ended").
  //   * Inbound `bye` envelope from the portal client — `reason` is
  //     "remote bye".
  //   * The ws client closed cleanly via OnClosed(code, reason) — in
  //     R4 this transitioned silently; R6 now surfaces it.
  // The driver has already dropped the PC; reuse requires constructing
  // a new driver. Fires on the driver's UI thread.
  virtual void OnClosed(std::string_view reason) {}

  // Terminal failure. The driver has already dropped the PC; restart
  // requires tearing the driver down and constructing a new one
  // (R7's job). Fires on the driver's UI thread.
  virtual void OnFailed(std::string_view reason) {}
};

class CbOffererDriver
    : public SignalingClientObserver,
      public webrtc::PeerConnectionObserver,
      public webrtc::CreateSessionDescriptionObserver,
      public webrtc::SetLocalDescriptionObserverInterface,
      public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  // |pcf|:        from M1's CreateCloudBrowserPcf; the driver takes a
  //               scoped_refptr to keep it alive across its own lifetime.
  // |ws_client|:  from M3 R2; driver registers itself as the observer
  //               on Start(); ws_client must outlive the driver.
  // |ice_config|: from M3 R3's loader output — full RTCConfiguration
  //               with .servers populated. Copied at construction;
  //               later config swaps require driver teardown +
  //               reconstruct.
  // |observer|:   optional terminal-failure + ICE-state observer.
  //               nullptr is fine.
  // |ui_runner|:  the task runner of the driver's UI thread (= the
  //               embedder's thread). All inbound observer fan-in
  //               from libwebrtc's signaling thread hops onto this
  //               runner before touching state_ or the ws client.
  //               Typically base::SequencedTaskRunner::
  //               GetCurrentDefault() captured at construction.
  CbOffererDriver(
      webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pcf,
      SignalingWsClient* ws_client,
      webrtc::PeerConnectionInterface::RTCConfiguration ice_config,
      OffererDriverObserver* observer,
      scoped_refptr<base::SequencedTaskRunner> ui_runner);

  CbOffererDriver(const CbOffererDriver&) = delete;
  CbOffererDriver& operator=(const CbOffererDriver&) = delete;

  ~CbOffererDriver() override;

  // Begin the offerer dance:
  //   1. CreatePeerConnection(ice_config_, this).
  //   2. The embedder is expected to add the M2 video transceiver +
  //      the M4 input DataChannel + the M5 cursor DataChannel onto
  //      pc() BEFORE the first OnRenegotiationNeeded() fires; the
  //      driver does NOT issue CreateOffer until that callback hops
  //      in, which guarantees the embedder's adds are part of the
  //      first SDP.
  //
  // Idempotent: a second call is logged + ignored.
  void Start();

  // Renegotiation trigger (R6). Embedder-initiated nudge after mutating
  // transceivers, codecs, or DataChannels on pc(). Valid only from
  // kIceInFlight; called in any other state, the request is coalesced
  // into pending_renegotiation_ and re-fires on the next transition
  // back to kIceInFlight. Idempotent across rapid bursts: a second
  // call while one is in flight sets the pending flag once.
  //
  // The embedder typically does NOT need to call this explicitly —
  // libwebrtc's OnRenegotiationNeeded() callback also routes to
  // BeginRenegotiation() in R6, so transceiver mutations on pc()
  // self-trigger. RequestRenegotiation() exists for the cases where
  // the embedder knows the mutation was renegotiation-relevant but
  // libwebrtc's heuristic may not have fired (e.g. M4 R8 clipboard
  // DC label changes the m-line set without a new transceiver).
  void RequestRenegotiation();

  // Explicit teardown (R6). Sends a `bye` envelope on the wire (best-
  // effort — failure is logged but does not block teardown), drops
  // pc_, transitions to kClosed, and fires observer_->OnClosed(reason).
  //
  // |reason| is informational only; the bye envelope itself carries
  // no payload (the wire contract omits the `data` field for `bye`).
  // Reason flows to the observer + the log line.
  //
  // Idempotent: calls in kClosed/kFailed are no-ops. Calls in any
  // mid-dance state (kCreatingOffer, kSettingLocal, kAwaitingAnswer,
  // kSettingRemote) collapse the dance cleanly — the bye envelope
  // arrives at the broker, the broker forwards to any waiting
  // portal client, and the portal client tears down its half. We do
  // NOT wait for the broker's ack before transitioning to kClosed.
  void Close(std::string_view reason);

  // State accessor for tests + the embedder's readiness gate.
  OffererState state() const;

  // Direct PeerConnection access for the embedder's add-tracks step.
  // Returned ref is valid until the driver enters kFailed or is
  // destructed. After Start() the PC is logically owned by the
  // driver and external mutation races the offerer dance — don't.
  webrtc::PeerConnectionInterface* pc() const;

  // SignalingClientObserver — inbound from the ws client. Already on
  // the UI thread per the M3 R2 contract.
  void OnEnvelope(const Envelope& env) override;
  void OnClosed(uint16_t code, std::string_view reason) override;
  void OnError(std::string_view reason) override;

  // webrtc::PeerConnectionObserver — invoked on libwebrtc's signaling
  // thread. Every override hops to ui_runner_ before touching state.
  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override;
  void OnIceCandidate(
      const webrtc::IceCandidateInterface* candidate) override;
  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState new_state) override;
  void OnIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
  void OnConnectionChange(
      webrtc::PeerConnectionInterface::PeerConnectionState new_state) override;
  void OnRenegotiationNeeded() override;
  void OnDataChannel(
      webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override;
  void OnAddTrack(
      webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
      const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&
          streams) override;
  void OnTrack(
      webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)
      override;

  // webrtc::CreateSessionDescriptionObserver — CreateOffer completion.
  // Fires on libwebrtc's signaling thread; we hop to ui_runner_
  // before touching state.
  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
  void OnFailure(webrtc::RTCError error) override;

  // SetLocalDescriptionObserverInterface +
  // SetRemoteDescriptionObserverInterface — completion of the two
  // descriptor setters. Disambiguated by state_ on the UI hop.
  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override;
  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override;

 private:
  // Inbound dispatch helpers. Routes |env| based on env.type with
  // state-guard enforcement; protocol violations route to
  // FailWithReason.
  void HandleOfferEnvelope(const Envelope& env);
  void HandleAnswerEnvelope(const Envelope& env);
  void HandleIceEnvelope(const Envelope& env);
  void HandleByeEnvelope();
  void HandleRequestRenegotiateEnvelope();
  void HandleProbeResultEnvelope(const Envelope& env);

  // Hop landing points — all run on ui_runner_, guarded by
  // weak_factory_'s WeakPtr.
  void HopHandleIceCandidate(
      std::unique_ptr<webrtc::IceCandidateInterface> candidate);
  void HopHandleIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState state);
  void HopHandleIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState state);
  void HopHandleRenegotiationNeeded();
  void HopHandleCreateOfferSuccess(std::string sdp_type, std::string sdp);
  void HopHandleCreateOfferFailure(std::string reason);
  void HopHandleSetLocalDescriptionComplete(bool ok, std::string reason);
  void HopHandleSetRemoteDescriptionComplete(bool ok, std::string reason);

  // Envelope emit helpers.
  void SendOfferEnvelope(const webrtc::SessionDescriptionInterface& desc);
  void SendIceCandidateEnvelope(
      const webrtc::IceCandidateInterface& candidate);
  void SendIceEndOfCandidates();
  // R6: emit a `bye` envelope. The wire contract omits the `data`
  // field entirely (not null, not {}; see cb_wire_envelope.h:27-28),
  // so the codec layer produces a two-field {type, from} JSON object.
  // Returns false if the ws Send rejected — caller logs but proceeds
  // with teardown regardless.
  bool SendByeEnvelope();

  // R6: renegotiation orchestrator. Three call sites converge here:
  // RequestRenegotiation(), HopHandleRenegotiationNeeded() (post-
  // initial), and HandleRequestRenegotiateEnvelope(). |trigger| is
  // logged + propagated to OnRenegotiationStarted for embedder
  // observability ("embedder" / "libwebrtc" / "remote").
  //
  // Behavior by current state:
  //   * kIceInFlight              — transition to kCreatingOffer,
  //                                 issue pc_->CreateOffer, fire
  //                                 observer_->OnRenegotiationStarted.
  //   * kCreatingOffer..kSettingRemote — set pending_renegotiation_;
  //                                 BeginRenegotiation() will be
  //                                 re-invoked on the next return to
  //                                 kIceInFlight.
  //   * any terminal/pre-ICE state — log + drop.
  void BeginRenegotiation(std::string_view trigger);

  // R6: clean-teardown helper. Idempotent in kClosed/kFailed.
  // Used by both the public Close() entry point and the inbound `bye`
  // envelope path. |source| is logged + propagated to OnClosed.
  void CloseInternal(std::string_view reason, std::string_view source);

  // Terminal failure helper. Drops the PC, transitions to kFailed,
  // fires observer_->OnFailed.
  void FailWithReason(std::string_view reason);

  // Construction-time inputs.
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pcf_;
  raw_ptr<SignalingWsClient> ws_client_;
  webrtc::PeerConnectionInterface::RTCConfiguration ice_config_;
  raw_ptr<OffererDriverObserver> observer_;
  scoped_refptr<base::SequencedTaskRunner> ui_runner_;

  // Established on Start(); released on dtor / FailWithReason / bye.
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;

  OffererState state_ = OffererState::kIdle;

  // Latch: OnRenegotiationNeeded() fires once on the initial transceiver
  // adds; the first fire transitions kCreatingPc → kCreatingOffer. In
  // R4 this latch stayed set forever (single-offer contract). In R6 it
  // is still set forever — once the initial dance kicks off, subsequent
  // OnRenegotiationNeeded fires no longer drive the initial-dance
  // branch; instead they route to BeginRenegotiation() iff state_ is
  // kIceInFlight (the steady-state guard). The latch is therefore a
  // pure "have we left kCreatingPc?" marker now, NOT a "no more
  // negotiations allowed" gate.
  bool initial_renegotiation_consumed_ = false;

  // R6: coalesced renegotiation request. If a trigger fires while
  // state_ is anywhere other than kIceInFlight (e.g. mid-dance), we
  // set this flag and re-invoke BeginRenegotiation() on the next
  // transition back to kIceInFlight (in HopHandleSetRemoteDescription
  // Complete on the renegotiated dance, or after the initial dance
  // completes).
  bool pending_renegotiation_ = false;

  // R6: terminal-state guard for the public Close() / CloseInternal()
  // path. Once set true, further Close() calls and inbound `bye`
  // envelopes are no-ops; this prevents the observer's OnClosed from
  // firing more than once if both the embedder and the portal client
  // initiate teardown concurrently.
  bool teardown_emitted_ = false;

  // R6: distinguishes the initial dance from renegotiated ones at the
  // kSettingRemote → kIceInFlight transition. Set true the first time
  // we reach kIceInFlight; thereafter any subsequent reach is a
  // renegotiation, which fires OnRenegotiationCompleted on the
  // observer. We don't reset it on teardown — once true, always true.
  bool was_in_ice_flight_once_ = false;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<CbOffererDriver> weak_factory_{this};
};

}  // namespace cloud_browser::signaling

#endif  // CAPTURE_SIGNALING_CB_OFFERER_DRIVER_H_

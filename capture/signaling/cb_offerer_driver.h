// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Native browser-process offerer handshake driver — M3 R4 (CV2-54).
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
// answerer. Renegotiation is unsupported in v1 (R6 territory; not
// touched here).
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

// Optional terminal-failure + ICE-state observer. The embedder may
// pass nullptr to use VLOG / LOG(ERROR) only. R7 (CV2-57) wires
// reconnect on top of OnFailed.
class OffererDriverObserver {
 public:
  virtual ~OffererDriverObserver() = default;

  // ICE connection state transitions surfaced verbatim — the embedder
  // uses kIceConnectionConnected to gate "session is live" metrics
  // and the M6 stats relay's start signal. Fires on the driver's UI
  // thread.
  virtual void OnIceConnectionStateChanged(
      webrtc::PeerConnectionInterface::IceConnectionState state) {}

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
      rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pcf,
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
      rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override;
  void OnAddTrack(
      rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
      const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>&
          streams) override;
  void OnTrack(
      rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)
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

  // Terminal failure helper. Drops the PC, transitions to kFailed,
  // fires observer_->OnFailed.
  void FailWithReason(std::string_view reason);

  // Construction-time inputs.
  rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pcf_;
  raw_ptr<SignalingWsClient> ws_client_;
  webrtc::PeerConnectionInterface::RTCConfiguration ice_config_;
  raw_ptr<OffererDriverObserver> observer_;
  scoped_refptr<base::SequencedTaskRunner> ui_runner_;

  // Established on Start(); released on dtor / FailWithReason / bye.
  rtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;

  OffererState state_ = OffererState::kIdle;

  // Latch: OnRenegotiationNeeded() fires once on the initial transceiver
  // adds; subsequent fires (e.g. M2 R5 capture-lifecycle resume) MUST
  // NOT trigger a second CreateOffer in v1. R6 territory.
  bool first_renegotiation_consumed_ = false;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<CbOffererDriver> weak_factory_{this};
};

}  // namespace cloud_browser::signaling

#endif  // CAPTURE_SIGNALING_CB_OFFERER_DRIVER_H_

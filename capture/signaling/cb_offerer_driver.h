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
//   2. Start() calls pcf->CreatePeerConnectionOrError(ice_config, deps)
//      synchronously on the embedder/UI thread (safe — Start() runs in
//      the embedder's non-task PreMainMessageLoopRun context; see the
//      Threading section). The returned PeerConnectionInterface fans
//      observer callbacks (OnIceCandidate / OnRenegotiationNeeded /
//      OnIceConnectionChange / ...) back to us on libwebrtc's
//      signaling thread — we PostTask onto ui_runner_ before touching
//      state.
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
// ## Invocation hop — PeerConnection ops onto the signaling thread
//   (CV2-69 functional re-test #3, 2026-05-18)
//
// The PeerConnection returned by libwebrtc is a *proxy*: every method
// is generated to run on the signaling thread. Called from any OTHER
// thread, the proxy performs a synchronous *blocking* thread-hop
// (Thread::BlockingCall) that waits on a //base sync primitive.
//
// The driver's hop-landing points (HopHandle*) and inbound-envelope
// handlers run as posted tasks on ui_runner_. chromium installs
// DisallowBaseSyncPrimitives + DisallowBlocking for the duration of
// every sequenced task — so a blocking proxy hop from inside one of
// those tasks trips base/threading/thread_restrictions.cc's DCHECK
// and FATALs the process (this is exactly what killed CreateOffer in
// re-test #3 — the embedder's own AddTransceiver/CreateDataChannel
// survived only because they run in the non-task PreMainMessageLoopRun
// context where the per-task disallow is NOT active).
//
// Fix: every PeerConnection mutation issued from a posted-task context
// — CreateOffer, SetLocalDescription, SetRemoteDescription,
// AddIceCandidate — is wrapped in signaling_thread_->PostTask(). On the
// signaling thread the proxy sees current==signaling and runs the call
// inline: no BlockingCall, no sync-primitive wait, no DCHECK. The SDP-
// observer adapters already hop the *return* path (signaling→ui_runner_);
// this is the symmetric *invocation* hop (ui→signaling).
//
// Start()'s CreatePeerConnectionOrError is deliberately NOT marshalled:
// it runs synchronously in the embedder's non-task PreMainMessageLoopRun
// context (no per-task disallow), and the embedder calls pc() on the
// very next line to AddTransceiver — making PC creation async would
// break that synchronous contract. Same reasoning covers the embedder's
// own AddTransceiver + CreateDataChannel calls.
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
#include <optional>
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
#include "capture/signaling/cb_signaling_transport.h"
#include "capture/signaling/cb_signaling_ws_client.h"
#include "capture/signaling/cb_wire_envelope.h"

namespace webrtc {
// Forward-declared — the driver holds a raw_ptr<webrtc::Thread> to the
// PCF's signaling thread and posts PeerConnection invocations onto it
// (CV2-69 re-test #3 fix). The full rtc_base/thread.h is pulled in by
// the .cc only; header consumers don't need it.
class Thread;
}  // namespace webrtc

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

  // A NEW viewer is asking for an offer that this session cannot give it.
  //
  // Fires when an inbound `request_renegotiate` arrives while we are in
  // kIceInFlight but our current offer was NEVER ANSWERED. That combination
  // means the peer asking is not the peer we are negotiating with: the
  // previous viewer left without a bye, and a fresh one has joined, found the
  // broker's replay buffer empty (the buffered offer aged out at
  // iceReplayMaxAge), and run its offer watchdog.
  //
  // Renegotiating on the EXISTING PeerConnection is wrong here — its ICE
  // ufrag/pwd and DTLS fingerprint belong to the viewer that is gone, which is
  // exactly the mismatch that produced `iceConnectionState=failed` with relay
  // candidates present on both sides. The embedder must re-arm instead, which
  // builds a fresh PC. Fires on the driver's UI thread.
  virtual void OnNewViewerNeedsOffer() {}

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

// chromium-7727 / CV2-69 cleanup (#176): CbOffererDriver inherits ONLY
// the two NON-refcounted observer interfaces (SignalingClientObserver +
// PeerConnectionObserver). It deliberately does NOT inherit the three
// refcounted webrtc SDP-observer interfaces (CreateSessionDescription
// Observer, SetLocal/RemoteDescriptionObserverInterface) — each of
// those non-virtually derives webrtc::RefCountInterface, so inheriting
// all three would give CbOffererDriver three distinct RefCountInterface
// base subobjects and an ambiguous Release()/AddRef() (the
// scoped_refptr<CbOffererDriver> diamond surfaced by CV2-69's first-
// ever instantiation of the driver). Instead, the three SDP-observer
// callbacks are delivered via three small dedicated refcounted adapter
// objects (nested classes below) — each implements exactly ONE
// observer interface (hence exactly one RefCountInterface), is
// make_ref_counted individually, and forwards to the driver's
// HopHandle* landing points via a UI-thread-safe WeakPtr post. This is
// the libwebrtc-idiomatic pattern: a long-lived embedder-owned driver
// is NOT itself a refcounted SDP observer; transient per-operation
// adapters are.
class CbOffererDriver
    : public SignalingClientObserver,
      public webrtc::PeerConnectionObserver {
 public:
  // |pcf|:        from M1's CreateCloudBrowserPcf; the driver takes a
  //               scoped_refptr to keep it alive across its own lifetime.
  // |signaling_thread|: the libwebrtc signaling thread that |pcf| was
  //               built on (main_parts owns it as signaling_thread_).
  //               PeerConnection proxy methods (CreateOffer /
  //               SetLocal/RemoteDescription / AddIceCandidate) MUST
  //               originate on this thread — see the Threading section
  //               above. The driver does NOT own it; it must outlive
  //               the driver (main_parts tears it down strictly after
  //               offerer_driver_ in PostMainMessageLoopRun).
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
      webrtc::Thread* signaling_thread,
      SignalingTransport* ws_client,
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

  // CV2-REARM: return a closed driver to kIdle so the SAME process can
  // serve a NEW viewer, keeping the browser's state (open tabs, scroll
  // position, in-memory logins).
  //
  // Before this, a worker offered exactly ONCE per process. `kClosed` is
  // terminal, `BeginRenegotiation` refuses to run from it, and the
  // embedder's native_session_started_ latch blocked a second
  // Cb.startNativeSession — so the only cure for "viewer left" was
  // process exit + supervisord respawn, which destroys everything the
  // user had open. Measured 2026-08-20: a viewer arriving at an idle
  // stack got no offer at all and sat at "waiting for offer" until the
  // page was closed, because nothing could re-offer.
  //
  // Rearm() only resets SESSION state. It deliberately does NOT touch:
  //   * ws_connected_ / the SignalingWsClient — the socket outlives the
  //     session; a re-arm mid-connection must not re-handshake.
  //   * was_in_ice_flight_once_ — "have we ever negotiated" is a
  //     process-lifetime fact the R6 renegotiation path keys off.
  //   * observer_, threads, pcf_, ice_config_ — construction-time deps.
  //
  // Returns false (and logs) if called from a state where re-arming
  // makes no sense: kIdle (nothing to re-arm) or kFailed (the PC is
  // gone for a reason that will recur; a fresh process is the honest
  // answer there).
  //
  // The caller MUST rebuild what it added to the previous PC —
  // transceivers, data channels, the video track source — exactly as it
  // does after Start(). See CloudBrowserBrowserMainParts::RearmSession.
  // |announce_bye| controls whether the close that precedes the re-arm emits a
  // `bye` on the wire.
  //
  //   true  — the previous viewer is GONE (an ICE-failure teardown, an
  //           embedder-driven recycle). The bye tells the broker to discard
  //           BOTH replay buffers so a late-joining viewer cannot be handed
  //           the dead offer. See signaling/server.go's discardReplay.
  //   false — we are re-arming FOR a viewer that is connected and waiting
  //           (the cold-arrival path). The broker forwards the bye straight to
  //           that viewer, whose client treats it as "session over" and tears
  //           down — so announcing would kill the very peer we are rebuilding
  //           for. Measured 2026-08-21: the re-arm completed in 9ms and the
  //           viewer went `connecting` -> `closed` without ever seeing the
  //           fresh offer.
  bool Rearm(bool announce_bye = true);

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

  // CV2-GPU-DEATH: like Close(), but first emits a session_unhealthy envelope
  // so physics releases this element's isolation-registry entry (the next
  // allocate_or_reuse mints a FRESH guest instead of reusing this dead one).
  // Called by main_parts when the BeginFrame driver reports permanent
  // renderer/GPU death (run11-class: ack-loop healthy, zero production for
  // 30s+). Distinct from Close() so a user-initiated close does not trigger a
  // recycle. Safe from posted-task contexts (same teardown as Close()).
  void CloseUnhealthy(std::string_view reason);

  // State accessor for tests + the embedder's readiness gate.
  OffererState state() const;

  // Direct PeerConnection access for the embedder's add-tracks step.
  // Returned ref is valid until the driver enters kFailed or is
  // destructed. After Start() the PC is logically owned by the
  // driver and external mutation races the offerer dance — don't.
  webrtc::PeerConnectionInterface* pc() const;

  // Marshaled PeerConnection::GetStats(). The embedder's RTP-stats poll
  // runs on the UI thread (driven by a RepeatingTimer), where chromium's
  // per-task DisallowBaseSyncPrimitives is installed; calling pc()->GetStats
  // directly from there trips the proxy's blocking thread-hop DCHECK and
  // FATALs the worker (thread_restrictions.cc:166) the instant ICE connects.
  // Routing through the driver keeps GetStats on signaling_thread_ with the
  // same [pc = pc_] ref-capture discipline as every other PC proxy call here,
  // so the scoped_refptr keeps the PC alive across the async stats delivery.
  // No-op if the PC is gone.
  void PollOutboundStats(
      webrtc::scoped_refptr<webrtc::RTCStatsCollectorCallback> callback);

  // SignalingClientObserver — inbound from the ws client. Already on
  // the UI thread per the M3 R2 contract.
  //
  // NOTE: main_parts is the *registered* ws observer; it forwards
  // these to the driver (resolves the SignalingWsClient ↔ driver
  // construction-order cycle — see the header member-block in
  // cloud_browser_browser_main_parts.h). OnConnected is forwarded the
  // same way: CV2-69 re-test#4 found the offer could be produced
  // before the WS handshake completes, so the driver now defers offer
  // emission until OnConnected (see EmitOfferAndSetLocal +
  // pending_local_offer_).
  void OnConnected() override;
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

 private:
  // chromium-7727 / CV2-69 (#176) — refcounted SDP-observer adapters.
  // Each implements exactly ONE webrtc SDP-observer interface, is
  // constructed via webrtc::make_ref_counted at the CreateOffer /
  // SetLocalDescription / SetRemoteDescription call site, and forwards
  // the libwebrtc-signaling-thread callback to the driver's HopHandle*
  // landing points via a ui_runner_ post bound to a
  // base::WeakPtr<CbOffererDriver> (UI-thread-safe; the WeakPtr is
  // dereferenced only inside the posted task, on ui_runner_'s
  // sequence). Nested classes so they retain private access to the
  // driver's HopHandle* members; defined out-of-line in the .cc.
  class CreateOfferObserver;
  class SetLocalDescObserver;
  class SetRemoteDescObserver;
  // Inbound dispatch helpers. Routes |env| based on env.type with
  // state-guard enforcement; protocol violations route to
  // FailWithReason.
  void HandleOfferEnvelope(const Envelope& env);
  void HandleAnswerEnvelope(const Envelope& env);
  void HandleIceEnvelope(const Envelope& env);
  void HandleByeEnvelope();
  void HandleRequestRenegotiateEnvelope();
  void HandleProbeResultEnvelope(const Envelope& env);

  // Remote ICE can arrive immediately after the portal sends its
  // answer, before our async SetRemoteDescription(answer) completion
  // has transitioned the PeerConnection into kIceInFlight. Retain
  // those candidates and replay them once the remote SDP is applied.
  void QueueRemoteIceCandidate(const Envelope& env);
  void FlushPendingRemoteIce();
  void AddRemoteIcePayload(const IceCandidatePayload& payload);

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

  // CV2-69 re-test#4 (Finding A): emit the `offer` envelope onto the
  // wire and issue SetLocalDescription. Split out of
  // HopHandleCreateOfferSuccess so it can be invoked either inline
  // (WS already connected) or deferred to OnConnected (WS not yet
  // connected — the offer is stashed in pending_local_offer_ and this
  // runs when the handshake completes). |local| is the freshly
  // CreateOffer'd SDP; ownership moves in. Caller guarantees
  // state_ == kCreatingOffer.
  void EmitOfferAndSetLocal(
      std::unique_ptr<webrtc::SessionDescriptionInterface> local);

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

  // CV2-GPU-DEATH: emit the session_unhealthy envelope (monostate payload,
  // `data` omitted like bye). Called from CloseUnhealthy() before teardown.
  bool SendSessionUnhealthyEnvelope();

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

  // CV2-69 re-test#4 (Finding B): terminate pc_ from FailWithReason /
  // CloseInternal — both posted-task-reachable, where chromium's
  // per-task DisallowBaseSyncPrimitives is installed. A naive
  // `pc_ = nullptr` here would (1) call PeerConnection::Close() — a
  // proxy method → blocking hop — and (2) run the proxy destructor,
  // which also blocking-hops to the signaling thread (the re-test #4
  // FATAL site). This helper instead marshals Close() onto the
  // signaling thread and RETAINS the pc_ ref: the PeerConnection
  // holds the driver as its PeerConnectionObserver, so it must not
  // outlive the driver. pc_ is destroyed synchronously by
  // ~CbOffererDriver — that runs in the embedder's non-task
  // PostMainMessageLoopRun context where the proxy dtor's blocking
  // hop is allowed, and the dtor drops pc_ before weak_factory_ so
  // the PC is fully gone before the driver.
  void ClosePcOnSignalingThread();

  // Construction-time inputs.
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pcf_;
  // The libwebrtc signaling thread |pcf_| was built on. PeerConnection
  // proxy invocations are PostTask()ed onto this thread so they
  // originate same-thread and the proxy runs them inline (no blocking
  // BlockingCall hop). Not owned — main_parts owns + outlives it.
  raw_ptr<webrtc::Thread> signaling_thread_;
  // SignalingTransport, not SignalingWsClient: the driver calls exactly
  // one method on it (Send, four sites), so it has no reason to depend on
  // the chromium-network-service implementation. See
  // cb_signaling_transport.h for why the interface is one virtual.
  raw_ptr<SignalingTransport> ws_client_;
  webrtc::PeerConnectionInterface::RTCConfiguration ice_config_;
  raw_ptr<OffererDriverObserver> observer_;
  scoped_refptr<base::SequencedTaskRunner> ui_runner_;

  // Established on Start(); released on dtor / FailWithReason / bye.
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;

  std::vector<IceCandidatePayload> pending_remote_ice_;

  // CV2-ICE early-answer buffer (RCA 2026-06-30, kSettingLocal gap).
  // On the cross-pod physics path the `answer` envelope can arrive
  // BEFORE our own SetLocalDescription completes — i.e. while state_ is
  // still kCreatingOffer / kSettingLocal, strictly EARLIER in the enum
  // than kAwaitingAnswer. The prior dup-tolerance guard only covered the
  // too-LATE window (kSettingRemote / kIceInFlight); a too-EARLY answer
  // fell through to FailWithReason → teardown → guest SIGABRT (respR=0).
  // This is the legitimate first answer, NOT a redundant duplicate, so
  // it must be BUFFERED (not dropped) and replayed once SLD completes and
  // we reach kAwaitingAnswer. HopHandleSetLocalDescriptionComplete drains
  // it. Holds the SDP only (the answer payload); empty == none buffered.
  std::optional<std::string> pending_early_answer_sdp_;

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

  // True once the CURRENT offer has been answered by a peer. Distinct from
  // was_in_ice_flight_once_, which is a "have we ever negotiated" latch that
  // deliberately survives Rearm().
  //
  // This one is per-offer: set when an `answer` is applied, cleared by
  // Rearm() and by every fresh CreateOffer. It is what lets an inbound
  // `request_renegotiate` in kIceInFlight distinguish
  //   * our own viewer asking to refresh SDP (answered -> renegotiate), from
  //   * a NEW viewer that never answered, asking for an offer it never got
  //     (unanswered -> the embedder must re-arm; see OnNewViewerNeedsOffer).
  bool current_offer_answered_ = false;

  // CV2-69 re-test#4 (Finding A): WS-connected gate for offer
  // emission. ws_connected_ flips true on OnConnected (forwarded by
  // main_parts). If CreateOffer completes before the WS handshake
  // does, HopHandleCreateOfferSuccess stashes the SDP in
  // pending_local_offer_ instead of Send()ing it (which would fail —
  // the socket is not up); OnConnected then drains it via
  // EmitOfferAndSetLocal. SetLocalDescription + ICE gathering are
  // deferred along with the offer, so no ICE candidate is produced
  // before the wire is live.
  bool ws_connected_ = false;
  std::unique_ptr<webrtc::SessionDescriptionInterface> pending_local_offer_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<CbOffererDriver> weak_factory_{this};
};

}  // namespace cloud_browser::signaling

#endif  // CAPTURE_SIGNALING_CB_OFFERER_DRIVER_H_

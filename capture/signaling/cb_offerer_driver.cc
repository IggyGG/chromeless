// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cb_offerer_driver.cc — see cb_offerer_driver.h.
//
// R6 (CV2-56) amendments — renegotiation + explicit teardown:
//   * Renegotiation dance re-enters from kIceInFlight via
//     BeginRenegotiation(); three trigger sites converge (embedder
//     RequestRenegotiation, post-initial OnRenegotiationNeeded,
//     inbound `request_renegotiate` envelope).
//   * Clean teardown via Close() / inbound `bye` flows through
//     CloseInternal() + SendByeEnvelope(); the prior R4 path that
//     handled inbound `bye` is collapsed into CloseInternal().
//   * Observer gains OnRenegotiationStarted / OnRenegotiationCompleted /
//     OnClosed hooks; OnFailed semantics unchanged.

#include "capture/signaling/cb_offerer_driver.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "api/jsep.h"
#include "api/jsep_ice_candidate.h"
#include "api/make_ref_counted.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "rtc_base/thread.h"  // CV2-69 re-test#3: signaling-thread PostTask

namespace cloud_browser::signaling {

namespace {

// Log prefix — "CloudBrowser:" lineage matches the M0 R5 scrape regex
// family already used by FormatPcfVideoCodecLogLine. The qualifier
// "M3R4/R6 offerer driver:" covers both the R4 base class + the R6
// renegotiation/teardown amendments; grep on "M3R4/R6 offerer driver:"
// is precise enough for either layer.
constexpr char kLogPrefix[] = "CloudBrowser: M3R4/R6 offerer driver: ";

const char* StateName(OffererState s) {
  switch (s) {
    case OffererState::kIdle:           return "Idle";
    case OffererState::kCreatingPc:     return "CreatingPc";
    case OffererState::kCreatingOffer:  return "CreatingOffer";
    case OffererState::kSettingLocal:   return "SettingLocal";
    case OffererState::kAwaitingAnswer: return "AwaitingAnswer";
    case OffererState::kSettingRemote:  return "SettingRemote";
    case OffererState::kIceInFlight:    return "IceInFlight";
    case OffererState::kClosed:         return "Closed";
    case OffererState::kFailed:         return "Failed";
  }
  // Unreachable — closed enum, exhaustively matched above. CHECK
  // mirrors cb_wire_envelope.cc's TagToString tail so a future enum
  // expansion crashes loudly in debug builds rather than silently
  // logging "Unknown".
  CHECK(false) << "unhandled OffererState";
  return "Unknown";
}

}  // namespace

// ---------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------

CbOffererDriver::CbOffererDriver(
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pcf,
    webrtc::Thread* signaling_thread,
    SignalingTransport* ws_client,
    webrtc::PeerConnectionInterface::RTCConfiguration ice_config,
    OffererDriverObserver* observer,
    scoped_refptr<base::SequencedTaskRunner> ui_runner)
    : pcf_(std::move(pcf)),
      signaling_thread_(signaling_thread),
      ws_client_(ws_client),
      ice_config_(std::move(ice_config)),
      observer_(observer),
      ui_runner_(std::move(ui_runner)) {
  DCHECK(pcf_);
  DCHECK(signaling_thread_);
  DCHECK(ws_client_);
  DCHECK(ui_runner_);
}

CbOffererDriver::~CbOffererDriver() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Drop the PC ref BEFORE the WeakPtrFactory destructs so any
  // in-flight signaling-thread callback resolves to a no-op WeakPtr
  // dispatch rather than a UAF. libwebrtc's Close() is implicit on
  // ref-drop; we don't need to call pc_->Close() explicitly.
  pc_ = nullptr;
}

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

// CV2-REARM: see the header for why this exists and what it must NOT reset.
bool CbOffererDriver::Rearm(bool announce_bye) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (state_ == OffererState::kIdle) {
    VLOG(1) << kLogPrefix << "Rearm() ignored: already kIdle";
    return false;
  }
  if (state_ == OffererState::kFailed) {
    // A failed driver failed for a reason (PC creation refused, SDP
    // rejected, protocol violation). Re-arming would paper over it and
    // most likely fail again on the next viewer; a fresh process is the
    // honest answer, and OnFailed has already told the embedder.
    LOG(WARNING) << kLogPrefix
                 << "Rearm() refused from kFailed — a fresh process is needed";
    return false;
  }

  // Not yet closed (the viewer vanished without a bye and ICE has not
  // finished rotting). Close first so the teardown path runs exactly
  // once, in its documented order, before we drop the PC.
  if (state_ != OffererState::kClosed) {
    VLOG(1) << kLogPrefix << "Rearm() from state=" << StateName(state_)
            << " — closing first";
    // The close source decides whether a `bye` goes out, and BOTH choices are
    // load-bearing (CloseInternal skips the bye for source "ws"/"remote"):
    //
    //   announce_bye=true  -> source "rearm". The previous viewer is gone, so
    //     the bye tells the broker to discard both replay buffers and a
    //     late-joining viewer cannot be handed the dead offer.
    //     (signaling/server.go, discardReplay on inbound bye.)
    //
    //   announce_bye=false -> source "remote", which suppresses the bye. Used
    //     when re-arming FOR a viewer that is already connected and waiting.
    //     The broker forwards a bye straight to that viewer, whose client
    //     calls teardown("peer said bye") — so announcing here kills the very
    //     peer we are rebuilding for. Measured 2026-08-21: the re-arm
    //     completed in 9ms and the viewer went `connecting` -> `closed`
    //     without ever seeing the fresh offer that was built for it.
    CloseInternal("rearm", announce_bye ? "rearm" : "remote");
  }

  // Release OUR ref to the old PC — but ON THE SIGNALING THREAD, never here.
  //
  // Two hazards, both documented at ClosePcOnSignalingThread:
  //   1. The proxy DESTRUCTOR blocking-hops to the signaling thread. That is
  //      the "re-test #4 FATAL site". Rearm() is reachable from posted-task
  //      contexts (an inbound `bye` arrives that way), where chromium's
  //      per-task DisallowBaseSyncPrimitives makes a blocking hop fatal. A
  //      bare `pc_ = nullptr` here can therefore kill the worker outright if
  //      this happens to hold the last ref.
  //   2. The PC holds THIS DRIVER as its PeerConnectionObserver by raw
  //      pointer, so the PC must never outlive the driver. That still holds:
  //      the driver is unique_ptr-owned by the embedder and is NOT destroyed
  //      on the re-arm path, so it outlives this posted release by the whole
  //      remaining life of the process.
  //
  // Moving the ref into the task means the final Release() happens on the
  // signaling thread, where the dtor's hop is a no-op.
  if (pc_) {
    signaling_thread_->PostTask([pc = std::move(pc_)]() mutable {
      pc = nullptr;
    });
  }
  pc_ = nullptr;  // moved-from above; explicit so the state is unambiguous.

  // CUT EVERY IN-FLIGHT CALLBACK FROM THE OLD PEERCONNECTION.
  //
  // The SDP observer adapters (CreateOfferObserver / SetLocalDescObserver /
  // SetRemoteDescObserver) are refcounted by libwebrtc and outlive us; they
  // hop back through a WeakPtr. A CreateOffer still in flight when the viewer
  // left would otherwise land DURING the next session and do one of two bad
  // things: if the new session happens to be in kCreatingOffer it is accepted,
  // putting the DEAD PC's ufrag/pwd and DTLS fingerprint on the wire for the
  // new viewer; in any other state it hits "CreateOffer success in unexpected
  // state" -> FailWithReason -> the worker dies.
  //
  // Invalidating here drops all of them at once. Safe because every WeakPtr is
  // taken fresh at post time (8 call sites, all `weak_factory_.GetWeakPtr()`
  // inline in a Bind), so the new session's hops get valid pointers — nothing
  // holds one across the re-arm boundary.
  weak_factory_.InvalidateWeakPtrs();

  // Session-scoped state only.
  state_ = OffererState::kIdle;
  teardown_emitted_ = false;
  initial_renegotiation_consumed_ = false;
  pending_renegotiation_ = false;
  pending_remote_ice_.clear();
  pending_local_offer_.reset();
  // An early answer buffered for the OLD offer would otherwise be replayed
  // against the NEW local SDP by HopHandleSetLocalDescriptionComplete, fail
  // SetRemoteDescription, and kill the driver via FailWithReason.
  pending_early_answer_sdp_.reset();
  current_offer_answered_ = false;

  LOG(INFO) << kLogPrefix
            << "CV2-REARM: driver returned to kIdle; the embedder must now "
               "rebuild transceivers + data channels and call Start()";
  return true;
}

void CbOffererDriver::Start() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != OffererState::kIdle) {
    VLOG(1) << kLogPrefix << "Start() ignored, state=" << StateName(state_);
    return;
  }
  state_ = OffererState::kCreatingPc;

  // CV2-69 re-test#3 threading note: CreatePeerConnectionOrError below
  // is called synchronously on the embedder/UI thread and is NOT
  // marshalled onto the signaling thread (unlike CreateOffer /
  // SetLocal/RemoteDescription / AddIceCandidate — see the header
  // Threading section). Two reasons it is safe + must stay synchronous:
  //   (a) Start() runs in the embedder's PreMainMessageLoopRun, which
  //       is NOT a sequenced-task context — chromium's per-task
  //       DisallowBaseSyncPrimitives is not installed there, so the
  //       proxy's blocking thread-hop does not trip the DCHECK.
  //   (b) the embedder calls pc() on the very next lines to
  //       AddTransceiver / CreateDataChannel; making PC construction
  //       async would hand back a null pc(). The synchronous contract
  //       is load-bearing.
  //
  // chromium-7727 API drift (CV2-69 cleanup, #176): the legacy
  // four-arg CreatePeerConnection(config, allocator, cert_generator,
  // observer) overload is REMOVED from PeerConnectionFactoryInterface.
  // The modern API is CreatePeerConnectionOrError(RTCConfiguration,
  // PeerConnectionDependencies) returning
  // RTCErrorOr<scoped_refptr<PeerConnectionInterface>> — the error
  // form surfaces the failure reason verbatim instead of a bare
  // nullptr. PeerConnectionDependencies is the deps-struct that
  // bundles the PeerConnectionObserver (and optional allocator /
  // cert_generator / async-resolver, all left at defaults here).
  webrtc::PeerConnectionDependencies pc_dependencies(/*observer=*/this);
  // CV2-ICE observability: log the EXACT IceServers + transport policy
  // handed to the PeerConnection right before creation. The Gate 6 ICE
  // stall is the guest never creating a TurnPort; this confirms whether
  // the credentialed TURN server actually survives into rtc_config the
  // BasicPortAllocator sees (urls + has-username + has-credential), vs
  // being dropped/stripped before allocation.
  LOG(INFO) << kLogPrefix << "CV2-ICE PC config: type="
            << static_cast<int>(ice_config_.type)
            << " servers=" << ice_config_.servers.size();
  for (const auto& srv : ice_config_.servers) {
    std::string urls;
    for (const auto& u : srv.urls) {
      if (!urls.empty()) urls += ",";
      urls += u;
    }
    LOG(INFO) << kLogPrefix << "CV2-ICE   server urls=[" << urls
              << "] has_username=" << (!srv.username.empty())
              << " has_credential=" << (!srv.password.empty());
  }
  webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::PeerConnectionInterface>>
      pc_or_error = pcf_->CreatePeerConnectionOrError(
          ice_config_, std::move(pc_dependencies));
  if (!pc_or_error.ok()) {
    FailWithReason(std::string("CreatePeerConnectionOrError failed: ") +
                   std::string(pc_or_error.error().message()));
    return;
  }
  pc_ = pc_or_error.MoveValue();
  if (!pc_) {
    FailWithReason("CreatePeerConnectionOrError returned ok() but a null PC");
    return;
  }

  // CreateOffer is NOT issued here. The embedder will now AddTrack /
  // AddTransceiver / CreateDataChannel onto pc_; the resulting
  // OnRenegotiationNeeded() callback is what triggers CreateOffer.
  // This ordering is what guarantees M2's video transceiver + M4's
  // input DC + M5's cursor DC are baked into the first SDP.
  VLOG(1) << kLogPrefix << "PC constructed; awaiting initial "
                           "OnRenegotiationNeeded after embedder track adds";
}

void CbOffererDriver::RequestRenegotiation() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ == OffererState::kClosed ||
      state_ == OffererState::kFailed) {
    VLOG(1) << kLogPrefix
            << "RequestRenegotiation ignored, state=" << StateName(state_);
    return;
  }
  BeginRenegotiation("embedder");
}

void CbOffererDriver::Close(std::string_view reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CloseInternal(reason, "embedder");
}

void CbOffererDriver::CloseUnhealthy(std::string_view reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // CV2-GPU-DEATH: the guest self-detected permanent renderer/GPU death. Emit
  // the session_unhealthy envelope FIRST (best-effort; ws_client_ may already
  // be gone) so physics releases this element's registry entry and re-pins a
  // fresh guest, THEN run the normal teardown. Distinct from Close() so that a
  // user-initiated close (guest is fine) does not blacklist the allocation.
  if (ws_client_) {
    SendSessionUnhealthyEnvelope();
  }
  CloseInternal(reason, "gpu-permanent-death");
}

OffererState CbOffererDriver::state() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return state_;
}

webrtc::PeerConnectionInterface* CbOffererDriver::pc() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return pc_.get();
}

void CbOffererDriver::PollOutboundStats(
    webrtc::scoped_refptr<webrtc::RTCStatsCollectorCallback> callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!pc_ || !signaling_thread_ || !callback) {
    return;
  }
  // Same marshaling discipline as CreateOffer / SetLocalDescription /
  // AddIceCandidate above: hop onto signaling_thread_ so the PC proxy's
  // blocking dispatch does NOT run under the UI thread's
  // DisallowBaseSyncPrimitives. Capture [pc = pc_] (a scoped_refptr) so
  // the PeerConnection stays alive across the async GetStats call even
  // if teardown races; libwebrtc holds callback alive until OnStatsDelivered.
  signaling_thread_->PostTask(
      [pc = pc_, callback]() { pc->GetStats(callback.get()); });
}

// ---------------------------------------------------------------------
// SignalingClientObserver (inbound from ws client, already on UI thread)
// ---------------------------------------------------------------------

void CbOffererDriver::OnConnected() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // CV2-69 re-test#4 (Finding A): the WS handshake completed — the
  // signaling channel is now duplex and Send() will succeed. If
  // CreateOffer finished before this point, HopHandleCreateOfferSuccess
  // stashed the SDP in pending_local_offer_ rather than Send()ing it
  // into a not-yet-open socket; drain it now.
  ws_connected_ = true;
  VLOG(1) << kLogPrefix << "WS connected; signaling channel is duplex";
  if (pending_local_offer_) {
    // Guard on kCreatingOffer: if a teardown/failure raced in while the
    // offer sat buffered, do NOT resurrect the dance — just drop the
    // stale SDP (the std::move below / the reset both null it).
    if (state_ == OffererState::kCreatingOffer) {
      VLOG(1) << kLogPrefix << "draining buffered offer post-connect";
      EmitOfferAndSetLocal(std::move(pending_local_offer_));
    } else {
      VLOG(1) << kLogPrefix
              << "buffered offer dropped post-connect, state="
              << StateName(state_);
      pending_local_offer_ = nullptr;
    }
  }
}

void CbOffererDriver::OnEnvelope(const Envelope& env) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ == OffererState::kFailed || state_ == OffererState::kClosed) {
    VLOG(1) << kLogPrefix << "envelope dropped post-terminal, type="
            << TagToString(env.type);
    return;
  }
  switch (env.type) {
    case EnvelopeType::kOffer:
      HandleOfferEnvelope(env);
      break;
    case EnvelopeType::kAnswer:
      HandleAnswerEnvelope(env);
      break;
    case EnvelopeType::kIce:
      HandleIceEnvelope(env);
      break;
    case EnvelopeType::kBye:
      HandleByeEnvelope();
      break;
    case EnvelopeType::kRequestRenegotiate:
      HandleRequestRenegotiateEnvelope();
      break;
    case EnvelopeType::kProbeResult:
      HandleProbeResultEnvelope(env);
      break;
    case EnvelopeType::kPeerAbsent:
      // CV2-PEER-ABSENT: the broker telling us no viewer holds the client
      // role yet. For the OFFERER that is the normal cold-boot state — the
      // offer is already buffered at the broker and is replayed to the next
      // viewer — so there is nothing to do but note it. Before this tag was
      // accepted, receiving it was a fatal decode error (see
      // cb_wire_envelope.h), which is the opposite of advisory.
      LOG(INFO) << kLogPrefix << "peer_absent from the broker: no viewer in "
                   "the session yet; offer stays buffered for the next one";
      break;
    case EnvelopeType::kSessionUnhealthy:
      // CV2-GPU-DEATH: this is an OUTBOUND-only type (guest→physics). The
      // guest never legitimately receives it; a peer sending it here is a
      // misuse. Ignore (do NOT treat as bye — that would tear down a healthy
      // session on a stray frame).
      LOG(WARNING) << "CbOffererDriver: ignoring inbound session_unhealthy "
                      "(outbound-only type; guest does not consume it)";
      break;
  }
}

void CbOffererDriver::OnClosed(uint16_t code, std::string_view reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  VLOG(1) << kLogPrefix << "ws closed code=" << code
          << " reason=" << reason;
  // R6: route ws close through the unified teardown path so the
  // embedder receives OnClosed exactly once across the three close
  // origins (embedder Close, remote `bye`, ws close). R7 (CV2-57)
  // will intercept this BEFORE we hit CloseInternal in the reconnect
  // case — for now ws close is terminal.
  CloseInternal(
      std::string("ws closed code=") + std::to_string(code) +
          " reason=" + std::string(reason),
      "ws");
}

void CbOffererDriver::OnError(std::string_view reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  FailWithReason(std::string("ws error: ") + std::string(reason));
}

// ---------------------------------------------------------------------
// PeerConnectionObserver overrides (libwebrtc signaling thread → UI hop)
// ---------------------------------------------------------------------

void CbOffererDriver::OnSignalingChange(
    webrtc::PeerConnectionInterface::SignalingState new_state) {
  VLOG(2) << kLogPrefix
          << "OnSignalingChange state=" << static_cast<int>(new_state);
}

void CbOffererDriver::OnIceCandidate(
    const webrtc::IceCandidateInterface* candidate) {
  // libwebrtc owns |candidate|'s lifetime only for the duration of
  // this callback; clone before posting to our task runner.
  //
  // TODO(M3-R4-ice-candidate-clone): webrtc::CreateIceCandidate
  // returns a raw owning pointer in the chromium-bundled revision we
  // expect; wrap in unique_ptr immediately. If the revision exposes
  // IceCandidateInterface::Clone() instead, swap to that — it
  // preserves the username_fragment + tcptype fields the
  // sdp_mid/mline_index/candidate-string ctor drops.
  // CV2-ICE observability: the deployed build never emits a relay
  // candidate (Gate 6 ICE stall — guest stuck on host candidates only).
  // Log every gathered candidate at INFO so the guest chromeless.log
  // shows its full SDP a-line (typ host / srflx / relay). Seeing only
  // host/srflx here (and never relay) confirms the TurnPort is never
  // created; a relay line would mean gathering works and the drop is
  // downstream.
  {
    std::string cand_sdp;
    candidate->ToString(&cand_sdp);
    LOG(INFO) << kLogPrefix << "CV2-ICE OnIceCandidate mid="
              << candidate->sdp_mid()
              << " mline=" << candidate->sdp_mline_index()
              << " sdp=[" << cand_sdp << "]";
  }
  std::unique_ptr<webrtc::IceCandidateInterface> cloned(
      webrtc::CreateIceCandidate(candidate->sdp_mid(),
                                 candidate->sdp_mline_index(),
                                 candidate->candidate()));
  if (!cloned) {
    LOG(INFO) << kLogPrefix
              << "CV2-ICE OnIceCandidate clone failed; dropping candidate";
    return;
  }
  ui_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&CbOffererDriver::HopHandleIceCandidate,
                     weak_factory_.GetWeakPtr(), std::move(cloned)));
}

void CbOffererDriver::OnIceGatheringChange(
    webrtc::PeerConnectionInterface::IceGatheringState new_state) {
  // CV2-ICE observability: New→Gathering→Complete transitions at INFO.
  LOG(INFO) << kLogPrefix << "CV2-ICE OnIceGatheringChange state="
            << static_cast<int>(new_state);
  ui_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&CbOffererDriver::HopHandleIceGatheringChange,
                     weak_factory_.GetWeakPtr(), new_state));
}

void CbOffererDriver::OnIceConnectionChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
  ui_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&CbOffererDriver::HopHandleIceConnectionChange,
                     weak_factory_.GetWeakPtr(), new_state));
}

void CbOffererDriver::OnConnectionChange(
    webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
  VLOG(1) << kLogPrefix
          << "OnConnectionChange state=" << static_cast<int>(new_state);
}

void CbOffererDriver::OnRenegotiationNeeded() {
  ui_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&CbOffererDriver::HopHandleRenegotiationNeeded,
                     weak_factory_.GetWeakPtr()));
}

void CbOffererDriver::OnDataChannel(
    webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
  // The browser peer is the offerer and never receives inbound
  // DataChannels in v1 — the portal client never opens one. M4 / M5
  // create channels FROM the browser side, observed via their own
  // observers, not this one. Log + drop.
  VLOG(1) << kLogPrefix << "Unexpected inbound DataChannel '"
          << data_channel->label() << "' — ignoring (v1 contract)";
}

void CbOffererDriver::OnAddTrack(
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
    const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&
        streams) {
  // v1 contract: browser sends, never receives. Sanity-log.
  VLOG(1) << kLogPrefix << "OnAddTrack receiver kind="
          << receiver->track()->kind()
          << " — unexpected on offerer side (v1 contract)";
}

void CbOffererDriver::OnTrack(
    webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
  VLOG(2) << kLogPrefix
          << "OnTrack mid=" << transceiver->mid().value_or("-");
}

// ---------------------------------------------------------------------
// Refcounted SDP-observer adapters (chromium-7727 / CV2-69 #176)
// ---------------------------------------------------------------------
//
// CbOffererDriver used to inherit CreateSessionDescriptionObserver +
// SetLocal/RemoteDescriptionObserverInterface directly and pass `this`
// to CreateOffer / SetLocalDescription / SetRemoteDescription. That
// gave the driver three distinct (non-virtual) webrtc::RefCountInterface
// base subobjects and an ambiguous Release()/AddRef() — the
// scoped_refptr<CbOffererDriver> diamond. The fix: three small
// dedicated adapter classes, each implementing exactly ONE observer
// interface (hence exactly one RefCountInterface), each constructed via
// webrtc::make_ref_counted at the call site. Each adapter forwards the
// libwebrtc-signaling-thread callback to the driver's HopHandle*
// landing point via a ui_runner_ post bound to a
// base::WeakPtr<CbOffererDriver>. The WeakPtr is dereferenced ONLY
// inside the posted task, on ui_runner_'s sequence — the adapter
// itself never touches the driver on the signaling thread, so the
// driver may be torn down concurrently without UAF (the posted task
// is simply dropped if the WeakPtr is invalid). This is the same
// hop-and-weak-guard discipline the driver's PeerConnectionObserver
// callbacks already use.

class CbOffererDriver::CreateOfferObserver
    : public webrtc::CreateSessionDescriptionObserver {
 public:
  CreateOfferObserver(base::WeakPtr<CbOffererDriver> driver,
                      scoped_refptr<base::SequencedTaskRunner> ui_runner)
      : driver_(std::move(driver)), ui_runner_(std::move(ui_runner)) {}

  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    // Fires on libwebrtc's signaling thread. Stringify before hopping;
    // libwebrtc retains ownership of |desc| only for this callback.
    std::string sdp_string;
    desc->ToString(&sdp_string);
    std::string sdp_type = desc->type();  // "offer" — we requested one.
    ui_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&CbOffererDriver::HopHandleCreateOfferSuccess, driver_,
                       std::move(sdp_type), std::move(sdp_string)));
  }

  void OnFailure(webrtc::RTCError error) override {
    ui_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&CbOffererDriver::HopHandleCreateOfferFailure, driver_,
                       std::string(error.message())));
  }

 private:
  const base::WeakPtr<CbOffererDriver> driver_;
  const scoped_refptr<base::SequencedTaskRunner> ui_runner_;
};

class CbOffererDriver::SetLocalDescObserver
    : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  SetLocalDescObserver(base::WeakPtr<CbOffererDriver> driver,
                       scoped_refptr<base::SequencedTaskRunner> ui_runner)
      : driver_(std::move(driver)), ui_runner_(std::move(ui_runner)) {}

  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    ui_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&CbOffererDriver::HopHandleSetLocalDescriptionComplete,
                       driver_, error.ok(), std::string(error.message())));
  }

 private:
  const base::WeakPtr<CbOffererDriver> driver_;
  const scoped_refptr<base::SequencedTaskRunner> ui_runner_;
};

class CbOffererDriver::SetRemoteDescObserver
    : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  SetRemoteDescObserver(base::WeakPtr<CbOffererDriver> driver,
                        scoped_refptr<base::SequencedTaskRunner> ui_runner)
      : driver_(std::move(driver)), ui_runner_(std::move(ui_runner)) {}

  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    ui_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&CbOffererDriver::HopHandleSetRemoteDescriptionComplete,
                       driver_, error.ok(), std::string(error.message())));
  }

 private:
  const base::WeakPtr<CbOffererDriver> driver_;
  const scoped_refptr<base::SequencedTaskRunner> ui_runner_;
};

// ---------------------------------------------------------------------
// Hop landing points (UI thread)
// ---------------------------------------------------------------------

void CbOffererDriver::HopHandleIceCandidate(
    std::unique_ptr<webrtc::IceCandidateInterface> candidate) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // R6: ICE flows freely once the initial dance has reached
  // kSettingLocal. During renegotiation the state cycles back through
  // kCreatingOffer / kSettingLocal / kAwaitingAnswer / kSettingRemote,
  // and libwebrtc continues to emit ICE candidates for the existing
  // transceivers throughout. The only hard drop is pre-initial (no
  // SDP yet, broker would have nothing to attach the candidate to)
  // and terminal (kClosed / kFailed).
  const bool pre_initial =
      !was_in_ice_flight_once_ && state_ < OffererState::kSettingLocal;
  const bool terminal = state_ == OffererState::kClosed ||
                        state_ == OffererState::kFailed;
  if (pre_initial || terminal) {
    VLOG(1) << kLogPrefix << "ICE candidate dropped, state="
            << StateName(state_);
    return;
  }
  SendIceCandidateEnvelope(*candidate);
}

void CbOffererDriver::HopHandleIceGatheringChange(
    webrtc::PeerConnectionInterface::IceGatheringState new_state) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  VLOG(1) << kLogPrefix
          << "ICE gathering state=" << static_cast<int>(new_state);
  if (new_state ==
      webrtc::PeerConnectionInterface::kIceGatheringComplete) {
    SendIceEndOfCandidates();
  }
}

void CbOffererDriver::HopHandleIceConnectionChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  VLOG(1) << kLogPrefix
          << "ICE connection state=" << static_cast<int>(new_state);
  if (observer_) {
    observer_->OnIceConnectionStateChanged(new_state);
  }
}

void CbOffererDriver::HopHandleRenegotiationNeeded() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!initial_renegotiation_consumed_) {
    // First fire — initial dance. M2's video transceiver + M4/M5's
    // data channels are already wired by the embedder before this
    // callback fires, so they're folded into the offer automatically.
    initial_renegotiation_consumed_ = true;
    if (state_ != OffererState::kCreatingPc) {
      FailWithReason("OnRenegotiationNeeded in unexpected state");
      return;
    }
    state_ = OffererState::kCreatingOffer;
    current_offer_answered_ = false;  // a brand-new offer has nobody behind it
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions opts;
    // CV2-69 #176: transient refcounted CreateOfferObserver adapter
    // (not `this` — see the adapter block above); CreateOffer AddRefs
    // the observer internally so it survives until libwebrtc releases
    // it after the OnSuccess/OnFailure callback.
    // CV2-69 re-test#3: this runs as a posted task on ui_runner_, so
    // CreateOffer is marshalled onto the signaling thread (header
    // Threading section) — a direct call would trip the //base sync-
    // primitive DCHECK. The PC + adapter scoped_refptrs are copied
    // into the task.
    auto offer_observer = webrtc::make_ref_counted<CreateOfferObserver>(
        weak_factory_.GetWeakPtr(), ui_runner_);
    signaling_thread_->PostTask(
        [pc = pc_, offer_observer, opts]() {
          pc->CreateOffer(offer_observer.get(), opts);
        });
    return;
  }
  // R6: subsequent fires — libwebrtc tells us SDP needs refresh
  // (e.g. M2 R5 capture-lifecycle resume re-attached the video
  // transceiver, or M4 R8 clipboard DC added a new m-line). Route
  // through the coalescing renegotiation orchestrator.
  BeginRenegotiation("libwebrtc");
}

void CbOffererDriver::HopHandleCreateOfferSuccess(std::string sdp_type,
                                                  std::string sdp) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != OffererState::kCreatingOffer) {
    FailWithReason("CreateOffer success in unexpected state");
    return;
  }
  // Reconstruct the SessionDescriptionInterface so we can hand it
  // back to SetLocalDescription.
  //
  // chromium-7727 API drift (CV2-69 cleanup, #176): the legacy
  // CreateSessionDescription(const std::string& type, const
  // std::string& sdp, SdpParseError* error) 3-arg form is REMOVED.
  // Modern API is CreateSessionDescription(SdpType, absl::string_view)
  // returning unique_ptr<SessionDescriptionInterface> (nullptr on
  // parse failure; no out-param). Convert the string type via
  // SdpTypeFromString (returns std::optional<SdpType>).
  std::optional<webrtc::SdpType> parsed_type =
      webrtc::SdpTypeFromString(sdp_type);
  if (!parsed_type) {
    FailWithReason(
        std::string("CreateSessionDescription: unparseable sdp_type '") +
        sdp_type + "' for local offer");
    return;
  }
  std::unique_ptr<webrtc::SessionDescriptionInterface> local(
      webrtc::CreateSessionDescription(*parsed_type, sdp));
  if (!local) {
    FailWithReason(
        "CreateSessionDescription failed for local offer (SDP parse "
        "error or unsupported type)");
    return;
  }
  // CV2-69 re-test#4 (Finding A): the offer may be ready before the
  // WS handshake completes — in re-test #4 CreateOffer finished ~19ms
  // after ws_client_->Connect(), and SendOfferEnvelope failed because
  // the socket was not up yet. If the WS is not connected, stash the
  // SDP; OnConnected drains it via EmitOfferAndSetLocal. state_ stays
  // kCreatingOffer while buffered — SetLocalDescription + ICE
  // gathering are deferred along with the offer, so no ICE candidate
  // is produced before the wire is live.
  if (!ws_connected_) {
    VLOG(1) << kLogPrefix
            << "offer ready but WS not connected — buffering, "
               "awaiting OnConnected";
    pending_local_offer_ = std::move(local);
    return;
  }
  EmitOfferAndSetLocal(std::move(local));
}

void CbOffererDriver::EmitOfferAndSetLocal(
    std::unique_ptr<webrtc::SessionDescriptionInterface> local) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Ordering: emit the `offer` envelope FIRST, then call
  // SetLocalDescription. SetLocalDescription is what unblocks ICE
  // gathering inside libwebrtc; emitting the offer envelope first
  // guarantees offer-before-ICE on the wire even if libwebrtc emits
  // its first ICE candidate synchronously from inside
  // SetLocalDescription. Physics's T96/T104 replay buffer also
  // enforces this server-side, but doing it client-side is cheaper
  // than relying on the broker to re-order.
  SendOfferEnvelope(*local);
  // SendOfferEnvelope routes a failed ws Send to FailWithReason; if
  // that fired, state_ is kFailed and pc_ is gone — do NOT proceed to
  // SetLocalDescription (it would overwrite kFailed and post onto a
  // null pc_).
  if (state_ != OffererState::kCreatingOffer) {
    return;
  }
  state_ = OffererState::kSettingLocal;
  // CV2-69 #176: transient refcounted SetLocalDescObserver adapter.
  // CV2-69 re-test#3: marshal SetLocalDescription onto the signaling
  // thread (header Threading section). The local SDP unique_ptr is
  // moved into the task.
  auto sld_observer = webrtc::make_ref_counted<SetLocalDescObserver>(
      weak_factory_.GetWeakPtr(), ui_runner_);
  signaling_thread_->PostTask(
      [pc = pc_, local = std::move(local), sld_observer]() mutable {
        pc->SetLocalDescription(std::move(local), sld_observer);
      });
}

void CbOffererDriver::HopHandleCreateOfferFailure(std::string reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  FailWithReason(std::string("CreateOffer failed: ") + reason);
}

void CbOffererDriver::HopHandleSetLocalDescriptionComplete(
    bool ok, std::string reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!ok) {
    FailWithReason(std::string("SetLocalDescription failed: ") + reason);
    return;
  }
  if (state_ != OffererState::kSettingLocal) {
    FailWithReason("SetLocalDescription complete in unexpected state");
    return;
  }
  state_ = OffererState::kAwaitingAnswer;
  VLOG(1) << kLogPrefix << "local SDP set; awaiting answer";

  // CV2-ICE early-answer drain (RCA 2026-06-30, kSettingLocal gap). If the
  // cross-pod answer beat our SetLocalDescription and was buffered above,
  // apply it now: we are freshly in kAwaitingAnswer, exactly the state
  // HandleAnswerEnvelope's happy path expects. Synthesize a minimal
  // `answer` Envelope from the buffered SDP and re-enter — it drives the
  // normal kAwaitingAnswer → kSettingRemote SetRemoteDescription. Clear
  // the buffer first so the re-entry can't recurse.
  if (pending_early_answer_sdp_.has_value()) {
    std::string sdp = std::move(*pending_early_answer_sdp_);
    pending_early_answer_sdp_.reset();
    VLOG(1) << kLogPrefix
            << "draining buffered early `answer` now that local SDP is set";
    // HandleAnswerEnvelope reads only env.data (std::get_if<SdpPayload>);
    // type/from are set for struct validity + wire-contract parity (the
    // answer originates from the client peer).
    Envelope replay;
    replay.type = EnvelopeType::kAnswer;
    replay.from = PeerRole::kClient;
    replay.data = SdpPayload{/*sdp_type=*/"answer", /*sdp=*/std::move(sdp)};
    HandleAnswerEnvelope(replay);
  }
}

void CbOffererDriver::HopHandleSetRemoteDescriptionComplete(
    bool ok, std::string reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!ok) {
    FailWithReason(std::string("SetRemoteDescription failed: ") + reason);
    return;
  }
  if (state_ != OffererState::kSettingRemote) {
    FailWithReason("SetRemoteDescription complete in unexpected state");
    return;
  }
  // R6: detect whether this is the initial dance or a renegotiated
  // one. If observer_ has already received OnIceConnectionStateChanged
  // for a prior cycle, we treat this as renegotiation completion. The
  // cheaper proxy: was the dance triggered from kIceInFlight? We can
  // observe that by remembering whether initial_renegotiation_consumed_
  // was set AND a prior kIceInFlight was reached. Track via a second
  // flag — see was_in_ice_flight_once_ initialized lazily in the same
  // transition below.
  const bool was_renegotiation = was_in_ice_flight_once_;
  state_ = OffererState::kIceInFlight;
  was_in_ice_flight_once_ = true;
  // We only reach here by APPLYING a remote answer, so the offer currently on
  // the wire has a live peer behind it. See current_offer_answered_.
  current_offer_answered_ = true;
  VLOG(1) << kLogPrefix
          << (was_renegotiation
                  ? "remote SDP set (renegotiated); ICE in flight"
                  : "remote SDP set (initial); ICE in flight");
  FlushPendingRemoteIce();

  if (was_renegotiation && observer_) {
    observer_->OnRenegotiationCompleted();
  }
  // R6: drain a pending renegotiation request that was deferred while
  // the dance was in flight. BeginRenegotiation() is idempotent in
  // kIceInFlight and will set pending_renegotiation_ false on entry.
  if (pending_renegotiation_) {
    pending_renegotiation_ = false;
    BeginRenegotiation("coalesced-pending");
  }
}

// ---------------------------------------------------------------------
// Envelope emit helpers
// ---------------------------------------------------------------------

void CbOffererDriver::SendOfferEnvelope(
    const webrtc::SessionDescriptionInterface& desc) {
  std::string sdp;
  desc.ToString(&sdp);
  Envelope env;
  env.type = EnvelopeType::kOffer;
  env.from = PeerRole::kBrowser;
  SdpPayload payload;
  payload.sdp_type = "offer";
  payload.sdp = std::move(sdp);
  env.data = std::move(payload);
  if (!ws_client_->Send(env)) {
    FailWithReason("ws Send(offer) failed");
  }
}

void CbOffererDriver::SendIceCandidateEnvelope(
    const webrtc::IceCandidateInterface& candidate) {
  std::string sdp;
  if (!candidate.ToString(&sdp)) {
    VLOG(1) << kLogPrefix << "ICE candidate ToString failed; dropping";
    return;
  }
  Envelope env;
  env.type = EnvelopeType::kIce;
  env.from = PeerRole::kBrowser;
  IceCandidatePayload payload;
  payload.is_end_of_candidates = false;
  payload.candidate = std::move(sdp);
  payload.sdp_mid = candidate.sdp_mid();
  payload.sdp_m_line_index = candidate.sdp_mline_index();
  env.data = std::move(payload);
  if (!ws_client_->Send(env)) {
    // Log and continue. A trickled ICE candidate that cannot be sent means
    // the SOCKET is gone, not that the driver is broken — the WS layer's own
    // close path will end the session in the normal way, and if it does not,
    // the peer connection's ICE timeout will.
    //
    // This used to FailWithReason, which is terminal. Once OnFailed started
    // recycling the guest (rather than only logging), that turned a viewer
    // closing their tab mid-handshake into a process exit: observed live
    // 2026-08-25 as three "FAIL state=AwaitingAnswer reason=ws Send(ice)
    // failed" in a row, each recycling a perfectly healthy browser, with
    // supervisord reporting "exited: chromium (exit status 0; not expected)".
    //
    // Exactly the severity mistake the empty-ICE-candidate fix corrected one
    // layer up: ICE is lossy by design, and one undeliverable candidate is
    // one fewer path, not a dead session.
    LOG(WARNING) << kLogPrefix << "ws Send(ice) failed — candidate dropped; "
                    "the transport is closing and will end the session on its "
                    "own path";
  }
}

void CbOffererDriver::SendIceEndOfCandidates() {
  // R1 exports MakeIceEndOfCandidates so the marker convention is
  // single-sited — the negative-test fixture + the M3 R2 wiring + us
  // all share the same constructor.
  if (!ws_client_->Send(MakeIceEndOfCandidates(PeerRole::kBrowser))) {
    // Same reasoning as SendIceCandidate above: a marker that cannot be
    // delivered is not a broken driver. It is also the LEAST consequential
    // frame to lose — the receiver treats end-of-candidates as an
    // optimisation, and gathering completes regardless.
    LOG(WARNING) << kLogPrefix
                 << "ws Send(ice end-of-candidates) failed — marker dropped";
  }
}

void CbOffererDriver::QueueRemoteIceCandidate(const Envelope& env) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const auto* payload = std::get_if<IceCandidatePayload>(&env.data);
  if (!payload) {
    FailWithReason("`ice` envelope missing candidate payload");
    return;
  }
  pending_remote_ice_.push_back(*payload);
  VLOG(1) << kLogPrefix
          << "queued inbound ICE until remote SDP is applied; pending="
          << pending_remote_ice_.size() << " state=" << StateName(state_);
}

void CbOffererDriver::FlushPendingRemoteIce() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (pending_remote_ice_.empty()) {
    return;
  }
  std::vector<IceCandidatePayload> pending;
  pending.swap(pending_remote_ice_);
  VLOG(1) << kLogPrefix << "flushing queued inbound ICE; count="
          << pending.size();
  for (const IceCandidatePayload& payload : pending) {
    AddRemoteIcePayload(payload);
    if (state_ == OffererState::kFailed || state_ == OffererState::kClosed) {
      return;
    }
  }
}

void CbOffererDriver::AddRemoteIcePayload(
    const IceCandidatePayload& payload) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (payload.is_end_of_candidates) {
    // libwebrtc accepts a null candidate to mean end-of-remote-pool.
    // CV2-69 re-test#3: marshal onto the signaling thread (header
    // Threading section) — OnEnvelope runs as a posted task.
    signaling_thread_->PostTask(
        [pc = pc_]() { pc->AddIceCandidate(nullptr); });
    return;
  }
  webrtc::SdpParseError err;
  std::unique_ptr<webrtc::IceCandidateInterface> cand(
      webrtc::CreateIceCandidate(payload.sdp_mid.value_or(""),
                                 payload.sdp_m_line_index.value_or(0),
                                 payload.candidate, &err));
  if (!cand) {
    // DROP the candidate; do NOT fail the session.
    //
    // This used to call FailWithReason, which is terminal — the driver
    // moves to kFailed, main_parts logs "unrecoverable failure", and the
    // peer connection is gone for the life of the process. One unparseable
    // candidate from a remote peer took down a working session.
    //
    // That is the wrong severity by a wide margin. ICE is designed around
    // candidates being lossy: they arrive out of order, some are
    // unresolvable (an mDNS .local candidate whose resolution fails is
    // normal and was logged immediately before this on 2026-08-24), and
    // connectivity is established from whichever ones DO work. A candidate
    // we cannot parse is one fewer path, not a dead session — the remaining
    // host/srflx/relay candidates can still pair.
    //
    // The empty-string case that actually triggered this is now caught
    // upstream in cb_wire_envelope.cc, where it is correctly read as
    // end-of-candidates. This stays as defence in depth: whatever else a
    // peer sends that we cannot parse, the answer is to skip it loudly, not
    // to take the browser down with it.
    LOG(WARNING) << kLogPrefix << "dropping unparseable remote ICE candidate: "
                 << err.description << " — candidate=[" << payload.candidate
                 << "] mid=" << payload.sdp_mid.value_or("")
                 << "; session continues on the remaining candidates";
    return;
  }
  // TODO(M3-R4-add-ice-async): libwebrtc has both a synchronous
  // AddIceCandidate(const IceCandidateInterface*) and an async
  // AddIceCandidate(std::unique_ptr, std::function<void(RTCError)>)
  // form. The async form surfaces add-side failures verbatim and is
  // preferred; confirm chromium-bundled libwebrtc revision exposes
  // it during first-build and swap.
  //
  // CV2-69 re-test#3: marshal AddIceCandidate onto the signaling
  // thread (header Threading section) — OnEnvelope runs as a posted
  // task. The candidate unique_ptr is moved into the task to keep it
  // alive for the duration of the synchronous AddIceCandidate call;
  // its bool return is consumed inside the task (false is a soft
  // error libwebrtc also logs internally).
  signaling_thread_->PostTask(
      [pc = pc_, cand = std::move(cand)]() {
        if (!pc->AddIceCandidate(cand.get())) {
          VLOG(1) << kLogPrefix
                  << "AddIceCandidate returned false; ignoring "
                     "(libwebrtc treats this as a soft error)";
        }
      });
}

// ---------------------------------------------------------------------
// Inbound dispatch
// ---------------------------------------------------------------------

void CbOffererDriver::HandleOfferEnvelope(const Envelope& /*env*/) {
  // The browser peer is ALWAYS the offerer in v1. An inbound `offer`
  // envelope means the broker mis-routed or the contract drifted.
  FailWithReason("unexpected inbound `offer` envelope "
                 "(v1 contract: browser is sole offerer)");
}

void CbOffererDriver::HandleAnswerEnvelope(const Envelope& env) {
  // CV2-ICE duplicate-answer tolerance (RCA 2026-06-30). Physics' cross-
  // pod answer delivery is NOT deduplicated: the SAME answer is routinely
  // re-delivered to the browser-offerer. Observed on a single live
  // session: two byte-identical 3494-byte `answer` envelopes assembled
  // 6ms apart. The first lands in kAwaitingAnswer and drives
  // kAwaitingAnswer → kSettingRemote (SetRemoteDescription posted to the
  // signaling thread, below); the duplicate then arrives while that SRD
  // is still in flight (state_ == kSettingRemote) or already complete
  // (state_ == kIceInFlight).
  //
  // The v1 contract makes the browser the SOLE offerer with exactly one
  // answer per offer, so a second answer in either of those states is a
  // benign re-delivery — drop it idempotently. Treating it as a hard
  // failure (the prior behaviour) called FailWithReason → teardown from
  // THIS posted-task context, where chromium's per-task
  // DisallowBaseSyncPrimitives is installed; the teardown's blocking
  // proxy hop tripped a FATAL `!tls_base_sync_primitives_disallowed`
  // DCHECK that ABORTED the guest browser process. A dead guest never
  // answers the client's STUN binding checks → client respR=0 → ICE
  // checking→disconnected→failed → framesDecoded=0 (the reported
  // "fleet-wide ICE-connectivity degradation").
  //
  // Renegotiation is unaffected: it BEGINS only from kIceInFlight and
  // re-enters kAwaitingAnswer before its fresh answer is due, so a
  // legitimately-new answer is always consumed in kAwaitingAnswer;
  // kSettingRemote/kIceInFlight are reachable only once an answer for the
  // current offer is already applying or applied.
  if (state_ == OffererState::kSettingRemote ||
      state_ == OffererState::kIceInFlight) {
    VLOG(1) << kLogPrefix
            << "duplicate/late `answer` envelope dropped (answer already "
               "applying or applied), state=" << StateName(state_);
    return;
  }
  // CV2-ICE early-answer buffer (RCA 2026-06-30, kSettingLocal gap). On
  // the cross-pod physics path the answer can arrive BEFORE our own
  // SetLocalDescription completes — state_ still kCreatingOffer /
  // kSettingLocal, strictly EARLIER than kAwaitingAnswer. This is the
  // legitimate first answer, not a redundant duplicate: BUFFER it (do not
  // drop, do not fail) and replay it from
  // HopHandleSetLocalDescriptionComplete once we reach kAwaitingAnswer.
  // Prior behaviour fell through to FailWithReason → teardown from this
  // posted-task context → guest SIGABRT → respR=0 (the deterministic
  // cross-pod "no video"). Validate the payload up front so a malformed
  // early answer still fails fast rather than buffering garbage.
  if (state_ == OffererState::kCreatingOffer ||
      state_ == OffererState::kSettingLocal) {
    const auto* early = std::get_if<SdpPayload>(&env.data);
    if (!early || early->sdp.empty()) {
      FailWithReason("`answer` envelope missing SDP payload");
      return;
    }
    pending_early_answer_sdp_ = early->sdp;
    VLOG(1) << kLogPrefix
            << "early `answer` buffered (arrived before local SDP set), "
               "state=" << StateName(state_)
            << "; will apply on SetLocalDescription completion";
    return;
  }
  if (state_ != OffererState::kAwaitingAnswer) {
    FailWithReason("`answer` envelope in unexpected state");
    return;
  }
  const auto* payload = std::get_if<SdpPayload>(&env.data);
  if (!payload || payload->sdp.empty()) {
    FailWithReason("`answer` envelope missing SDP payload");
    return;
  }
  // chromium-7727 API drift (CV2-69 cleanup, #176): see
  // HopHandleCreateOfferSuccess above. The remote SDP type is the
  // literal "answer" — use webrtc::SdpType::kAnswer directly rather
  // than round-tripping through SdpTypeFromString.
  std::unique_ptr<webrtc::SessionDescriptionInterface> remote(
      webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer,
                                       payload->sdp));
  if (!remote) {
    FailWithReason(
        "CreateSessionDescription failed for remote answer (SDP parse "
        "error)");
    return;
  }
  state_ = OffererState::kSettingRemote;
  // CV2-69 #176: transient refcounted SetRemoteDescObserver adapter.
  // CV2-69 re-test#3: marshal SetRemoteDescription onto the signaling
  // thread (header Threading section) — OnEnvelope runs as a posted
  // task. The remote SDP unique_ptr is moved into the task.
  auto srd_observer = webrtc::make_ref_counted<SetRemoteDescObserver>(
      weak_factory_.GetWeakPtr(), ui_runner_);
  signaling_thread_->PostTask(
      [pc = pc_, remote = std::move(remote), srd_observer]() mutable {
        pc->SetRemoteDescription(std::move(remote), srd_observer);
      });
}

void CbOffererDriver::HandleIceEnvelope(const Envelope& env) {
  if (state_ != OffererState::kIceInFlight) {
    const bool can_queue_mid_dance =
        (state_ >= OffererState::kAwaitingAnswer &&
         state_ <= OffererState::kSettingRemote) ||
        (was_in_ice_flight_once_ && state_ >= OffererState::kCreatingOffer &&
         state_ <= OffererState::kSettingRemote);
    if (can_queue_mid_dance) {
      QueueRemoteIceCandidate(env);
      return;
    }
    VLOG(1) << kLogPrefix
            << "inbound ICE before local SDP can accept it; dropping, state="
            << StateName(state_);
    return;
  }
  const auto* payload = std::get_if<IceCandidatePayload>(&env.data);
  if (!payload) {
    FailWithReason("`ice` envelope missing candidate payload");
    return;
  }
  AddRemoteIcePayload(*payload);
}

void CbOffererDriver::HandleByeEnvelope() {
  // A `bye` that arrives when there is no session to end is stale — it
  // belongs to the viewer who ALREADY left, not to the one this driver is
  // now offering to. Dropping it here is defence in depth for the re-arm
  // path.
  //
  // Measured 2026-08-21: a clean client close produced TWO byes ~49ms apart
  // (the client sends one itself, then the broker synthesised another on
  // socket drop). The first correctly tore the session down and RearmSession
  // rebuilt it in ~7ms; the second then closed the FRESH session, so the
  // still-pending OnRenegotiationNeeded landed in kClosed and took the
  // FailWithReason path -> kFailed -> Rearm() refuses -> process exit. The
  // browser died on every viewer change while the log said "rebuilt".
  //
  // The broker no longer sends the duplicate (signaling/server.go, peer
  // .saidBye), but the worker must not depend on that: an older broker, a
  // retried frame, or a genuinely doubled client are all outside its control,
  // and the cost of being wrong is the browser process.
  //
  // kIdle / kCreatingPc mean "no negotiated session yet" — exactly the window
  // a re-armed driver sits in until its fresh offer is answered.
  if (state_ == OffererState::kIdle || state_ == OffererState::kCreatingPc) {
    LOG(INFO) << kLogPrefix
              << "dropping stale `bye` in state=" << StateName(state_)
              << " — no live session to close (re-arm in progress)";
    return;
  }
  // R6: route inbound `bye` through the unified teardown path.
  // CloseInternal handles idempotency, observer fan-out, and the
  // teardown_emitted_ guard.
  CloseInternal("remote bye", "remote");
}

void CbOffererDriver::HandleRequestRenegotiateEnvelope() {
  // R6: inbound `request_renegotiate` from the portal client. Valid
  // only in kIceInFlight — the portal client asked us to refresh SDP.
  // Out-of-sequence (pre-initial-dance or mid-dance) is a protocol
  // violation: REPLAYABLE_TYPES contains `request_renegotiate` so the
  // broker may deliver it ahead of the answer if the portal client
  // sent it during a reconnect race; treat the pre-kIceInFlight case
  // as "coalesce for later" rather than a hard fail, to be robust to
  // R7's reconnect storms.
  // kAwaitingAnswer is THE cold-arrival state: we have an offer on the wire and
  // nobody has answered it. A request_renegotiate arriving here is a viewer
  // saying "I never got an offer" — and it is right, because the offer it is
  // waiting for was buffered by the broker and then aged out at
  // iceReplayMaxAge before this viewer ever joined.
  //
  // Measured 2026-08-21: after re-arming, the driver sits in kAwaitingAnswer,
  // NOT kIceInFlight. The first version of this fix guarded kIceInFlight and
  // therefore never fired — the request fell through to the mid-dance coalesce
  // below, which sets pending_renegotiation_ and waits for a return to
  // kIceInFlight that can only happen if someone answers. Nobody ever does,
  // so the viewer waits out its own watchdog and reports `failed`.
  //
  // Re-offering on this PeerConnection would be wrong for the same reason as
  // below: its ICE ufrag/pwd and DTLS fingerprint were minted for the offer
  // the new viewer never received. Only a fresh PC is correct, and only the
  // embedder can build one.
  if (state_ == OffererState::kAwaitingAnswer) {
    LOG(INFO) << kLogPrefix
              << "inbound request_renegotiate while AWAITING AN ANSWER — a new "
                 "viewer never received our offer; re-arming for it";
    if (observer_) {
      observer_->OnNewViewerNeedsOffer();
    }
    return;
  }
  if (state_ == OffererState::kIceInFlight) {
    // WHO is asking matters more than the state.
    //
    // If our current offer was never answered, the peer asking is NOT the peer
    // we are negotiating with. That is the cold-arrival shape, measured live
    // 2026-08-21:
    //
    //   09:37:58  worker re-arms after the last viewer left; offer buffered
    //   ...       >5 minutes idle
    //   09:43:19  a NEW viewer joins. The broker correctly DROPS the buffered
    //             offer (age 322s > the 300s cap) so nothing is replayed, and
    //             the page sits at "waiting for offer".
    //   +45s      the client's offer watchdog sends `request_renegotiate`.
    //
    // We were in kIceInFlight the whole time — from the PREVIOUS viewer's
    // session. Renegotiating on that PeerConnection re-offers with ICE
    // ufrag/pwd and a DTLS fingerprint belonging to a peer that is gone, which
    // is exactly the mismatch that shows up as `iceConnectionState=failed`
    // with relay candidates present on BOTH sides — the failure that started
    // this whole line of work.
    //
    // A fresh PeerConnection is the only correct answer, and only the embedder
    // can build one (it owns the transceivers and data channels), so hand the
    // decision up.
    //
    // Why this cannot misfire on a HEALTHY viewer: kIceInFlight is assigned at
    // exactly ONE site (HopHandleSetRemoteDescriptionComplete), and that site
    // sets current_offer_answered_ = true two lines later. A viewer that is
    // actually connected therefore always has the flag set, so a live viewer
    // asking to refresh SDP takes the BeginRenegotiation path below.
    //
    // This branch is belt-and-braces: the cold-arrival case is caught earlier,
    // in kAwaitingAnswer. It remains because kIceInFlight-with-an-unanswered
    // offer would mean the two flags disagree, and re-offering stale ICE
    // credentials is the more expensive way to be wrong.
    if (!current_offer_answered_) {
      LOG(INFO) << kLogPrefix
                << "inbound request_renegotiate from an UNANSWERED offer — a "
                   "new viewer needs a fresh session, not a renegotiation";
      if (observer_) {
        observer_->OnNewViewerNeedsOffer();
      }
      return;
    }
    BeginRenegotiation("remote");
    return;
  }
  if (state_ == OffererState::kClosed ||
      state_ == OffererState::kFailed ||
      state_ == OffererState::kIdle) {
    VLOG(1) << kLogPrefix
            << "inbound request_renegotiate dropped, state="
            << StateName(state_);
    return;
  }
  // Mid-dance — coalesce. Will be drained on the next kIceInFlight
  // transition (HopHandleSetRemoteDescriptionComplete).
  VLOG(1) << kLogPrefix
          << "inbound request_renegotiate coalesced (mid-dance), state="
          << StateName(state_);
  pending_renegotiation_ = true;
}

void CbOffererDriver::HandleProbeResultEnvelope(const Envelope& /*env*/) {
  // M0 R5's streamer.js historically consumed probe_result to apply
  // bandwidth/quality knobs. v1 native-peer doesn't honor these yet;
  // log + drop.
  //
  // TODO(M3-R4-probe-result-apply): wire probe_result fields into
  // PC's sender encodings (max_bitrate_bps, scale_resolution_down_by,
  // etc) once M6 R1's stats relay lands and we have a path to surface
  // the numeric knobs.
  VLOG(1) << kLogPrefix
          << "probe_result received; ignored (TODO probe-result-apply)";
}

// ---------------------------------------------------------------------
// R6: renegotiation orchestrator + clean teardown
// ---------------------------------------------------------------------

void CbOffererDriver::BeginRenegotiation(std::string_view trigger) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ == OffererState::kClosed ||
      state_ == OffererState::kFailed ||
      state_ == OffererState::kIdle ||
      state_ == OffererState::kCreatingPc) {
    VLOG(1) << kLogPrefix
            << "BeginRenegotiation dropped (pre-ICE or terminal), state="
            << StateName(state_) << " trigger=" << trigger;
    return;
  }
  if (state_ != OffererState::kIceInFlight) {
    // Mid-dance — coalesce. Will be drained on the next return to
    // kIceInFlight in HopHandleSetRemoteDescriptionComplete.
    VLOG(1) << kLogPrefix
            << "BeginRenegotiation coalesced (mid-dance), state="
            << StateName(state_) << " trigger=" << trigger;
    pending_renegotiation_ = true;
    return;
  }
  // Steady state — kick off a fresh offer dance.
  VLOG(1) << kLogPrefix
          << "BeginRenegotiation: dance re-entering kCreatingOffer, "
             "trigger=" << trigger;
  state_ = OffererState::kCreatingOffer;
  current_offer_answered_ = false;  // a brand-new offer has nobody behind it
  if (observer_) {
    observer_->OnRenegotiationStarted(trigger);
  }
  // TODO(M3-R6-renegotiation-opts): the initial CreateOffer in
  // HopHandleRenegotiationNeeded passes a default RTCOfferAnswerOptions.
  // For renegotiation, libwebrtc supports
  // RTCOfferAnswerOptions::ice_restart = true to force a fresh ICE
  // username-fragment pair (per draft-ietf-ice-rfc5245bis §9.1.1.1).
  // We don't enable ice_restart by default — only the embedder knows
  // whether the network path actually changed. Wire a parameter onto
  // RequestRenegotiation() that maps through here when the embedder
  // explicitly opts in. The OnRenegotiationNeeded()-driven trigger
  // never sets ice_restart.
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions opts;
  // CV2-69 #176: transient refcounted CreateOfferObserver adapter
  // (not `this` — see the adapter block above); CreateOffer AddRefs
  // the observer internally so it survives until libwebrtc releases
  // it after the OnSuccess/OnFailure callback.
  // CV2-69 re-test#3: BeginRenegotiation runs from posted-task
  // contexts (HopHandleRenegotiationNeeded, the coalesced-pending
  // drain, the inbound request_renegotiate handler), so CreateOffer
  // is marshalled onto the signaling thread — header Threading
  // section. The PC + adapter scoped_refptrs are copied into the task.
  auto offer_observer = webrtc::make_ref_counted<CreateOfferObserver>(
      weak_factory_.GetWeakPtr(), ui_runner_);
  signaling_thread_->PostTask(
      [pc = pc_, offer_observer, opts]() {
        pc->CreateOffer(offer_observer.get(), opts);
      });
}

void CbOffererDriver::CloseInternal(std::string_view reason,
                                    std::string_view source) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (teardown_emitted_) {
    VLOG(1) << kLogPrefix
            << "Close dropped (already emitted), source=" << source
            << " reason=" << reason;
    return;
  }
  if (state_ == OffererState::kFailed) {
    // Failure already terminated us; OnFailed already fired. Don't
    // also fire OnClosed.
    VLOG(1) << kLogPrefix
            << "Close dropped (already failed), source=" << source
            << " reason=" << reason;
    teardown_emitted_ = true;
    return;
  }
  VLOG(1) << kLogPrefix << "Closing: source=" << source
          << " reason=" << reason << " from state=" << StateName(state_);
  // Best-effort bye emit. Skip if the close source is the ws layer
  // itself (the socket is already gone); skip if we never reached a
  // post-PC state (no signaling channel established). Failure of the
  // ws Send is logged but does NOT block teardown — the broker times
  // out the session on its own per the wire contract.
  const bool emit_bye = source != "ws" && source != "remote" &&
                        state_ >= OffererState::kCreatingOffer &&
                        state_ <= OffererState::kIceInFlight;
  if (emit_bye) {
    if (!SendByeEnvelope()) {
      VLOG(1) << kLogPrefix << "Close: bye send failed (logged, ignored)";
    }
  }
  state_ = OffererState::kClosed;
  teardown_emitted_ = true;
  pending_remote_ice_.clear();
  // CV2-69 re-test#4 (Finding B): marshalled PC Close() — see
  // ClosePcOnSignalingThread. CloseInternal is reachable from the
  // posted-task OnEnvelope path (inbound `bye`), so a direct
  // pc_ = nullptr here would FATAL on the proxy's blocking-hop dtor.
  // The ref is retained; ~CbOffererDriver destroys it.
  ClosePcOnSignalingThread();
  if (observer_) {
    observer_->OnClosed(reason);
  }
}

bool CbOffererDriver::SendByeEnvelope() {
  // The bye envelope omits the `data` field entirely per the wire
  // contract (cb_wire_envelope.h:27-28). std::monostate is the codec's
  // representation of "no data field" for the kBye tag, so we leave
  // env.data default-constructed.
  Envelope env;
  env.type = EnvelopeType::kBye;
  env.from = PeerRole::kBrowser;
  // env.data left default — monostate, encoder omits the wire field.
  return ws_client_->Send(env);
}

bool CbOffererDriver::SendSessionUnhealthyEnvelope() {
  // CV2-GPU-DEATH: like the bye envelope, session_unhealthy omits the `data`
  // field entirely (monostate). The type alone tells physics to release this
  // element's isolation-registry entry so the next allocate_or_reuse mints a
  // fresh guest. Sent BEFORE the bye in the CloseUnhealthy() teardown so the
  // recycle signal reaches physics even if the close races the WS shutdown.
  Envelope env;
  env.type = EnvelopeType::kSessionUnhealthy;
  env.from = PeerRole::kBrowser;
  // env.data left default — monostate, encoder omits the wire field.
  return ws_client_->Send(env);
}

// ---------------------------------------------------------------------
// PC teardown + terminal failure
// ---------------------------------------------------------------------

void CbOffererDriver::ClosePcOnSignalingThread() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!pc_) {
    return;
  }
  // CV2-69 re-test#4 (Finding B): terminate the PeerConnection from
  // FailWithReason / CloseInternal — both reachable from posted-task
  // contexts (every Hop*/envelope handler), where chromium's per-task
  // DisallowBaseSyncPrimitives is installed.
  //
  // Two operations a naive `pc_ = nullptr` would do here, BOTH unsafe
  // from a posted task:
  //   1. PeerConnection::Close() — a proxy method → blocking hop.
  //   2. proxy destruction — the proxy dtor blocking-hops to the
  //      signaling thread (this is the re-test #4 FATAL site).
  //
  // Fix: marshal Close() onto the signaling thread, and DO NOT drop
  // the ref here. Close() halts ICE/media + drives the
  // iceConnectionState=closed transition; it runs inline on the
  // signaling thread (no blocking hop). The pc_ ref is deliberately
  // RETAINED — the PeerConnection holds the driver as its
  // PeerConnectionObserver, so the PC must NOT outlive the driver
  // (an async ref-drop would open a window where a PC callback
  // dereferences a freed driver). pc_ is destroyed synchronously by
  // ~CbOffererDriver, which runs in the embedder's non-task
  // PostMainMessageLoopRun context where the proxy dtor's blocking
  // hop IS allowed (no per-task disallow) — and the dtor drops pc_
  // before weak_factory_, so the PC is fully gone before the driver.
  signaling_thread_->PostTask([pc = pc_]() { pc->Close(); });
}

void CbOffererDriver::FailWithReason(std::string_view reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  LOG(ERROR) << kLogPrefix << "FAIL state=" << StateName(state_)
             << " reason=" << reason;
  state_ = OffererState::kFailed;
  pending_remote_ice_.clear();
  ClosePcOnSignalingThread();
  if (observer_) {
    observer_->OnFailed(reason);
  }
}

}  // namespace cloud_browser::signaling

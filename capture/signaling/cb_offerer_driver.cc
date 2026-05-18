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
    SignalingWsClient* ws_client,
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

OffererState CbOffererDriver::state() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return state_;
}

webrtc::PeerConnectionInterface* CbOffererDriver::pc() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return pc_.get();
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
  std::unique_ptr<webrtc::IceCandidateInterface> cloned(
      webrtc::CreateIceCandidate(candidate->sdp_mid(),
                                 candidate->sdp_mline_index(),
                                 candidate->candidate()));
  if (!cloned) {
    VLOG(1) << kLogPrefix
            << "OnIceCandidate clone failed; dropping candidate";
    return;
  }
  ui_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&CbOffererDriver::HopHandleIceCandidate,
                     weak_factory_.GetWeakPtr(), std::move(cloned)));
}

void CbOffererDriver::OnIceGatheringChange(
    webrtc::PeerConnectionInterface::IceGatheringState new_state) {
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
  VLOG(1) << kLogPrefix
          << (was_renegotiation
                  ? "remote SDP set (renegotiated); ICE in flight"
                  : "remote SDP set (initial); ICE in flight");

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
    FailWithReason("ws Send(ice) failed");
  }
}

void CbOffererDriver::SendIceEndOfCandidates() {
  // R1 exports MakeIceEndOfCandidates so the marker convention is
  // single-sited — the negative-test fixture + the M3 R2 wiring + us
  // all share the same constructor.
  if (!ws_client_->Send(MakeIceEndOfCandidates(PeerRole::kBrowser))) {
    FailWithReason("ws Send(ice end-of-candidates) failed");
  }
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
  if (state_ < OffererState::kAwaitingAnswer) {
    // TODO(M3-R4-pre-answer-ice-buffer): physics's REPLAYABLE_TYPES
    // set is offer / answer / request_renegotiate — NOT ice. So pre-
    // answer ICE from the remote side is something the broker won't
    // re-order for us. Right now we drop on the floor (the portal
    // client's libwebrtc only emits ICE after its SetLocalDescription
    // on the answer, which means our SetRemoteDescription has
    // already completed). Confirm during M3 R4 integration smoke;
    // if pre-answer remote ICE turns out to be possible, queue +
    // flush on SetRemoteDescription completion.
    VLOG(1) << kLogPrefix
            << "inbound ICE before answer; dropping (TODO buffer)";
    return;
  }
  const auto* payload = std::get_if<IceCandidatePayload>(&env.data);
  if (!payload) {
    FailWithReason("`ice` envelope missing candidate payload");
    return;
  }
  if (payload->is_end_of_candidates) {
    // libwebrtc accepts a null candidate to mean end-of-remote-pool.
    // CV2-69 re-test#3: marshal onto the signaling thread (header
    // Threading section) — OnEnvelope runs as a posted task.
    signaling_thread_->PostTask(
        [pc = pc_]() { pc->AddIceCandidate(nullptr); });
    return;
  }
  webrtc::SdpParseError err;
  std::unique_ptr<webrtc::IceCandidateInterface> cand(
      webrtc::CreateIceCandidate(payload->sdp_mid.value_or(""),
                                 payload->sdp_m_line_index.value_or(0),
                                 payload->candidate, &err));
  if (!cand) {
    FailWithReason(std::string("CreateIceCandidate failed: ") +
                   err.description);
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

void CbOffererDriver::HandleByeEnvelope() {
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
  if (state_ == OffererState::kIceInFlight) {
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
  ClosePcOnSignalingThread();
  if (observer_) {
    observer_->OnFailed(reason);
  }
}

}  // namespace cloud_browser::signaling

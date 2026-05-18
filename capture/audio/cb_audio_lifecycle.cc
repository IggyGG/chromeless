// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Implementation of CbAudioLifecycle. See cb_audio_lifecycle.h for the
// full design discussion (orphan-stream failure mode, ADM ref-cycle
// care, observer-chain composition, sequence diagrams).
//
// Implementation notes that do NOT belong in the header:
//
//   * transceiver_->StopStandard() is the libwebrtc API that triggers
//     the sender-side teardown path we depend on for clean ADM
//     StopRecording semantics. There is also a legacy ::Stop() — the
//     spec-difference is documented at
//     api/rtp_transceiver_interface.h (StopStandard is the W3C
//     spec-aligned variant; ::Stop is the legacy synchronous one).
//     StopStandard is the right call for native chromeless because
//     M2's video transceiver path also uses it (per the framesink-
//     capturer lifecycle in cv2/m2-r5-capture-lifecycle); using the
//     same primitive across audio + video keeps the libwebrtc
//     teardown observability consistent.
//
//   * On the OnFailed path the PeerConnection has already been
//     dropped by the driver, so transceiver_->StopStandard() will
//     usually return webrtc::RTCErrorType::INVALID_STATE. We log that
//     at INFO (not WARNING) because it is the known shape; flooding
//     WARNING on the failure path would mask the upstream signal
//     (the "ice failed" / "answerer hung up" reason from the driver).

#include "capture/audio/cb_audio_lifecycle.h"

#include <utility>

#include "absl/strings/str_cat.h"
#include "api/audio/audio_device.h"
#include "api/rtc_error.h"
#include "base/logging.h"
#include "base/sequence_checker.h"

namespace cloud_browser::audio {

namespace {

const char* IceConnectionStateName(
    webrtc::PeerConnectionInterface::IceConnectionState state) {
  using webrtc::PeerConnectionInterface;
  switch (state) {
    case PeerConnectionInterface::kIceConnectionNew:          return "new";
    case PeerConnectionInterface::kIceConnectionChecking:     return "checking";
    case PeerConnectionInterface::kIceConnectionConnected:    return "connected";
    case PeerConnectionInterface::kIceConnectionCompleted:    return "completed";
    case PeerConnectionInterface::kIceConnectionFailed:       return "failed";
    case PeerConnectionInterface::kIceConnectionDisconnected: return "disconnected";
    case PeerConnectionInterface::kIceConnectionClosed:       return "closed";
    case PeerConnectionInterface::kIceConnectionMax:          return "max";
  }
  return "unknown";
}

const char* StateName(AudioLifecycleState s) {
  switch (s) {
    case AudioLifecycleState::kIdle:    return "idle";
    case AudioLifecycleState::kArmed:   return "armed";
    case AudioLifecycleState::kActive:  return "active";
    case AudioLifecycleState::kStopped: return "stopped";
  }
  return "unknown";
}

}  // namespace

CbAudioLifecycle::CbAudioLifecycle(
    signaling::OffererDriverObserver* downstream,
    AudioLifecycleObserver* observer,
    scoped_refptr<base::SequencedTaskRunner> ui_runner,
    raw_ptr<webrtc::AudioDeviceModule> adm_debug)
    : downstream_(downstream),
      observer_(observer),
      ui_runner_(std::move(ui_runner)),
      adm_debug_(adm_debug) {
  DCHECK(ui_runner_) << "ui_runner is required";
  VLOG(1) << "[m55-r5] CbAudioLifecycle constructed; adm_debug="
          << static_cast<const void*>(adm_debug_.get());
}

CbAudioLifecycle::~CbAudioLifecycle() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != AudioLifecycleState::kStopped &&
      state_ != AudioLifecycleState::kIdle) {
    // Embedder forgot to call PrepareForTeardown / didn't see OnClosed /
    // OnFailed before destruction. Run the best-effort cleanup so the
    // refs are released in the right order even on this path. The
    // graceful=false flag in the observer notification flags the leak
    // for the embedder's telemetry.
    LOG(WARNING) << "[m55-r5] dtor with state=" << StateName(state_)
                 << "; running best-effort teardown (embedder leaked "
                    "the lifecycle?)";
    StopInternal(/*graceful=*/false, "lifecycle destroyed", "dtor");
  }
}

void CbAudioLifecycle::AdoptBindings(
    webrtc::scoped_refptr<webrtc::AudioSourceInterface> source,
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> track,
    webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (state_ == AudioLifecycleState::kStopped) {
    LOG(WARNING) << "[m55-r5] AdoptBindings called in kStopped; ignoring "
                    "(construct a new lifecycle for a new session)";
    return;
  }
  if (state_ != AudioLifecycleState::kIdle) {
    LOG(WARNING) << "[m55-r5] AdoptBindings called in state="
                 << StateName(state_) << "; ignoring (already armed)";
    return;
  }
  if (!source || !track || !transceiver) {
    LOG(ERROR) << "[m55-r5] AdoptBindings: nullptr "
               << (source ? "" : "source ")
               << (track ? "" : "track ")
               << (transceiver ? "" : "transceiver")
               << "; keeping state=kIdle (R5 will run best-effort "
                  "teardown if Close/Failed lands later)";
    return;
  }

  source_ = std::move(source);
  track_ = std::move(track);
  transceiver_ = std::move(transceiver);
  state_ = AudioLifecycleState::kArmed;

  VLOG(1) << "[m55-r5] AdoptBindings -> kArmed; track id=" << track_->id()
          << " transceiver mid="
          << (transceiver_->mid().has_value() ? *transceiver_->mid()
                                              : "(unset)")
          << " direction="
          // chromium 7727: webrtc::RtpTransceiverDirectionToString was
          // removed from the api/ surface (survives only as an internal
          // helper under webrtc/pc/). api/rtp_transceiver_direction.h now
          // provides AbslStringify for RtpTransceiverDirection — the
          // canonical stringification path is absl::StrCat.
          << absl::StrCat(transceiver_->direction());
}

void CbAudioLifecycle::PrepareForTeardown(std::string_view reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  switch (state_) {
    case AudioLifecycleState::kStopped:
      VLOG(2) << "[m55-r5] PrepareForTeardown(reason=" << reason
              << ") in kStopped; no-op";
      return;
    case AudioLifecycleState::kIdle:
      VLOG(1) << "[m55-r5] PrepareForTeardown(reason=" << reason
              << ") in kIdle; nothing to tear down; transitioning to "
                 "kStopped to suppress OnClosed/OnFailed best-effort";
      state_ = AudioLifecycleState::kStopped;
      if (observer_) {
        observer_->OnAudioStopped(/*graceful=*/true, reason);
      }
      return;
    case AudioLifecycleState::kArmed:
    case AudioLifecycleState::kActive:
      StopInternal(/*graceful=*/true, reason, "prepare");
      return;
  }
}

AudioLifecycleState CbAudioLifecycle::state() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return state_;
}

// --- OffererDriverObserver overrides ----------------------------------

void CbAudioLifecycle::OnIceConnectionStateChanged(
    webrtc::PeerConnectionInterface::IceConnectionState state) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  VLOG(2) << "[m55-r5] OnIceConnectionStateChanged(" << IceConnectionStateName(state)
          << ") in state=" << StateName(state_);

  // Promote kArmed → kActive on the first kIceConnectionConnected
  // post-AdoptBindings. We do NOT re-fire OnAudioActivated on
  // subsequent renegotiation (the audio_activated_emitted_ latch
  // guards that case).
  if (state == webrtc::PeerConnectionInterface::kIceConnectionConnected &&
      state_ == AudioLifecycleState::kArmed) {
    state_ = AudioLifecycleState::kActive;
    if (!audio_activated_emitted_) {
      audio_activated_emitted_ = true;
      VLOG(1) << "[m55-r5] kArmed -> kActive; audio sender presumed live "
                 "(ADM StartRecording on libwebrtc worker thread); adm_debug="
              << static_cast<const void*>(adm_debug_.get());
      if (observer_) {
        observer_->OnAudioActivated();
      }
    }
  }

  ForwardIceConnectionState(state);
}

void CbAudioLifecycle::OnRenegotiationStarted(std::string_view trigger) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  VLOG(2) << "[m55-r5] OnRenegotiationStarted(trigger=" << trigger
          << ") — audio bindings preserved across renegotiation";
  ForwardRenegotiationStarted(trigger);
}

void CbAudioLifecycle::OnRenegotiationCompleted() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  VLOG(2) << "[m55-r5] OnRenegotiationCompleted";
  ForwardRenegotiationCompleted();
}

void CbAudioLifecycle::OnClosed(std::string_view reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != AudioLifecycleState::kStopped &&
      state_ != AudioLifecycleState::kIdle) {
    LOG(WARNING) << "[m55-r5] OnClosed(reason=" << reason
                 << ") in state=" << StateName(state_)
                 << "; embedder did not call PrepareForTeardown before "
                    "driver.Close — running best-effort cleanup, "
                    "PulseAudio orphan-stream window is open until "
                    "libwebrtc worker teardown completes";
    StopInternal(/*graceful=*/false, reason, "on-closed");
  } else if (state_ == AudioLifecycleState::kIdle) {
    // No bindings ever adopted; transition the latch so a later
    // dtor doesn't double-fire.
    state_ = AudioLifecycleState::kStopped;
  }
  ForwardClosed(reason);
}

void CbAudioLifecycle::OnFailed(std::string_view reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != AudioLifecycleState::kStopped &&
      state_ != AudioLifecycleState::kIdle) {
    LOG(WARNING) << "[m55-r5] OnFailed(reason=" << reason
                 << ") in state=" << StateName(state_)
                 << "; running best-effort teardown — driver has already "
                    "dropped pc_; transceiver->StopStandard() is expected "
                    "to return INVALID_STATE on this path";
    StopInternal(/*graceful=*/false, reason, "on-failed");
  } else if (state_ == AudioLifecycleState::kIdle) {
    state_ = AudioLifecycleState::kStopped;
  }
  ForwardFailed(reason);
}

// --- private ----------------------------------------------------------

void CbAudioLifecycle::StopInternal(bool graceful, std::string_view reason,
                                    std::string_view source) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Idempotent guard — should already be enforced at every caller, but
  // belt-and-suspenders so a future caller addition can't accidentally
  // double-stop and double-fire OnAudioStopped.
  if (state_ == AudioLifecycleState::kStopped) {
    return;
  }

  VLOG(1) << "[m55-r5] StopInternal source=" << source
          << " reason=" << reason
          << " graceful=" << (graceful ? "true" : "false")
          << " state_=" << StateName(state_);

  // Step 1: stop the transceiver. On the graceful path this is the
  // load-bearing call — libwebrtc schedules sender teardown on its
  // worker thread, which unhooks the AudioSourceInterface from the
  // AudioState and (transitively) signals ADM::StopRecording on the
  // PulseAudio backend's task queue. On the non-graceful path
  // (PeerConnection already dropped by the driver) we still attempt
  // the call so that any libwebrtc revision that tightens the
  // teardown ordering inherits the safer shape automatically.
  if (transceiver_) {
    webrtc::RTCError stop_err = transceiver_->StopStandard();
    if (!stop_err.ok()) {
      // INFO on the non-graceful path because INVALID_STATE is the
      // known shape there (see file-level "Implementation notes").
      // WARNING on the graceful path because that's a real surprise.
      if (graceful) {
        LOG(WARNING) << "[m55-r5] transceiver->StopStandard() failed: "
                     << ToString(stop_err.type()) << " — "
                     << stop_err.message();
      } else {
        VLOG(1) << "[m55-r5] transceiver->StopStandard() failed (expected "
                   "on " << source << " path): "
                << ToString(stop_err.type()) << " — "
                << stop_err.message();
      }
    }
  }

  // Step 2-4: drop refs in declaration order (transceiver → track →
  // source). The order matters: libwebrtc's audio sender holds a ref
  // to the track which holds a ref to the source which (transitively
  // through AudioState) refs the ADM. Releasing transceiver first
  // collapses the sender's strong ref chain; releasing track next
  // releases the sender's track-level state; releasing source last
  // lets the AudioState's RemoveSendStream call complete on the
  // signaling thread before the source's destructor needs to run.
  //
  // Move-into-local + reset shape: makes the release point explicit
  // in the log line and gives a debugger-friendly stack frame if the
  // dtor of one of these refs were to trip an assert.
  {
    auto transceiver = std::move(transceiver_);
    transceiver = nullptr;
  }
  {
    auto track = std::move(track_);
    track = nullptr;
  }
  {
    auto source = std::move(source_);
    source = nullptr;
  }

  state_ = AudioLifecycleState::kStopped;

  VLOG(1) << "[m55-r5] StopInternal complete; state_=kStopped; adm_debug="
          << static_cast<const void*>(adm_debug_.get())
          << " (ADM kept alive by PCF — not released here)";

  if (observer_) {
    observer_->OnAudioStopped(graceful, reason);
  }
}

void CbAudioLifecycle::ForwardIceConnectionState(
    webrtc::PeerConnectionInterface::IceConnectionState state) {
  if (downstream_) {
    downstream_->OnIceConnectionStateChanged(state);
  }
}

void CbAudioLifecycle::ForwardRenegotiationStarted(std::string_view trigger) {
  if (downstream_) {
    downstream_->OnRenegotiationStarted(trigger);
  }
}

void CbAudioLifecycle::ForwardRenegotiationCompleted() {
  if (downstream_) {
    downstream_->OnRenegotiationCompleted();
  }
}

void CbAudioLifecycle::ForwardClosed(std::string_view reason) {
  if (downstream_) {
    downstream_->OnClosed(reason);
  }
}

void CbAudioLifecycle::ForwardFailed(std::string_view reason) {
  if (downstream_) {
    downstream_->OnFailed(reason);
  }
}

}  // namespace cloud_browser::audio

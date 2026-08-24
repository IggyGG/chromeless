// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cb_audio_lifecycle — Module M5.5 of the ChromelessV2 native-peer
// migration (parent CV2-8; this R5 is CV2-32).
//
// CV2-32 binds the send-side audio bindings produced by M5.5 R4
// (CV2-31) — webrtc::AudioSourceInterface + webrtc::AudioTrackInterface
// + sendonly webrtc::RtpTransceiverInterface — to the peer/session
// lifecycle owned by M3 R4 (CV2-54) / R6 (CV2-56). The contract is
// symmetric with the video transceiver's implicit lifecycle: when the
// peer is created the audio sender is wired up and the libwebrtc Linux
// PulseAudio ADM (M5.5 R1, CV2-28) is permitted to call StartRecording
// off its own internal task queue; when the peer is torn down (graceful
// `bye`, embedder Close, or terminal failure) the audio sender is
// stopped first, so that libwebrtc's internal teardown calls
// StopRecording cleanly and the PulseAudio recording stream is
// disconnected before the ADM's TaskQueueFactory unwinds.
//
// # Why a dedicated lifecycle class
//
// Two failure modes motivated splitting this out of R4 instead of
// folding it into the audio-source helper there:
//
//   1. Orphan PulseAudio streams. The libwebrtc Linux ADM's StopRecording
//      path needs to run BEFORE the PeerConnection drops, because the
//      sender's teardown unhooks the AudioSourceInterface from the
//      AudioState and (transitively) signals ADM::StopRecording on the
//      worker thread. If pc_ is dropped first, the source's last sender
//      ref releases on the signaling thread while the sender's worker-
//      thread teardown is still in flight — observable as orphan
//      `pa_stream` handles in `pacmd list-source-outputs` after a few
//      hundred peer cycles. M3 R6's OffererDriverObserver::OnClosed
//      fires AFTER pc_ is dropped (see cb_offerer_driver.h:84-85 + the
//      "The driver has already dropped the PC" line at :211), so we
//      cannot wait for OnClosed to do the right thing — the embedder
//      MUST call PrepareForTeardown() BEFORE driver.Close().
//
//   2. ADM ref-cycle care. The ADM is owned by the PCF dependencies
//      (PCF construction takes ownership in cloud_browser_pcf.cc; PCF's
//      AudioState holds it for the life of the PCF, which is process-
//      static per M1). Source and track each scoped_refptr the ADM
//      transitively through libwebrtc-internal state. R5 deliberately
//      does NOT take an owning ref to the ADM — the embedder's PCF
//      owns it and outlives any single peer. We accept only a raw
//      diagnostic pointer (used solely for log lines) so a misuse
//      cannot extend the ADM's lifetime past the PCF's.
//
// # Composition with M3 R6's observer slot
//
// CbOffererDriver takes a single OffererDriverObserver*. M5.5 R5 wants
// to observe the same events as the embedder, so we implement a
// chained observer: the embedder installs CbAudioLifecycle as the
// driver's observer, and CbAudioLifecycle forwards every event to a
// downstream OffererDriverObserver* (the embedder's "main" observer,
// e.g. CloudBrowserBrowserMainParts) AFTER its own handling. This
// keeps R5 invisible to the embedder's event handlers and avoids
// requiring an R6 amendment for multi-observer support.
//
// # Sequence (happy path)
//
//   1. Embedder constructs the CbOffererDriver(...) with
//      CbAudioLifecycle as its observer + the embedder's "main"
//      observer threaded through CbAudioLifecycle::SetDownstreamObserver.
//   2. Embedder calls driver.Start(), which transitions through
//      kCreatingPc and (after the embedder's add-tracks step) emits
//      the first `offer` envelope.
//   3. Embedder runs M5.5 R4's helper to materialise the audio source,
//      audio track, and sendonly audio transceiver on driver.pc(), then
//      calls audio_lifecycle.AdoptBindings(source, track, transceiver).
//   4. SDP negotiation completes, ICE connects. libwebrtc internally
//      starts the sender; the ADM's StartRecording fires on the
//      worker task queue. CbAudioLifecycle's OnIceConnectionStateChanged
//      logs the "audio-up" metric (single-line; no PII).
//   5. Steady-state. libwebrtc pumps PCM through the sender; PulseAudio
//      captures cb_capture.monitor (per infra/pulse-default.pa).
//   6. Embedder decides to end the session:
//        a. audio_lifecycle.PrepareForTeardown("session ended")
//             - transceiver_->StopStandard()  ← triggers sender teardown,
//                                               unhooks source from
//                                               AudioState, ADM
//                                               StopRecording fires on
//                                               worker thread.
//             - Drops transceiver_, track_, source_ refs in that order.
//             - state_ = kStopped.
//        b. driver.Close("session ended")
//             - Sends `bye`, transitions to kClosed, drops pc_.
//             - Fires OnClosed via our observer chain; we log only.
//
// # Sequence (failure / async-teardown path)
//
//   1. libwebrtc fires OnIceConnectionStateChange(kIceConnectionFailed)
//      on the driver.
//   2. Driver routes via FailWithReason → drops pc_ → fires
//      observer_->OnFailed("ice failed").
//   3. CbAudioLifecycle::OnFailed runs the best-effort teardown:
//      transceiver_->StopStandard() is attempted (libwebrtc will
//      typically return RTCErrorType::INVALID_STATE because the PC
//      is gone — logged at INFO, not WARNING, because this is the
//      known shape); refs are dropped unconditionally; state_ is
//      forced to kStopped. The orphan-stream window is bounded to
//      "however long libwebrtc's worker teardown took"; the worst
//      case observed in M1 fixture work was ~50 ms.
//
// Cross-references:
//   * capture/audio/cb_audio_device_module.{h,cc}              (M5.5 R1)
//   * capture/audio/cb_audio_source.{h,cc}                     (M5.5 R4
//                                                               draft —
//                                                               see TODO
//                                                               below)
//   * capture/signaling/cb_offerer_driver.{h,cc}               (M3 R4/R6)
//   * capture/build-integration/cloud_browser_pcf.{h,cc}       (M1 / R1)
//   * infra/pulse-default.pa
//
// TODO(M55-R5-r4-handoff): M5.5 R4 (cv2/m55-r4-audio-transceiver,
//   CV2-31) is being drafted in parallel and has not landed at the time
//   this header was written. The AdoptBindings() signature commits to
//   the libwebrtc-standard interfaces (AudioSourceInterface +
//   AudioTrackInterface + RtpTransceiverInterface), which is what any
//   reasonable R4 shape will produce. If R4 wraps the source in a
//   cloud_browser_audio_source class (a CbAudioSource subclass of
//   AudioSourceInterface), AdoptBindings still accepts it via
//   scoped_refptr<AudioSourceInterface> upcast — no signature churn
//   required. Confirm at R4-R5 merge time and amend this TODO line.

#ifndef CAPTURE_AUDIO_CB_AUDIO_LIFECYCLE_H_
#define CAPTURE_AUDIO_CB_AUDIO_LIFECYCLE_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtp_transceiver_interface.h"
#include "api/scoped_refptr.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "capture/signaling/cb_offerer_driver.h"

namespace cloud_browser::audio {

// Forward-declared so consumers don't need to pull in libwebrtc's full
// AudioDeviceModule transitive set just to log the ADM pointer in a
// debug field. We hold a raw_ptr only, never a scoped_refptr — see
// header file's "ADM ref-cycle care" section.
}  // namespace cloud_browser::audio

namespace webrtc {
class AudioDeviceModule;
}  // namespace webrtc

namespace cloud_browser::audio {

// Lifecycle-state of the audio pipeline as observed by R5. Linear in
// the happy path; AdoptBindings is gated by kArmed → kActive, teardown
// converges on kStopped from any non-kIdle state.
enum class AudioLifecycleState : uint8_t {
  kIdle,      // Constructed; AdoptBindings not yet called.
  kArmed,     // AdoptBindings landed; awaiting OnIceConnected to
              // promote to kActive. Send-side libwebrtc machinery is
              // wired but may not yet have called ADM::StartRecording.
  kActive,    // OnIceConnectionStateChanged(kIceConnectionConnected)
              // observed; we expect StartRecording to be in flight or
              // already complete. Steady state.
  kStopped,   // PrepareForTeardown completed, OR a best-effort
              // teardown ran from OnClosed/OnFailed. Refs released;
              // re-entry is a no-op.
};

// Optional debug observer. Embedder may pass nullptr; events are
// otherwise logged via VLOG / LOG(WARNING) only. Fires on the lifecycle
// owner's UI thread — same thread as the driver observer callbacks
// (M3 R4/R6 contract).
class AudioLifecycleObserver {
 public:
  virtual ~AudioLifecycleObserver() = default;

  // Fires on the kArmed → kActive transition (first
  // OnIceConnectionStateChanged with kIceConnectionConnected after
  // AdoptBindings). The "audio sender is presumed live" signal —
  // consumers may begin gating "audio enabled" UI state here.
  virtual void OnAudioActivated() {}

  // Fires once on the transition into kStopped, regardless of which
  // teardown path executed (PrepareForTeardown, OnClosed, OnFailed).
  // |graceful| is true iff PrepareForTeardown drove the teardown
  // (i.e. the orphan-stream-safe ordering ran). |reason| is the
  // string supplied to whichever entry point fired.
  virtual void OnAudioStopped(bool graceful, std::string_view reason) {}
};

class CbAudioLifecycle : public signaling::OffererDriverObserver {
 public:
  // |downstream|:  optional pass-through observer. Every
  //                OffererDriverObserver event we receive from the
  //                driver is forwarded to |downstream| AFTER our own
  //                handling completes (so the embedder's main observer
  //                sees teardown notifications with R5's refs already
  //                released). nullptr is fine.
  // |observer|:    optional R5-specific debug observer. Independent
  //                of |downstream|; both may be set.
  // |ui_runner|:   the task runner for the lifecycle owner's UI thread.
  //                All driver callbacks already arrive on this thread
  //                (M3 R4/R6 contract); ui_runner_ is captured purely
  //                for SEQUENCE_CHECKER + WeakPtr-hop hygiene if a
  //                future revision needs to PostTask back to ourselves
  //                (e.g. delayed-stop fallback). Today every method
  //                runs inline on the calling thread.
  // |adm_debug|:   raw, non-owning pointer to the AudioDeviceModule the
  //                PCF constructed via M5.5 R1's helper. Used SOLELY
  //                for log lines (e.g. "stopping audio backed by ADM
  //                <ptr>"). Must outlive the lifecycle owner; PCF
  //                is process-static in M1 so this is trivially true
  //                in production. Tests may pass nullptr.
  CbAudioLifecycle(
      signaling::OffererDriverObserver* downstream,
      AudioLifecycleObserver* observer,
      scoped_refptr<base::SequencedTaskRunner> ui_runner,
      raw_ptr<webrtc::AudioDeviceModule> adm_debug);

  CbAudioLifecycle(const CbAudioLifecycle&) = delete;
  CbAudioLifecycle& operator=(const CbAudioLifecycle&) = delete;

  ~CbAudioLifecycle() override;

  // Hand the M5.5 R4 send-side audio bindings to R5 for lifecycle
  // ownership. Called by the embedder AFTER driver.Start() returned and
  // AFTER R4's helper materialised the source + track + transceiver on
  // driver.pc(). Transitions kIdle → kArmed.
  //
  // The lifecycle takes scoped_refptr ownership of all three; the
  // embedder MUST NOT retain its own scoped_refptr to any of them past
  // this call — that would defeat the orphan-stream-safe teardown
  // ordering (drop order is transceiver → track → source after
  // transceiver->StopStandard() returns, and we need to be the last
  // strong ref so drop actually unhooks libwebrtc's internal sender
  // state).
  //
  // Idempotent: a second call is logged at WARNING and ignored. Calls
  // in kStopped are also logged + ignored — a once-stopped lifecycle
  // does not get reused (the embedder should construct a new
  // lifecycle alongside the new CbOffererDriver for the next session).
  //
  // |source|:      AudioSourceInterface backed by libwebrtc internals
  //                that the ADM feeds. The exact source type comes
  //                from M5.5 R4 (see header TODO).
  // |track|:       AudioTrackInterface wrapping |source|, with the
  //                track id chosen by R4 (typically "cb-audio-0").
  // |transceiver|: sendonly RtpTransceiverInterface returned by
  //                pc->AddTransceiver(track, init) in R4.
  void AdoptBindings(
      webrtc::scoped_refptr<webrtc::AudioSourceInterface> source,
      webrtc::scoped_refptr<webrtc::AudioTrackInterface> track,
      webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver);

  // Graceful teardown gate. The embedder MUST call this BEFORE
  // calling CbOffererDriver::Close() so that the audio sender is
  // stopped — and the ADM's StopRecording is allowed to run — while
  // the PeerConnection is still alive (see header's "orphan PulseAudio
  // streams" failure-mode discussion).
  //
  // Behaviour:
  //   * kArmed / kActive: runs the orphan-stream-safe sequence:
  //       1. transceiver_->StopStandard()  — synchronous return;
  //          libwebrtc schedules sender teardown on worker thread.
  //       2. Drop transceiver_ ref. ← last strong ref typically.
  //       3. Drop track_ ref.
  //       4. Drop source_ ref. ← libwebrtc-internal source teardown
  //          chain runs here; ADM StopRecording resolves on worker.
  //       5. state_ = kStopped; fires observer_->OnAudioStopped(
  //          graceful=true, reason).
  //   * kIdle: nothing to tear down; just transition to kStopped
  //     so OnClosed/OnFailed don't run the best-effort cleanup.
  //     Fires OnAudioStopped(graceful=true, reason).
  //   * kStopped: no-op (idempotent).
  //
  // |reason| flows to the observer + log; informational only.
  void PrepareForTeardown(std::string_view reason);

  // CV2-REARM: return a stopped lifecycle to kIdle so the SAME object can
  // adopt the NEXT session's bindings.
  //
  // The header above says "construct a new lifecycle for a new session", and
  // that was right while a session ended with the process. It is not possible
  // once the worker re-arms in place: CbOffererDriver holds this object by RAW
  // POINTER as its observer (see the ctor call in
  // cloud_browser_browser_main_parts.cc), and there is no way to swap that
  // pointer — destroying and rebuilding the lifecycle would leave the driver
  // observing freed memory.
  //
  // So the object address stays stable and only the SESSION state resets.
  // Without this, AdoptBindings on the second session hits the kStopped guard,
  // logs "construct a new lifecycle" at WARNING, and silently ignores the
  // bindings — the worker would keep video but lose AUDIO from the second
  // viewer onward, with nothing failing loudly.
  //
  // Safe only from kStopped: re-arming a live lifecycle would abandon a
  // running capture. Returns false (and logs) otherwise.
  bool Rearm();

  // State accessor for tests + the embedder's diagnostics path.
  AudioLifecycleState state() const;

  // --- OffererDriverObserver overrides --------------------------------
  // All fire on ui_runner_'s sequence (M3 R4/R6 contract). Each
  // forwards to downstream_ AFTER R5's own handling.
  void OnIceConnectionStateChanged(
      webrtc::PeerConnectionInterface::IceConnectionState state) override;
  void OnRenegotiationStarted(std::string_view trigger) override;
  void OnRenegotiationCompleted() override;
  // Pure pass-through. This class sits BETWEEN the driver and the embedder, so
  // an un-overridden observer method is silently swallowed by the base class's
  // empty default — the embedder would never learn a new viewer needs an
  // offer, and the cold-arrival fix would do nothing at all.
  void OnNewViewerNeedsOffer() override;
  void OnClosed(std::string_view reason) override;
  void OnFailed(std::string_view reason) override;

 private:
  // Common teardown body. |graceful| selects between the orphan-
  // stream-safe ordering (PrepareForTeardown's path) and the best-
  // effort ordering used from OnClosed/OnFailed when the PC has
  // already been dropped by the driver.
  //
  // |source| is the human-readable origin tag for log lines:
  //   "prepare"   — PrepareForTeardown called by embedder.
  //   "on-closed" — driver fired OnClosed before PrepareForTeardown.
  //   "on-failed" — driver fired OnFailed (terminal failure path).
  //   "dtor"      — destructor cleanup, embedder leaked the lifecycle.
  void StopInternal(bool graceful, std::string_view reason,
                    std::string_view source);

  // Pass-through helper — invoked at the tail of every observer
  // method when downstream_ is non-null. Centralises the nullptr
  // guard.
  void ForwardIceConnectionState(
      webrtc::PeerConnectionInterface::IceConnectionState state);
  void ForwardRenegotiationStarted(std::string_view trigger);
  void ForwardRenegotiationCompleted();
  void ForwardClosed(std::string_view reason);
  void ForwardFailed(std::string_view reason);

  // Construction-time inputs.
  raw_ptr<signaling::OffererDriverObserver> downstream_;
  raw_ptr<AudioLifecycleObserver> observer_;
  scoped_refptr<base::SequencedTaskRunner> ui_runner_;
  raw_ptr<webrtc::AudioDeviceModule> adm_debug_;

  // Owned lifetimes — populated by AdoptBindings, released in
  // StopInternal. Ordering of declaration matches release order in
  // StopInternal (transceiver → track → source) so accidental
  // destructor-driven drops also unwind in the right order.
  webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver_;
  webrtc::scoped_refptr<webrtc::AudioTrackInterface> track_;
  webrtc::scoped_refptr<webrtc::AudioSourceInterface> source_;

  AudioLifecycleState state_ = AudioLifecycleState::kIdle;

  // Latches OnAudioActivated() to a single fire per session. R5
  // intentionally does NOT re-fire on renegotiation: M3 R6's
  // OnRenegotiationStarted/Completed pair flips ICE state through
  // a brief disconnected→connected cycle, and we don't want to
  // double-emit the "audio up" metric.
  bool audio_activated_emitted_ = false;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<CbAudioLifecycle> weak_factory_{this};
};

}  // namespace cloud_browser::audio

#endif  // CAPTURE_AUDIO_CB_AUDIO_LIFECYCLE_H_

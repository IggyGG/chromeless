// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cb_audio_track — Module M5.5 R4 of the ChromelessV2 native-peer
// migration (parent CV2-8; M5.5 umbrella CV2-25; this R4 is CV2-31).
//
// CV2-31 wires the send-side audio path on the browser-process
// PeerConnection: ADM (R1) → AudioSource → AudioTrack → sendonly
// transceiver. It is the cross-module hand-off that turns the three
// upstream R# pieces into a single observable artifact on the native
// peer — a transceiver carrying an opus-encoded audio stream from
// PulseAudio's cb_capture.monitor source out over RTP.
//
// Composition (the only thing this R# does):
//
//   1. AudioOptions (M5.5 R3 — CV2-30) supplies the cricket::
//      AudioOptions value with APM disabled (no echo cancellation,
//      no AGC, no noise suppression). Display-capture audio is
//      already-mixed program audio; running APM over it would
//      corrupt music and voiceovers. R3 owns the policy decision +
//      the helper that returns the configured options value; R4
//      consumes the helper and threads its return value into
//      PCF::CreateAudioSource.
//
//   2. PeerConnectionFactoryInterface (M1 — CV2-26/27, owned by
//      CloudBrowserBrowserMainParts::pcf_) supplies:
//        - CreateAudioSource(options) → AudioSourceInterface
//          backed by the AudioDeviceModule installed at R1 (CV2-28).
//          The PCF's worker thread drives the ADM's recording pump;
//          captured frames flow into the source.
//        - CreateAudioTrack(label, source) → AudioTrackInterface
//          wrapping the source. The track is what AddTransceiver
//          accepts.
//
//   3. PeerConnectionInterface (M3 R4 — CV2-54, the offerer driver's
//      pc_) accepts the track via AddTransceiver(track,
//      RtpTransceiverInit{direction=kSendOnly}). The transceiver
//      shows up in the next CreateOffer's SDP as
//        m=audio 9 UDP/TLS/RTP/SAVPF <opus payload type>
//        a=sendonly
//      which is the M5.5 acceptance shape (CV2-25 §"observable on
//      the native peer").
//
// Why "sendonly" and not "sendrecv":
//
//   The cloud-browser worker captures program audio and ships it to
//   the user; it does NOT accept inbound audio from the user (no
//   microphone-into-browser path). Declaring sendrecv would lie
//   about the direction and force the remote peer to allocate an
//   audio decoder + jitter buffer it would never feed. sendonly is
//   the accurate direction and matches what M2's video transceiver
//   does. The "recv" half is reserved for a future M-# if/when an
//   inbound audio use case lands; right now no such use case exists.
//
// Why a factory function (not a class):
//
//   The source/track/transceiver are all webrtc webrtc::scoped_refptr
//   types and the PC already owns the transceiver after AddTransceiver
//   returns. R4 doesn't need to remember anything across calls — once
//   the transceiver is on the PC, lifetime is the PC's problem. A
//   bare factory returning the new refs to the caller is the smallest
//   shape that makes the composition testable.
//
//   The caller (M5.5 R5 — CV2-32, audio lifecycle parity) owns the
//   returned refs and is responsible for ordering teardown vs the PC's
//   own teardown. R4 stays narrowly scoped to construction.
//
// Threading:
//
//   * CreateAudioSource / CreateAudioTrack are PCF methods. Per
//     CV2-26 R-thread DECISION, PCF methods MUST be marshaled onto
//     the signaling_thread_ owned by CloudBrowserBrowserMainParts.
//     M2's video track source and M3 R4's offerer driver both
//     observe this; R4 inherits the same contract via the caller —
//     the embedder MUST call AddSendOnlyAudioTransceiver on the
//     signaling thread. R4 itself does NOT thread-hop; the caller's
//     marshaling guarantees correctness.
//
//   * AddTransceiver is a PC method and is also signaling-thread-
//     affine. Same contract.
//
//   * The ADM's internal recording task queues are driven by the
//     PCF's worker thread (per R1's
//     CreateCloudBrowserNativeAudioDeviceModule contract); R4 does
//     not interact with those threads directly.
//
// Cross-references:
//
//   * capture/audio/cb_audio_device_module.{h,cc}            (R1)
//   * capture/audio/cb_audio_options.h                       (R3)
//     TODO(M55-R4-r3-header-name): R3 is drafting in parallel on
//     branch cv2/m55-r3-media-audio-options. Confirm the actual
//     header path + function name when R3 lands and update the
//     include + call site below. The draft uses the most likely
//     names; the call shape (returns webrtc::AudioOptions by
//     value) is what matters for the composition.
//   * capture/build-integration/cloud_browser_pcf.{h,cc}     (M1)
//   * capture/signaling/cb_offerer_driver.{h,cc}             (M3 R4)
//   * Plane CV2-31 (the R4 ticket itself).

#ifndef CAPTURE_AUDIO_CB_AUDIO_TRACK_H_
#define CAPTURE_AUDIO_CB_AUDIO_TRACK_H_

#include <string>

#include "api/audio_options.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtp_transceiver_interface.h"
#include "api/scoped_refptr.h"

namespace cloud_browser {

// The composed result of R4: the source + track + transceiver triple
// that AddSendOnlyAudioTransceiver constructs in one shot.
//
// All three refs are returned so the caller (M5.5 R5 lifecycle owner)
// can:
//   * keep the source alive at least as long as the track (libwebrtc
//     does not hold a strong ref source→track in all configurations);
//   * inspect the transceiver's mid / sender state for the M5.5
//     acceptance assertion (CV2-25);
//   * call transceiver->StopStandard() in teardown before dropping
//     the PC ref (R5's parity-with-M2 contract).
//
// On construction failure (any of the three steps returning null),
// the struct's transceiver field is null and the caller MUST treat
// the entire composition as failed — partial cleanup is the caller's
// responsibility but in practice an early-stage failure leaves the
// source/track also null because we don't promote partial state.
struct SendOnlyAudioTransceiver {
  webrtc::scoped_refptr<webrtc::AudioSourceInterface> source;
  webrtc::scoped_refptr<webrtc::AudioTrackInterface> track;
  webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver;
};

// Compose the send-side audio path on the browser-process native peer.
//
// Steps (see top-of-file Composition section for rationale):
//   1. pcf->CreateAudioSource(options) — options sourced from
//      M5.5 R3's helper, threaded through |audio_options|.
//   2. pcf->CreateAudioTrack(|track_label|, source.get()).
//   3. pc->AddTransceiver(track, RtpTransceiverInit{direction=
//      kSendOnly}). The returned RTCErrorOr is unwrapped — on error
//      the function logs the verbatim error message and returns a
//      struct with all three fields null.
//
// |pcf|, |pc|: NON-OWNING raw pointers. The caller owns the refs
//   (typically CloudBrowserBrowserMainParts::pcf_ and the offerer
//   driver's pc_) and must keep them alive across the call. The
//   returned refs hold their own strong refs as appropriate; this
//   function does not extend |pcf|'s or |pc|'s lifetime.
//
// |audio_options|: the webrtc::AudioOptions value from R3's
//   helper. R4 does NOT construct the options itself — that is
//   R3's policy domain. R4 only knows that whatever R3 returns is
//   what gets handed to CreateAudioSource.
//
//   TODO(M55-R4-audio-options-type): the parameter type is intended
//   to be webrtc::AudioOptions (the libwebrtc public type), but
//   different chromium-bundled webrtc revisions surface it as either
//   webrtc::AudioOptions or webrtc::AudioOptions. The draft uses
//   webrtc::AudioOptions per the prevailing M1-era convention;
//   confirm during the first compile on triform-8 and swap if the
//   chromium-bundled tree has migrated. The forward-declared name
//   below stays the same regardless.
//
// |track_label|: the AudioTrack's label string. Default
//   "cb_audio_send" matches the M2 video track's "cb_video_send"
//   naming convention. The label appears in SDP a=msid lines and
//   the libwebrtc-level RtpTransceiver mid; keeping it stable is
//   nice for log greppability but is NOT a hard contract — the
//   M5.5 acceptance assertion (CV2-25) only checks for an audio
//   m-section with a=sendonly, not for any specific label.
//
// Returns: a SendOnlyAudioTransceiver struct with all three fields
//   populated on success. On any step failure, every field is null.
//
// Threading: signaling-thread-affine. See top-of-file Threading
//   section. The caller MUST marshal this call onto the PCF/PC's
//   signaling_thread_; R4 does not thread-hop internally.
SendOnlyAudioTransceiver AddSendOnlyAudioTransceiver(
    webrtc::PeerConnectionFactoryInterface* pcf,
    webrtc::PeerConnectionInterface* pc,
    const webrtc::AudioOptions& audio_options,
    const std::string& track_label = "cb_audio_send");

}  // namespace cloud_browser

#endif  // CAPTURE_AUDIO_CB_AUDIO_TRACK_H_

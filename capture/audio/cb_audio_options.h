// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cb_audio_options — Module M5.5 of the ChromelessV2 native-peer
// migration (parent CV2-8; this R3 is CV2-30).
//
// CV2-30 builds the webrtc::AudioOptions struct the cloud-browser
// worker attaches to the AudioSource / AudioTrack constructed on top
// of the M5.5 R1 ADM (kPlatformDefaultAudio → built-in PulseAudio,
// recording the cb_capture.monitor server-default source).
//
// The cb-chromium use case is fundamentally different from libwebrtc's
// default audio profile:
//
//   * Default profile (microphone, voice call):
//     APM ON — AEC + AGC + NS + highpass + typing-detection. Tuned
//     for a single human speaker reading off a near-field mic into
//     a 16 kHz mono pipeline. The processing chain is voice-shaped:
//     suppress non-voice spectra, normalize level, kill room echo.
//
//   * cb-chromium profile (media / program audio):
//     APM OFF. The audio coming out of cb_capture.monitor is the
//     browser tab's mixed program output — music, video, game audio,
//     UI sounds, multi-speaker conference playback. Running it
//     through voice-tuned APM corrupts the signal:
//       - AEC: nothing to cancel (no acoustic loop), but the AEC
//         filter still mangles transients.
//       - NS: spectral subtraction notches out non-voice content,
//         e.g. music's high-frequency detail and sub-bass.
//       - AGC: aggressive level normalization flattens dynamics and
//         pumps on percussion.
//       - HPF: 80 Hz cutoff strips bass from music + low-frequency
//         film/game content.
//       - Typing detection: pattern-matches keypress transients in
//         the spectrum and gates them out — wrong heuristic for
//         every non-keyboard transient (drums, gunshots, UI chimes).
//
// M5.5 R3 ratifies the "media-grade pass-through" decision: APM off,
// no client-side processing. The viewer-side WebRTC stack still does
// its own jitter buffer / PLC / decode, but the encode path is
// transparent.
//
// Why a separate file (not just an inline struct literal in
// cloud_browser_pcf.cc):
//   - The options block is the M5.5 R3 cross-module hand-off seam.
//     Future tuning revisions (R4 stereo / Opus stereo, R5 Opus
//     bitrate cap, etc.) replace ONLY this file's body; the PCF +
//     track construction sites stay stable.
//   - Keeps the rationale (above) co-located with the field set, so
//     a future reader doesn't have to chase commit messages to learn
//     why noise_suppression = false for "audio capture".
//
// Cross-references:
//   * capture/audio/cb_audio_device_module.{h,cc} (M5.5 R1, CV2-28)
//     Constructs the native ADM these options ride on top of.
//   * capture/build-integration/cloud_browser_pcf.{h,cc} (M1, CV2-27)
//     The PCF construction site that owns the ADM + will own the
//     AudioSource the BuildMediaAudioOptions() output is attached to.
//     (Track-attachment wiring lands in a follow-up R; this R3 only
//     defines the options block.)
//   * RED test from M5.5 R2 (CV2-29) validates the audio quality of
//     the integrated R1+R3 path against the reference monitor signal.
//   * Plane CV2-30 (the R3 ticket itself).

#ifndef CAPTURE_AUDIO_CB_AUDIO_OPTIONS_H_
#define CAPTURE_AUDIO_CB_AUDIO_OPTIONS_H_

#include "api/audio_options.h"

namespace cloud_browser {

// Build the webrtc::AudioOptions block to attach to the cloud-browser
// worker's AudioSource / AudioTrack.
//
// The returned options disable every APM stage:
//   * echo_cancellation = false
//   * auto_gain_control = false
//   * noise_suppression = false
//   * highpass_filter   = false
//   * typing_detection  = false
//
// Rationale (full): see file-header comment. Short version: the
// cb_capture.monitor source carries the browser tab's mixed program
// audio (music, video, game audio, UI), NOT a microphone. APM is
// voice-tuned signal processing and corrupts media content. The
// cb-chromium path is media-grade pass-through.
//
// Returns:
//   A webrtc::AudioOptions by value (cheap — it's a struct of
//   absl::optional<bool/int> fields). Caller passes it to
//   PeerConnectionFactoryInterface::CreateAudioSource(options) or
//   the equivalent track-side setter.
//
// Threading:
//   Pure value construction. Safe to call from any thread; the
//   returned struct contains no shared state.
//
// TODO(M55-R3-stereo): once R4 brings up stereo capture from the
// PulseAudio monitor source (today the ADM is mono — choice b
// constraint), add stereo_swapping = false here explicitly so the
// channel order is pinned and reviewable in one place.
webrtc::AudioOptions BuildMediaAudioOptions();

}  // namespace cloud_browser

#endif  // CAPTURE_AUDIO_CB_AUDIO_OPTIONS_H_

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cb_audio_options.cc — see cb_audio_options.h.
//
// CV2-30 R3: media-grade AudioOptions / APM disable. Every APM stage
// surfaced by Chromium 147's webrtc::AudioOptions is set to false; the
// cb_capture.monitor source carries program audio, not a microphone,
// and the voice-tuned default profile corrupts media content (see
// header for the per-stage rationale).

#include "capture/audio/cb_audio_options.h"

#include "api/audio_options.h"
#include "rtc_base/logging.h"

namespace cloud_browser {

webrtc::AudioOptions BuildMediaAudioOptions() {
  webrtc::AudioOptions options;

  // Acoustic echo cancellation. No physical acoustic loop exists in
  // the cb-chromium worker — the audio is captured from the
  // PulseAudio monitor source, not a microphone, so there is no
  // far-end speaker output feeding back into a near-end mic. AEC
  // here would only mangle transients without cancelling anything
  // real.
  options.echo_cancellation = false;

  // Automatic gain control. Media content is mastered with intentional
  // dynamics — film score swells, game audio impact peaks, music
  // verse-vs-chorus level differences. AGC's voice-tuned level
  // normalization flattens those dynamics and pumps on percussion;
  // we want the levels to land at the viewer untouched.
  options.auto_gain_control = false;

  // Noise suppression. NS is spectral subtraction tuned for
  // human-voice spectra (250 Hz – 4 kHz center of mass). Applied to
  // program audio it notches out music's high-frequency detail and
  // sub-bass content, gates out low-level ambience that the content
  // author put there on purpose, and produces audible "swimming"
  // artifacts on sustained non-voice content.
  options.noise_suppression = false;

  // High-pass filter (~80 Hz cutoff in libwebrtc's APM). Strips bass
  // from music and low-frequency content from film / game audio.
  // For program audio we want the full spectrum delivered.
  options.highpass_filter = false;

  // Chromium 147 no longer exposes a public AudioOptions typing_detection
  // field. If an older libwebrtc revision still has an internal typing
  // detector, there is no public source-level knob to toggle here; the
  // exported APM controls above are the current API surface.

  // TODO(M55-R3-stereo): when R4 brings up stereo capture from the
  // PulseAudio monitor source, set options.stereo_swapping = false
  // here explicitly so channel order is pinned. Leaving it
  // absl::nullopt today keeps the field at libwebrtc's default
  // (off), which is the right value for the mono path R1 ships.

  RTC_LOG(LS_INFO) << "CloudBrowser: BuildMediaAudioOptions — APM "
                      "disabled (AEC/AGC/NS/HPF false) for "
                      "media/program audio pass-through";

  return options;
}

}  // namespace cloud_browser

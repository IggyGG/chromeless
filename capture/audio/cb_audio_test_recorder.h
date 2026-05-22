// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cb_audio_test_recorder — minimal AudioTransport implementation used
// by cb_audio_device_module_test.cc (Module M5.5 — CV2-29 / R2) to
// observe whether the libwebrtc AudioDeviceModule constructed by
// CreateCloudBrowserNativeAudioDeviceModule() (R1 / CV2-28) actually
// delivers recorded samples and, if so, whether those samples carry
// non-silent energy.
//
// Why not reuse webrtc::test::FakeAudioCaptureModule or TestAudioDevice
// Module's renderer side:
//   * Those exercise libwebrtc's *own* mocking surface — the same code
//     path that bypasses the platform-default backend the M5.5 "choice
//     b" decision needs to validate. M5.5 R2 has to drive the REAL
//     libwebrtc Linux PulseAudio ADM end-to-end and capture what it
//     emits; a fake ADM would moot the test.
//
// Contract:
//   * Implements webrtc::AudioTransport — the interface libwebrtc's
//     ADM invokes when InitRecording + StartRecording have succeeded
//     and the capture pump has 10ms of PCM to deliver.
//   * Buffers received samples in-memory, capped at kMaxBufferedSamples
//     to keep test runs bounded (~10 s @ 48 kHz mono = 480k samples,
//     960 KiB at 16-bit — well within unit-test memory budget).
//   * Computes RMS over the buffered samples — the empirical signal
//     M5.5 R2's iron acceptance criterion gates on:
//
//         tone -> recorded frames at the ADM with RMS above silence
//         threshold within N seconds
//
//     (per Plane CV2-29 description).
//   * Exposes WaitForFrames(deadline) so the test fixture can block
//     until N frames have been delivered (or the deadline trips and
//     the test reports a RED verdict).
//
// Thread safety:
//   * libwebrtc invokes AudioTransport on its own internal task queue
//     (drawn from the TaskQueueFactory threaded into R1's helper).
//     The fixture reads buffered state from the test's main thread.
//     All shared state goes through |lock_|.
//
// TODO(M55-R2-test-audio-device-header): some libwebrtc branches ship
//   AudioTransport at api/audio/audio_device.h, others at
//   modules/audio_device/include/audio_transport.h. The webrtc_overrides
//   passthrough re-exports the api/ flavour today (used by the M1 PCF
//   test fixture's TestAudioDeviceModule include); confirm the same
//   header still satisfies AudioTransport in this branch when the test
//   first goes through the chromium build pod. If not, widen the
//   passthrough rather than reaching past it.

#ifndef CAPTURE_AUDIO_CB_AUDIO_TEST_RECORDER_H_
#define CAPTURE_AUDIO_CB_AUDIO_TEST_RECORDER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "api/audio/audio_device.h"  // AudioTransport, see TODO above.
#include "rtc_base/synchronization/mutex.h"
#include "rtc_base/thread_annotations.h"

namespace cloud_browser {
namespace test {

// 10 s @ 48 kHz mono — see file header. Configurable via the ctor for
// tests that need a longer window (e.g. a deliberately slow tone source
// inside the M0 R7 harness).
inline constexpr size_t kDefaultMaxBufferedSamples = 48000 * 10;

class CbAudioTestRecorder : public webrtc::AudioTransport {
 public:
  // |max_buffered_samples| caps the in-memory PCM buffer (16-bit mono).
  // Once reached, subsequent RecordedDataIsAvailable calls become no-ops
  // so we don't fight the capture pump for memory in a wedged-test
  // scenario. The default (kDefaultMaxBufferedSamples) is sized for a
  // 10 s acceptance window; pass a larger value only when the harness
  // intentionally needs more head-room.
  explicit CbAudioTestRecorder(
      size_t max_buffered_samples = kDefaultMaxBufferedSamples);
  ~CbAudioTestRecorder() override;

  CbAudioTestRecorder(const CbAudioTestRecorder&) = delete;
  CbAudioTestRecorder& operator=(const CbAudioTestRecorder&) = delete;

  // --- AudioTransport overrides --------------------------------------
  //
  // RecordedDataIsAvailable is the capture-side hook the M5.5 R2 test
  // gates on. NeedMorePlayData is implemented as a silence-renderer so
  // libwebrtc's render path doesn't underflow during the test (some
  // libwebrtc Linux ADM revisions interleave capture + render init);
  // the M5.5 contract is capture-only and the test ignores any render
  // pull.
  int32_t RecordedDataIsAvailable(
      const void* audio_data,
      size_t samples_per_channel,
      size_t bytes_per_sample,
      size_t number_of_channels,
      uint32_t sample_rate,
      uint32_t audio_delay_milliseconds,
      int32_t clock_drift,
      uint32_t volume,
      bool key_pressed,
      uint32_t& new_mic_volume) override;

  int32_t NeedMorePlayData(size_t samples_per_channel,
                           size_t bytes_per_sample,
                           size_t number_of_channels,
                           uint32_t sample_rate,
                           void* audio_data,
                           size_t& n_samples_out,
                           int64_t* elapsed_time_ms,
                           int64_t* ntp_time_ms) override;

  // --- Observers (test-side) -----------------------------------------

  // Total number of Recorded callbacks delivered since construction —
  // distinct from sample count. Useful for asserting "the capture pump
  // ran at all" separately from "the capture pump delivered audible
  // signal". A pump that never fires fails the first check; a pump
  // that fires but only delivers silence fails the second.
  size_t RecordedCallCount() const;

  // Total number of 16-bit PCM samples buffered (mono-equivalent —
  // multi-channel frames are summed into the 16-bit buffer downmixed,
  // see the .cc).
  size_t BufferedSampleCount() const;

  // Sample rate observed on the most recent Recorded callback (Hz).
  // Returns 0 until the first callback lands. The M5.5 acceptance
  // criterion implicitly demands a non-zero rate — a degenerate ADM
  // that delivers zero-rate frames would let an RMS-of-zero check
  // pass vacuously.
  uint32_t LastSampleRateHz() const;

  // Compute RMS over the buffered samples. Returns 0.0 if no samples
  // have been recorded yet. Range: [0.0, 32768.0] (16-bit signed PCM).
  //
  // The "silence floor" the M5.5 R2 acceptance criterion compares
  // against is documented in cb_audio_device_module_test.cc; this
  // method just returns the raw measurement.
  double ComputeRmsSquaredMean() const;

  // Convenience: returns ComputeRmsSquaredMean() taken to sqrt.
  double ComputeRms() const;

  // Block (busy-polling on a 10 ms tick) until at least |min_samples|
  // have been buffered or |deadline_ms| ms have elapsed since the call.
  // Returns true if the sample threshold was reached; false on timeout.
  //
  // The M5.5 R2 fixture uses this to bound the iron test's runtime —
  // RED verdicts surface as a timeout (the capture pump never produced
  // enough samples for an RMS measurement), separate from "samples
  // arrived but RMS too low" RED verdicts.
  bool WaitForSamples(size_t min_samples, int deadline_ms) const;

 private:
  // Append a downmixed-to-mono 16-bit chunk to |buffer_|. Internal —
  // called from RecordedDataIsAvailable under |lock_|.
  void AppendChunkLocked(const int16_t* mono_pcm, size_t samples)
      RTC_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  mutable webrtc::Mutex lock_;
  std::vector<int16_t> buffer_ RTC_GUARDED_BY(lock_);
  size_t recorded_call_count_ RTC_GUARDED_BY(lock_) = 0;
  uint32_t last_sample_rate_hz_ RTC_GUARDED_BY(lock_) = 0;
  const size_t max_buffered_samples_;
};

}  // namespace test
}  // namespace cloud_browser

#endif  // CAPTURE_AUDIO_CB_AUDIO_TEST_RECORDER_H_

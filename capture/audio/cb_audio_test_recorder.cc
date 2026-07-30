// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cb_audio_test_recorder.cc — see cb_audio_test_recorder.h.
//
// CV2-29 / M5.5 R2 — observer half of the empirical ADM-source RED
// test. Buffers 16-bit PCM delivered by the libwebrtc ADM and reports
// RMS; the test fixture compares RMS against a silence floor.

#include "capture/audio/cb_audio_test_recorder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include "rtc_base/logging.h"

namespace cloud_browser {
namespace test {

CbAudioTestRecorder::CbAudioTestRecorder(size_t max_buffered_samples)
    : max_buffered_samples_(max_buffered_samples) {
  // Reserve once — avoids realloc churn under the capture pump's
  // 10ms cadence. 16-bit mono @ 48 kHz over 10 s = 480k * sizeof(int16)
  // = 960 KiB; well within unit-test memory budget. See header.
  buffer_.reserve(max_buffered_samples_);
}

CbAudioTestRecorder::~CbAudioTestRecorder() = default;

int32_t CbAudioTestRecorder::RecordedDataIsAvailable(
    const void* audio_data,
    size_t samples_per_channel,
    size_t bytes_per_sample,
    size_t number_of_channels,
    uint32_t sample_rate,
    uint32_t /*audio_delay_milliseconds*/,
    int32_t /*clock_drift*/,
    uint32_t /*volume*/,
    bool /*key_pressed*/,
    uint32_t& new_mic_volume) {
  // Default the out-param even when we early-out — libwebrtc's
  // AudioTransport contract is that callees must always set
  // new_mic_volume, even if "no change" (signalled with 0). Failing to
  // set it leaves an uninitialised-read in some ADM revisions.
  new_mic_volume = 0;

  // M5.5 R2 only cares about 16-bit PCM — libwebrtc's Linux ADM
  // delivers exactly that today, and re-implementing per-format
  // downmix for an int24/float32 ADM that doesn't exist on this
  // platform would be premature.
  //
  // TODO(M55-R2-format-assumption): if a future libwebrtc revision
  // changes the Linux ADM's wire format, this guard catches it. The
  // test fixture then surfaces it as "ADM delivered unexpected
  // bytes_per_sample" rather than silently mis-interpreting the
  // buffer.
  if (bytes_per_sample != sizeof(int16_t) * number_of_channels) {
    RTC_LOG(LS_WARNING)
        << "CbAudioTestRecorder: dropping chunk — bytes_per_sample="
        << bytes_per_sample << " channels=" << number_of_channels
        << " (expected " << (sizeof(int16_t) * number_of_channels) << ")";
    return 0;
  }
  if (number_of_channels == 0 || samples_per_channel == 0 || sample_rate == 0) {
    return 0;
  }

  // Downmix to mono in 16-bit signed PCM. Multi-channel frames get
  // averaged (sum / channels) — sufficient for an RMS-energy
  // measurement; we're not trying to preserve stereo phase.
  //
  // Allocating on the stack would risk overflow for atypical
  // |samples_per_channel| values; a small heap vector is the
  // path-of-least-surprise.
  std::vector<int16_t> mono;
  mono.resize(samples_per_channel);
  const int16_t* src = static_cast<const int16_t*>(audio_data);
  if (number_of_channels == 1) {
    std::memcpy(mono.data(), src, samples_per_channel * sizeof(int16_t));
  } else {
    for (size_t i = 0; i < samples_per_channel; ++i) {
      int32_t acc = 0;
      for (size_t c = 0; c < number_of_channels; ++c) {
        acc += src[i * number_of_channels + c];
      }
      mono[i] = static_cast<int16_t>(acc / static_cast<int32_t>(number_of_channels));
    }
  }

  webrtc::MutexLock lock(&lock_);
  ++recorded_call_count_;
  last_sample_rate_hz_ = sample_rate;
  AppendChunkLocked(mono.data(), mono.size());
  return 0;
}

int32_t CbAudioTestRecorder::NeedMorePlayData(
    size_t samples_per_channel,
    size_t bytes_per_sample,
    size_t number_of_channels,
    uint32_t /*sample_rate*/,
    void* audio_data,
    size_t& n_samples_out,
    int64_t* elapsed_time_ms,
    int64_t* ntp_time_ms) {
  // Silence-renderer: zero the requested buffer and report the same
  // sample count back. libwebrtc's render path can pull on us during
  // capture init on some Linux ADM revisions; returning silence keeps
  // that path quiet without affecting the capture-side measurement.
  if (audio_data != nullptr) {
    std::memset(audio_data, 0,
                samples_per_channel * bytes_per_sample);
  }
  n_samples_out = samples_per_channel;
  if (elapsed_time_ms) *elapsed_time_ms = -1;
  if (ntp_time_ms) *ntp_time_ms = -1;
  // |number_of_channels| is intentionally unused — silence is
  // channel-agnostic when the buffer is zeroed wholesale above.
  (void)number_of_channels;
  return 0;
}

void CbAudioTestRecorder::PullRenderData(int bits_per_sample,
                                         int /*sample_rate*/,
                                         size_t number_of_channels,
                                         size_t number_of_frames,
                                         void* audio_data,
                                         int64_t* elapsed_time_ms,
                                         int64_t* ntp_time_ms) {
  // Silence-renderer, same contract as NeedMorePlayData above. Unlike
  // that one, this signature reports its buffer geometry in BITS per
  // sample and FRAMES, not bytes and samples — so the size arithmetic
  // is (frames * channels * bits/8), not (samples * bytes). Getting
  // that wrong would zero the wrong length and either under-fill (the
  // caller renders whatever garbage was in the tail) or overrun.
  if (audio_data != nullptr) {
    std::memset(audio_data, 0,
                number_of_frames * number_of_channels *
                    (static_cast<size_t>(bits_per_sample) / 8));
  }
  if (elapsed_time_ms) *elapsed_time_ms = -1;
  if (ntp_time_ms) *ntp_time_ms = -1;
}

size_t CbAudioTestRecorder::RecordedCallCount() const {
  webrtc::MutexLock lock(&lock_);
  return recorded_call_count_;
}

size_t CbAudioTestRecorder::BufferedSampleCount() const {
  webrtc::MutexLock lock(&lock_);
  return buffer_.size();
}

uint32_t CbAudioTestRecorder::LastSampleRateHz() const {
  webrtc::MutexLock lock(&lock_);
  return last_sample_rate_hz_;
}

double CbAudioTestRecorder::ComputeRmsSquaredMean() const {
  webrtc::MutexLock lock(&lock_);
  if (buffer_.empty()) {
    return 0.0;
  }
  // Use double accumulator — int64 would also work for 16-bit PCM at
  // these sizes, but the RMS sqrt below is double anyway and the
  // accumulator-cast in the inner loop is the same cost.
  double acc = 0.0;
  for (int16_t s : buffer_) {
    const double v = static_cast<double>(s);
    acc += v * v;
  }
  return acc / static_cast<double>(buffer_.size());
}

double CbAudioTestRecorder::ComputeRms() const {
  return std::sqrt(ComputeRmsSquaredMean());
}

bool CbAudioTestRecorder::WaitForSamples(size_t min_samples,
                                         int deadline_ms) const {
  // Busy-poll on a 10 ms tick — matches the libwebrtc ADM's natural
  // capture cadence so we don't over-sleep past the first batch.
  // The 10 ms granularity is the same one upstream
  // TestAudioDeviceModule_unittests use; copying that idiom keeps the
  // M5.5 R2 fixture recognisable to anyone who has read those tests.
  using namespace std::chrono;
  const auto deadline = steady_clock::now() + milliseconds(deadline_ms);
  while (steady_clock::now() < deadline) {
    if (BufferedSampleCount() >= min_samples) {
      return true;
    }
    std::this_thread::sleep_for(milliseconds(10));
  }
  return BufferedSampleCount() >= min_samples;
}

void CbAudioTestRecorder::AppendChunkLocked(const int16_t* mono_pcm,
                                            size_t samples) {
  if (buffer_.size() >= max_buffered_samples_) {
    return;  // Cap reached — see header.
  }
  const size_t budget = max_buffered_samples_ - buffer_.size();
  const size_t take = std::min(samples, budget);
  buffer_.insert(buffer_.end(), mono_pcm, mono_pcm + take);
}

}  // namespace test
}  // namespace cloud_browser

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// BweAdapter — see bwe_adapter.h.
//
// TODO(T17-build-env): exercise this on a Linux box with depot_tools
// against the libwebrtc headers from the pinned Chromium tree
// (docs/build/chromium-from-source.md §6).

#include "capture/encoder/bwe_adapter.h"

#include <algorithm>
#include <utility>

#include "rtc_base/logging.h"

namespace cloud_browser {

void BweAdapter::RegisterEncoder(webrtc::VideoEncoder* encoder,
                                  int layer_index) {
  if (encoder == nullptr) return;
  absl::MutexLock lock(&mu_);
  // Idempotent register — duplicates would cause double-fan-out.
  for (const auto& e : encoders_) {
    if (e.encoder == encoder) return;
  }
  encoders_.push_back({encoder, layer_index});
  RTC_LOG(LS_INFO) << "BweAdapter: registered encoder layer="
                   << layer_index << " count=" << encoders_.size();
}

void BweAdapter::UnregisterEncoder(webrtc::VideoEncoder* encoder) {
  if (encoder == nullptr) return;
  absl::MutexLock lock(&mu_);
  encoders_.erase(
      std::remove_if(encoders_.begin(), encoders_.end(),
                      [encoder](const EncoderEntry& e) {
                        return e.encoder == encoder;
                      }),
      encoders_.end());
  RTC_LOG(LS_INFO) << "BweAdapter: unregistered encoder count="
                   << encoders_.size();
}

void BweAdapter::OnBitrateUpdated(
    webrtc::DataRate target_bitrate,
    webrtc::DataRate stable_target_bitrate,
    webrtc::DataRate link_capacity,
    uint8_t fraction_loss,
    int64_t rtt_ms,
    double framerate_fps) {
  // Take a snapshot of the registry under the lock, then drop the
  // lock before calling SetRates on each encoder. SetRates can
  // acquire encoder-internal locks; holding our mu_ across the call
  // would create a lock-order edge from BweAdapter::mu_ to encoder
  // locks, which is a known deadlock pattern in libwebrtc encoder
  // factories.
  std::vector<EncoderEntry> snapshot;
  BweUpdate update;
  update.total_bitrate_bps = static_cast<int>(target_bitrate.bps());
  update.stable_bitrate_bps =
      static_cast<int>(stable_target_bitrate.bps());
  update.link_capacity_bps =
      link_capacity.IsFinite() ? static_cast<int>(link_capacity.bps()) : -1;
  update.fraction_loss = fraction_loss;
  update.rtt_ms = rtt_ms;
  update.framerate_fps = framerate_fps;
  {
    absl::MutexLock lock(&mu_);
    snapshot = encoders_;
    update.active_encoder_count = static_cast<int>(snapshot.size());
    last_update_ = update;
  }

  // Fire metrics first. If the sink throws, the encoders still get
  // their update — observability bugs should not stall the pipeline.
  if (sink_) {
    sink_(update);
  }

  // Build RateControlParameters once. Phase 2 single-layer: every
  // encoder gets the full target. Phase 2 stretch (see
  // bwe-adapter-design.md §4) splits this across simulcast layers
  // by layer_index.
  webrtc::VideoBitrateAllocation alloc;
  // Spatial layer 0, temporal layer 0 — single-layer correctness.
  alloc.SetBitrate(0, 0, static_cast<uint32_t>(target_bitrate.bps()));
  webrtc::VideoEncoder::RateControlParameters params(alloc, framerate_fps);
  params.bandwidth_allocation = target_bitrate;

  for (const auto& e : snapshot) {
    if (e.encoder != nullptr) {
      e.encoder->SetRates(params);
    }
  }
}

size_t BweAdapter::encoder_count() const {
  absl::MutexLock lock(&mu_);
  return encoders_.size();
}

BweUpdate BweAdapter::last_update() const {
  absl::MutexLock lock(&mu_);
  return last_update_;
}

// ---------------------------------------------------------------------
// WrapWithBweAdapter — decorator that registers / unregisters the
// inner encoder with the adapter, forwarding everything else.
// ---------------------------------------------------------------------

namespace {

class BweRegisteringEncoder : public webrtc::VideoEncoder {
 public:
  BweRegisteringEncoder(std::unique_ptr<webrtc::VideoEncoder> inner,
                         BweAdapter* adapter,
                         int layer_index)
      : inner_(std::move(inner)),
        adapter_(adapter),
        layer_index_(layer_index) {}

  ~BweRegisteringEncoder() override {
    // Defensive: if Release wasn't called (libwebrtc's contract says
    // it must be, but bugs happen), we still drop our adapter
    // registration so we don't leave a dangling pointer.
    if (registered_ && adapter_ != nullptr) {
      adapter_->UnregisterEncoder(inner_.get());
    }
  }

  int32_t InitEncode(
      const webrtc::VideoCodec* codec_settings,
      const webrtc::VideoEncoder::Settings& settings) override {
    int32_t r = inner_->InitEncode(codec_settings, settings);
    if (r == WEBRTC_VIDEO_CODEC_OK && adapter_ != nullptr && !registered_) {
      adapter_->RegisterEncoder(inner_.get(), layer_index_);
      registered_ = true;
    }
    return r;
  }

  int32_t Encode(
      const webrtc::VideoFrame& frame,
      const std::vector<webrtc::VideoFrameType>* frame_types) override {
    return inner_->Encode(frame, frame_types);
  }

  int32_t RegisterEncodeCompleteCallback(
      webrtc::EncodedImageCallback* callback) override {
    return inner_->RegisterEncodeCompleteCallback(callback);
  }

  int32_t Release() override {
    if (registered_ && adapter_ != nullptr) {
      adapter_->UnregisterEncoder(inner_.get());
      registered_ = false;
    }
    return inner_->Release();
  }

  void SetRates(const RateControlParameters& parameters) override {
    inner_->SetRates(parameters);
  }

  EncoderInfo GetEncoderInfo() const override {
    return inner_->GetEncoderInfo();
  }

 private:
  std::unique_ptr<webrtc::VideoEncoder> inner_;
  BweAdapter* const adapter_;
  const int layer_index_;
  bool registered_ = false;
};

}  // namespace

std::unique_ptr<webrtc::VideoEncoder> WrapWithBweAdapter(
    std::unique_ptr<webrtc::VideoEncoder> inner,
    BweAdapter* adapter,
    int layer_index) {
  if (adapter == nullptr) return inner;
  return std::make_unique<BweRegisteringEncoder>(std::move(inner), adapter,
                                                  layer_index);
}

}  // namespace cloud_browser

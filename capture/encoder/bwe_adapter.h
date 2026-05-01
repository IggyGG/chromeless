// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// BweAdapter — central plumbing between libwebrtc's bandwidth
// estimator and our per-codec encoders.
//
// libwebrtc already calls VideoEncoder::SetRates() on each active
// encoder when its BWE updates the target. This adapter is the
// **central observer + coordinator** sitting one layer up:
//
//   * It is the single place where every BWE update is observed,
//     metric-tagged, and (optionally) rewritten before reaching the
//     encoders. Without it, encoder-level SetRates calls are
//     observable only by adding logging to every per-codec wrapper.
//   * It is the natural home for cross-encoder logic that does not
//     belong inside any single encoder: layer mapping for simulcast
//     (Phase 2 stretch), framerate / resolution scaling on
//     congestion (ABR per PROJECT_BRIEF.md Phase 2), and the
//     "decide which simulcast layer gets which fraction" decision
//     once we ship simulcast.
//
// Cross-references:
//   * docs/internal/bwe-adapter-design.md (this task's design doc)
//   * capture/encoder/encoder_factory.h    (T19 — the seam this
//                                           adapter closes)
//   * capture/encoder/vp9_encoder.h        (T35)
//   * capture/encoder/h264_encoder.h       (T36)

#ifndef CAPTURE_ENCODER_BWE_ADAPTER_H_
#define CAPTURE_ENCODER_BWE_ADAPTER_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"
#include "api/units/data_rate.h"
#include "api/video/video_bitrate_allocation.h"
#include "api/video_codecs/video_encoder.h"
#include "base/memory/raw_ptr.h"

namespace cloud_browser {

// One BWE update, in the shape we surface to metrics. Pure data —
// the adapter constructs one of these on every OnBitrateUpdated and
// hands it to the optional MetricsSink.
struct BweUpdate {
  int total_bitrate_bps   = 0;   // sum across simulcast layers.
  int stable_bitrate_bps  = 0;   // BWE's "smoothed" estimate.
  int link_capacity_bps   = 0;   // -1 if BWE doesn't have one.
  uint8_t fraction_loss   = 0;   // 0..255 per RFC 3550.
  int64_t rtt_ms          = 0;
  double framerate_fps    = 0.0;
  int active_encoder_count = 0;  // how many encoders we forwarded to.
};

class BweAdapter {
 public:
  // MetricsSink runs on the same thread as OnBitrateUpdated (the
  // libwebrtc worker thread that fired the BWE callback). It must
  // not block.
  using MetricsSink = std::function<void(const BweUpdate&)>;

  BweAdapter() = default;
  explicit BweAdapter(MetricsSink sink) : sink_(std::move(sink)) {}

  BweAdapter(const BweAdapter&) = delete;
  BweAdapter& operator=(const BweAdapter&) = delete;

  // Encoders register themselves on InitEncode and unregister on
  // Release. The registry is thread-safe; it is normal for libwebrtc
  // to call OnBitrateUpdated concurrently with register/unregister
  // when a new sender ramps up.
  //
  // `layer_index` is the spatial layer index for simulcast / SVC.
  // Phase 2 single-layer => 0.
  void RegisterEncoder(webrtc::VideoEncoder* encoder, int layer_index = 0);
  void UnregisterEncoder(webrtc::VideoEncoder* encoder);

  // Called by libwebrtc (or by tests). The adapter:
  //   1. Logs / tags via MetricsSink.
  //   2. Builds a webrtc::VideoEncoder::RateControlParameters.
  //   3. Calls SetRates on each registered encoder.
  //
  // For Phase 2 single-layer we fan the same RateControlParameters
  // out to every encoder. Phase 2 stretch: rewrite per encoder based
  // on its layer_index. See bwe-adapter-design.md §4.
  void OnBitrateUpdated(
      webrtc::DataRate target_bitrate,
      webrtc::DataRate stable_target_bitrate,
      webrtc::DataRate link_capacity,
      uint8_t fraction_loss,
      int64_t rtt_ms,
      double framerate_fps);

  // Visible to tests; the registry size is the only useful invariant
  // outside the OnBitrateUpdated path.
  size_t encoder_count() const;

  // Snapshot of the last update for diagnostic exposure.
  BweUpdate last_update() const;

 private:
  struct EncoderEntry {
    raw_ptr<webrtc::VideoEncoder> encoder;
    int layer_index;
  };

  mutable absl::Mutex mu_;
  std::vector<EncoderEntry> encoders_ ABSL_GUARDED_BY(mu_);
  BweUpdate last_update_ ABSL_GUARDED_BY(mu_);
  MetricsSink sink_;  // const after construction.
};

// WrapWithBweAdapter returns a wrapped VideoEncoder that registers
// `inner` with `adapter` on InitEncode and unregisters on Release,
// forwarding every other call straight through. The factory uses
// this so the per-codec encoder wrappers (Vp9Encoder, H264Encoder)
// stay agnostic to the adapter — adapter coordination lives in
// exactly one place.
//
// Returns `inner` unchanged if `adapter` is nullptr.
std::unique_ptr<webrtc::VideoEncoder> WrapWithBweAdapter(
    std::unique_ptr<webrtc::VideoEncoder> inner,
    BweAdapter* adapter,
    int layer_index = 0);

}  // namespace cloud_browser

#endif  // CAPTURE_ENCODER_BWE_ADAPTER_H_

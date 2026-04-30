// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// x264 H.264 software encoder for CloudBrowserVideoEncoderFactory.
//
// This is the v1 H.264 encoder. It exists primarily so Safari clients
// (which prefer H.264 in SDP negotiation) get a working pipeline; on
// Chrome/Firefox we expect VP9 (T35) to be picked first.
//
// Tuning rationale: docs/internal/h264-tuning-rationale.md.
//
// Cross-references:
//   * capture/encoder/encoder_factory.h    (the seam)
//   * capture/encoder/vp9_encoder.h        (the VP9 sibling)
//   * docs/internal/encoder-factory-design.md

#ifndef CAPTURE_ENCODER_H264_ENCODER_H_
#define CAPTURE_ENCODER_H264_ENCODER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "api/video/video_frame.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"

// Forward declarations for the x264 C struct so this header doesn't
// drag <x264.h> into every translation unit. The struct lives in the
// .cc.
struct x264_t;
struct x264_picture_t;

namespace cloud_browser {

struct H264EncoderConfig {
  // Initial target bitrate. Re-tuned by libwebrtc's BWE via SetRates.
  int target_bitrate_bps = 4'000'000;

  // Target framerate. Re-tuned by SetRates.
  int framerate = 30;

  // Period over which intra-refresh sweeps the picture. With
  // `b_intra_refresh = 1`, x264 inserts intra-coded slices that march
  // across the frame; this controls how fast that march completes.
  // Default 60 → ~2 s at 30 fps. Same shape as Vp9EncoderConfig.
  int intra_refresh_period_frames = 60;

  // SDP profile-level-id we are claiming. Default `42e01f`
  // (Constrained Baseline 3.1). Affects the x264 profile we set; the
  // actual SDP fmtp is owned by the factory (encoder_factory_stub.cc).
  std::string profile_level_id = "42e01f";

  // x264 preset. Anything slower than "ultrafast" will not hit
  // realtime on commodity cloud CPUs at 1080p.
  std::string preset = "ultrafast";

  // x264 tune. "zerolatency" disables lookahead and B-frames at
  // initialization time; we still override individual fields after.
  std::string tune = "zerolatency";

  // Number of encoder threads. 0 = auto (max(1, num_cores - 1)).
  int num_threads = 0;

  // Diagnostic; tagged into EncoderInfo::implementation_name.
  bool low_latency_tag = true;
};

class H264Encoder : public webrtc::VideoEncoder {
 public:
  explicit H264Encoder(H264EncoderConfig config);
  ~H264Encoder() override;

  H264Encoder(const H264Encoder&) = delete;
  H264Encoder& operator=(const H264Encoder&) = delete;

  // webrtc::VideoEncoder:
  int32_t InitEncode(const webrtc::VideoCodec* codec_settings,
                     const webrtc::VideoEncoder::Settings& settings) override;

  int32_t Encode(
      const webrtc::VideoFrame& frame,
      const std::vector<webrtc::VideoFrameType>* frame_types) override;

  int32_t RegisterEncodeCompleteCallback(
      webrtc::EncodedImageCallback* callback) override;

  int32_t Release() override;

  void SetRates(const RateControlParameters& parameters) override;

  EncoderInfo GetEncoderInfo() const override;

 private:
  // Maps a profile-level-id like "42e01f" onto x264 profile and level
  // strings ("baseline" / "main" / "high" + "3.1" etc.). Returns
  // false on unsupported strings.
  bool ResolveProfileLevel(std::string* profile, int* level_idc) const;

  H264EncoderConfig config_;

  // x264 state. Heap-allocated so the C types don't bleed into the
  // header.
  x264_t* encoder_ = nullptr;
  std::unique_ptr<x264_picture_t> pic_in_;
  std::unique_ptr<x264_picture_t> pic_out_;

  webrtc::EncodedImageCallback* callback_ = nullptr;

  uint64_t frames_in_ = 0;
  int width_ = 0;
  int height_ = 0;
  bool initialized_ = false;
};

}  // namespace cloud_browser

#endif  // CAPTURE_ENCODER_H264_ENCODER_H_

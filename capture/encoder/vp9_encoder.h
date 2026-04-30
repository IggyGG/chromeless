// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// libvpx VP9 software encoder bound to CloudBrowserVideoEncoderFactory.
//
// This is the v1 video encoder for our cloud browser pipeline.  It
// implements webrtc::VideoEncoder and wraps libvpx's VP9 codec, tuned
// for **zero-latency** screen-shaped content (a Chromium tab).
//
// All encoder knob choices are documented in
// docs/internal/vp9-tuning-rationale.md; the short version is:
//   * realtime deadline (VPX_DL_REALTIME)
//   * one-pass CBR rate control
//   * no automatic keyframes — intra-refresh handles recovery
//   * cyclic-refresh AQ (VP9E_SET_AQ_MODE = 3) for intra-refresh
//   * VP9E_CONTENT_SCREEN tune for crisp text
//
// Cross-references:
//   * capture/encoder/encoder_factory.h    (the seam)
//   * docs/internal/encoder-factory-design.md
//   * docs/internal/vp9-tuning-rationale.md

#ifndef CAPTURE_ENCODER_VP9_ENCODER_H_
#define CAPTURE_ENCODER_VP9_ENCODER_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "api/video/encoded_image.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"

// Forward declarations for the libvpx C structs so this header doesn't
// drag the libvpx headers into every translation unit.
struct vpx_codec_ctx;
struct vpx_image;

namespace cloud_browser {

// Configuration for Vp9Encoder. Fields are runtime-modifiable via
// SetRates() (bitrate, framerate); the rest are fixed at InitEncode().
struct Vp9EncoderConfig {
  // Initial target bitrate. Re-tuned by libwebrtc's BWE via SetRates.
  int target_bitrate_bps = 4'000'000;

  // Target framerate. Re-tuned by SetRates.
  int framerate = 30;

  // How often to rotate the cyclic-refresh region, in frames. With
  // VP9E_SET_AQ_MODE = 3 the encoder refreshes a sliding band each
  // frame; this parameter sets the period over which the whole frame
  // is refreshed. 60 means a fully refreshed picture every 60 frames
  // (~2 s at 30 fps), which matches our intra-refresh budget.
  int intra_refresh_period_frames = 60;

  // Keyframe interval. Default to "infinite" — we rely on
  // intra-refresh + libwebrtc's IDR-on-demand to recover. -1 disables
  // automatic keyframes (kf_mode = VPX_KF_DISABLED). Positive values
  // are passed through as kf_max_dist.
  int keyframe_interval = -1;

  // libvpx encoding deadline. Always VPX_DL_REALTIME for our
  // workload; exposed for tests that want to crank quality up.
  uint64_t deadline = 1;  // VPX_DL_REALTIME == 1.

  // Number of encoder threads. 0 = auto (max(1, num_cores - 1)).
  int num_threads = 0;

  // Mostly diagnostic; tagged into EncoderInfo::implementation_name.
  bool low_latency_tag = true;
};

class Vp9Encoder : public webrtc::VideoEncoder {
 public:
  explicit Vp9Encoder(Vp9EncoderConfig config);
  ~Vp9Encoder() override;

  Vp9Encoder(const Vp9Encoder&) = delete;
  Vp9Encoder& operator=(const Vp9Encoder&) = delete;

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
  // Apply VP9-specific control codes (VP9E_*). Called once after
  // vpx_codec_enc_init succeeds and again on rate changes.
  bool ApplyVp9Controls();

  Vp9EncoderConfig config_;

  // libvpx state. Owned (heap-allocated) so this header doesn't pull
  // in the libvpx C headers.
  std::unique_ptr<vpx_codec_ctx> codec_;
  std::unique_ptr<vpx_image> raw_image_;

  // Output sink supplied by libwebrtc.
  webrtc::EncodedImageCallback* callback_ = nullptr;

  // Frame counter, used both for pts and for IDR-on-demand decisions.
  uint64_t frames_in_ = 0;

  // Cached size from InitEncode. Re-init on size changes.
  int width_ = 0;
  int height_ = 0;

  bool initialized_ = false;
};

}  // namespace cloud_browser

#endif  // CAPTURE_ENCODER_VP9_ENCODER_H_

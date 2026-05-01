// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// SVT-AV1 software AV1 encoder for CloudBrowserVideoEncoderFactory.
//
// Closes out the v1+ encoder set per T43:
//   * SW VP9 (T35), SW H.264 (T36) — Phase 1 today.
//   * HW NVENC (T63), HW VAAPI (T70) — Phase 4 hardware paths.
//   * SW AV1 here (T75) — the missing software AV1 path. libaom
//     realtime is too slow for our budget; SVT-AV1 (Intel/Netflix)
//     hits realtime at preset M8 with the screen-content tune
//     (T43 §3).
//
// The factory uses this when AV1 is the negotiated codec but no HW
// AV1 path is available (NVENC AV1 needs Ada Lovelace+; VAAPI AV1
// needs Intel Arc / AMD RDNA 3+). On older cloud GPUs and CPU-only
// hosts, this is the AV1 fast path.
//
// Tuning rationale: docs/internal/svtav1-tuning-rationale.md.
//
// Cross-references:
//   * capture/encoder/encoder_factory.h    (T19 — the seam)
//   * capture/encoder/vp9_encoder.h        (T35 — VP9 SW sibling)
//   * capture/encoder/h264_encoder.h       (T36 — H.264 SW sibling)
//   * capture/encoder/nvenc_encoder.h      (T63 — HW AV1 path)
//   * capture/encoder/vaapi_encoder.h      (T70 — HW AV1 path)
//   * docs/research/av1-encoders.md        (T43 — codec landscape)

#ifndef CAPTURE_ENCODER_SVTAV1_ENCODER_H_
#define CAPTURE_ENCODER_SVTAV1_ENCODER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "api/video/video_frame.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"
#include "base/memory/raw_ptr.h"

namespace cloud_browser {

struct SvtAv1EncoderConfig {
  // Initial target bitrate (bps). Re-tuned by SetRates().
  int target_bitrate_bps = 4'000'000;

  // Target framerate. Re-tuned by SetRates().
  int framerate = 30;

  // Period over which intra-refresh sweeps the picture (frames).
  // SVT-AV1 has built-in cyclic refresh under
  // `enable_tf` / segmentation; we set it via the open-GOP intra
  // period below. Default 60 = ~2 s at 30 fps, same shape as
  // VP9/H264.
  int intra_refresh_period_frames = 60;

  // AV1 profile. v1 only ships profile 0 (8-bit 4:2:0), matching
  // every other encoder. Profile 1 (4:4:4) and 2 (12-bit) are
  // Phase 4.5 stretch.
  int profile = 0;

  // SVT-AV1 enc preset (`enc_mode`). Lower = slower / better
  // quality; higher = faster / worse. Per T43, **M8 is the fastest
  // preset that hits realtime with acceptable quality** on
  // commodity cloud CPUs. M9-M13 cut quality in measurable ways
  // without buying us materially more headroom.
  //
  // We expose the integer (0..13) rather than the EB_ENC_PRESET_M8
  // enum to keep the SDK out of the header. Resolved at session
  // init.
  int preset_m = 8;

  // Tune: "VQ" (visual quality, default for screen content),
  // "PSNR", or "SSIM". Browser UI is text-heavy + flat regions —
  // the VQ tune handles those better than PSNR does in practice.
  std::string tune = "VQ";

  // Number of encoder threads. 0 = SVT-AV1 picks (max(1,
  // num_cores - 1) per the convention shared with our other SW
  // encoders).
  int num_threads = 0;

  // Diagnostic; tagged into EncoderInfo::implementation_name.
  bool low_latency_tag = true;

  // Declared out-of-line to satisfy chromium-style ("Complex
  // class/struct needs an explicit out-of-line constructor"). The
  // struct holds non-trivial members; pinning lifecycle bodies in
  // the .cc keeps them out of every TU that #includes this header.
  SvtAv1EncoderConfig();
  ~SvtAv1EncoderConfig();
  SvtAv1EncoderConfig(const SvtAv1EncoderConfig&);
  SvtAv1EncoderConfig& operator=(const SvtAv1EncoderConfig&);
  SvtAv1EncoderConfig(SvtAv1EncoderConfig&&);
  SvtAv1EncoderConfig& operator=(SvtAv1EncoderConfig&&);
};

class SvtAv1Encoder : public webrtc::VideoEncoder {
 public:
  explicit SvtAv1Encoder(SvtAv1EncoderConfig config);
  ~SvtAv1Encoder() override;

  SvtAv1Encoder(const SvtAv1Encoder&) = delete;
  SvtAv1Encoder& operator=(const SvtAv1Encoder&) = delete;

  // Static probe: returns true if libSvtAv1Enc is linked + loadable
  // on this host. Cheap (no encode session created). The factory
  // caches the result.  When `HAS_SVT_AV1` is undefined returns
  // false unconditionally.
  static bool ProbeAvailable();

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
  class Impl;  // PIMPL — defined in svtav1_encoder.cc, keeps
                // <EbSvtAv1Enc.h> out of every TU.

  SvtAv1EncoderConfig config_;
  std::unique_ptr<Impl> impl_;
  raw_ptr<webrtc::EncodedImageCallback> callback_ = nullptr;
  uint64_t frames_in_ = 0;
  int width_ = 0;
  int height_ = 0;
  bool initialized_ = false;
};

}  // namespace cloud_browser

#endif  // CAPTURE_ENCODER_SVTAV1_ENCODER_H_

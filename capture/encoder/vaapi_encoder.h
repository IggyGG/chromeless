// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// VAAPI hardware video encoder for CloudBrowserVideoEncoderFactory.
//
// VAAPI is the cross-vendor Linux HW encode API. T43 (docs/research/
// av1-encoders.md) maps codec → vendor coverage:
//
//   * Intel Arc / Xe-HPG (Alchemist / Battlemage / Meteor Lake /
//     Arrow Lake) — H.264, HEVC, AV1, VP9 via the iHD media-driver.
//   * AMD RDNA 3+ (RX 7000+) — H.264, HEVC, AV1 via Mesa's
//     radeonsi/RADV stack (AMF native is also an option but the
//     VAAPI shim is the easier landing).
//   * Older AMD VCN — H.264 / HEVC only via legacy radeon driver.
//
// This file is the libva wrapper. T63 covered NVIDIA NVENC; the two
// share the encoder factory seam but speak completely different
// SDKs.
//
// Tuning rationale: docs/internal/vaapi-tuning-rationale.md.
//
// Cross-references:
//   * capture/encoder/encoder_factory.h    (T19 — the seam)
//   * capture/encoder/nvenc_encoder.h      (T63 — NVIDIA sibling)
//   * capture/encoder/vp9_encoder.h        (T35 — SW sibling)
//   * capture/encoder/h264_encoder.h       (T36 — SW sibling)
//   * docs/research/av1-encoders.md        (T43 — codec landscape)

#ifndef CAPTURE_ENCODER_VAAPI_ENCODER_H_
#define CAPTURE_ENCODER_VAAPI_ENCODER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "api/video/video_frame.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"
#include "base/memory/raw_ptr.h"

namespace cloud_browser {

// Same shape as NvencEncoderConfig — keeps the factory's per-codec
// branching uniform across SW / NVENC / VAAPI.
struct VaapiEncoderConfig {
  // "H264", "HEVC", "AV1", "VP9". Determines the VAProfile +
  // VAEntrypoint we hand vaCreateConfig and the libwebrtc codec
  // type the encoder advertises.
  std::string codec_type = "H264";

  // Initial target bitrate (bps). Re-tuned by SetRates().
  int target_bitrate_bps = 4'000'000;

  // Target framerate. Re-tuned by SetRates().
  int framerate = 30;

  // Intra-refresh sweep period (frames). VAAPI exposes intra-
  // refresh on H.264 / HEVC via VAEncMiscParameterTypeRIR — 60 is
  // ~2 s at 30 fps. AV1 + VP9 use cyclic-refresh AQ instead, same
  // shape as Vp9Encoder.
  int intra_refresh_period_frames = 60;

  // SDP profile-level-id for H.264 / HEVC. Same parser as
  // H264Encoder (T36). Ignored for AV1 / VP9.
  std::string profile_level_id = "42e01f";

  // VAAPI rate-control mode. The header avoids pulling <va/va.h>;
  // implementation translates the string at session init.
  //   "CBR"   — VA_RC_CBR (default; matches our pacer)
  //   "VBR"   — VA_RC_VBR
  //   "CQP"   — VA_RC_CQP (offline; do not use here)
  std::string rate_control = "CBR";

  // GOP size in frames. -1 = infinite (rely on intra-refresh +
  // libwebrtc IDR-on-demand). Positive values pass through as the
  // VAAPI sequence parameter `intra_period`. Same intent as
  // NvencEncoderConfig::keyframe_interval.
  int gop_size = -1;

  // Optional preferred VA driver name override. Empty = let libva's
  // own LIBVA_DRIVER_NAME / autodetect decide. "iHD" picks Intel's
  // modern media-driver; "i965" the legacy one; "radeonsi" the AMD
  // Mesa stack. Operators set this when running on a host with
  // multiple VA drivers installed.
  std::string driver_override;

  // Diagnostic; tagged into EncoderInfo::implementation_name. The
  // implementation discovers vendor at runtime (vaQueryVendorString)
  // and records it in the EncoderInfo so metrics can break out HW
  // path by vendor.
  bool low_latency_tag = true;
};

class VaapiEncoder : public webrtc::VideoEncoder {
 public:
  explicit VaapiEncoder(VaapiEncoderConfig config);
  ~VaapiEncoder() override;

  VaapiEncoder(const VaapiEncoder&) = delete;
  VaapiEncoder& operator=(const VaapiEncoder&) = delete;

  // Factory-level probe: returns true if a VAAPI session can be
  // opened on this host for `codec_type`. Cheap (under ~5 ms once
  // the VA display is initialised); the factory caches the
  // result. False on a host without /dev/dri/renderD128, libva, or
  // a driver providing the encode entrypoint for `codec_type`.
  static bool ProbeAvailable(const std::string& codec_type);

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
  class Impl;  // PIMPL, defined in vaapi_encoder.cc — keeps libva
                // headers out of every TU.

  VaapiEncoderConfig config_;
  std::unique_ptr<Impl> impl_;
  raw_ptr<webrtc::EncodedImageCallback> callback_ = nullptr;
  uint64_t frames_in_ = 0;
  int width_ = 0;
  int height_ = 0;
  bool initialized_ = false;
  std::string vendor_string_;  // populated post-Initialize.
};

}  // namespace cloud_browser

#endif  // CAPTURE_ENCODER_VAAPI_ENCODER_H_

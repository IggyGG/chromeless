// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// NVENC hardware video encoder for CloudBrowserVideoEncoderFactory.
//
// Phase 4: hardware encode lives behind the same VideoEncoderFactory
// seam as the Phase 1 software encoders (T35 VP9, T36 H.264). This
// file is the NVIDIA Video Codec SDK wrapper. Codec-agnostic
// (H264/HEVC/AV1) — see codec_type in NvencEncoderConfig.
//
// Per T43 (docs/research/av1-encoders.md):
//   * NVENC H.264 / HEVC: any GPU with NVENC support (T4 / L4 / L40 /
//     A10 / A100 / H100, etc.).
//   * NVENC AV1: requires Ada Lovelace+ (L4, L40, H100 — NOT T4 / A10
//     / A100).
//   * The factory's runtime probe (see encoder_factory_stub.cc)
//     decides codec-by-codec which is available.
//
// Tuning rationale: docs/internal/nvenc-tuning-rationale.md.
//
// Cross-references:
//   * capture/encoder/encoder_factory.h    (T19 — the seam)
//   * capture/encoder/vp9_encoder.h        (T35 — SW sibling)
//   * capture/encoder/h264_encoder.h       (T36 — SW sibling)
//   * docs/research/av1-encoders.md        (T43 — codec landscape)

#ifndef CAPTURE_ENCODER_NVENC_ENCODER_H_
#define CAPTURE_ENCODER_NVENC_ENCODER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "api/video/video_frame.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"
#include "base/memory/raw_ptr.h"

namespace cloud_browser {

// Codec-and-vendor-agnostic shape so adding NVENC presets / new
// codecs is a knob change rather than a rewrite. Mirrors the shape
// of Vp9EncoderConfig / H264EncoderConfig.
struct NvencEncoderConfig {
  // "H264", "HEVC", or "AV1". Determines which NV_ENC_CODEC_*_GUID
  // we hand the SDK at session init and which RTP packetizer
  // libwebrtc picks downstream.
  std::string codec_type = "H264";

  // Initial target bitrate. SetRates() retunes via
  // nvEncReconfigureEncoder.
  int target_bitrate_bps = 4'000'000;

  // Target framerate. Re-tuned by SetRates.
  int framerate = 30;

  // Intra-refresh sweep period (frames). Mirrors the same knob in
  // Vp9EncoderConfig / H264EncoderConfig — picture is fully
  // refreshed every `intra_refresh_period_frames` without inserting
  // a full IDR.
  int intra_refresh_period_frames = 60;

  // SDP profile-level-id we are claiming (codec-specific
  // interpretation):
  //   * H264: "42e01f" (Constrained Baseline 3.1) etc., same as
  //     H264Encoder.
  //   * HEVC: ignored in v1 (Main profile by default).
  //   * AV1: ignored in v1 (Main profile by default).
  std::string profile_level_id = "42e01f";

  // NVENC preset GUID (passed through as a string so we don't pull
  // <nvEncodeAPI.h> into this header). Defaults map to:
  //   * "P3" — modern preset (NV_ENC_PRESET_P3_GUID), balanced
  //     low-latency on Ada Lovelace+ hardware.
  //   * "LL_HQ" — older preset (NV_ENC_PRESET_LOW_LATENCY_HQ_GUID),
  //     used as a fallback on pre-Ada hardware.
  // The NVENC implementation in nvenc_encoder.cc resolves the
  // string to a GUID at session init.
  std::string preset = "P3";

  // NVENC tuning info (low-latency presets unlock this knob).
  //   * "LOW_LATENCY"        — sane default for our budget.
  //   * "ULTRA_LOW_LATENCY"  — strict in-order; minimal queuing.
  //   * "HIGH_QUALITY"       — DON'T pick; for offline encode.
  //   * "LOSSLESS"           — DON'T pick.
  std::string tuning_info = "LOW_LATENCY";

  // Diagnostic; tagged into EncoderInfo::implementation_name.
  bool low_latency_tag = true;

  // Declared out-of-line to satisfy chromium-style ("Complex
  // class/struct needs an explicit out-of-line constructor"). The
  // struct holds non-trivial members; pinning lifecycle bodies in
  // the .cc keeps them out of every TU that #includes this header.
  NvencEncoderConfig();
  ~NvencEncoderConfig();
  NvencEncoderConfig(const NvencEncoderConfig&);
  NvencEncoderConfig& operator=(const NvencEncoderConfig&);
  NvencEncoderConfig(NvencEncoderConfig&&);
  NvencEncoderConfig& operator=(NvencEncoderConfig&&);
};

class NvencEncoder : public webrtc::VideoEncoder {
 public:
  explicit NvencEncoder(NvencEncoderConfig config);
  ~NvencEncoder() override;

  NvencEncoder(const NvencEncoder&) = delete;
  NvencEncoder& operator=(const NvencEncoder&) = delete;

  // Factory-level probe: returns true if at least one NVENC session
  // can be opened on this host for `codec_type` ("H264"/"HEVC"/"AV1").
  // Used by encoder_factory_stub.cc to decide HW-vs-SW at runtime.
  // Cheap (under ~5 ms on a warm system); the factory caches the
  // result.
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
  // Opaque PIMPL — keeps NVIDIA Video Codec SDK headers
  // (<nvEncodeAPI.h>, <cuda.h>) out of this header. Definition
  // lives in nvenc_encoder.cc.
  class Impl;

  NvencEncoderConfig config_;
  std::unique_ptr<Impl> impl_;
  raw_ptr<webrtc::EncodedImageCallback> callback_ = nullptr;
  uint64_t frames_in_ = 0;
  int width_ = 0;
  int height_ = 0;
  bool initialized_ = false;
};

}  // namespace cloud_browser

#endif  // CAPTURE_ENCODER_NVENC_ENCODER_H_

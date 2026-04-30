// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Stub implementation of CloudBrowserVideoEncoderFactory.
//
// This is SCAFFOLDING — it exists to lock the contract documented in
// encoder_factory.h. Real encoder bodies (libvpx VP9 wrapper, x264
// wrapper, NVENC, VAAPI) plug in here in their own follow-up tasks.
//
// Until those tasks land, CreateVideoEncoder() returns a placeholder
// that asserts at runtime. GetSupportedFormats() and QueryCodecSupport()
// are real and exercise the SDP-layer plumbing without instantiating
// any encoder.
//
// Cross-references:
//   - capture/encoder/encoder_factory.h
//   - docs/internal/encoder-factory-design.md

#include "capture/encoder/encoder_factory.h"

#include <cassert>
#include <utility>

#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"

namespace cloud_browser {
namespace {

// Tiny placeholder VideoEncoder that satisfies the type but refuses to
// run. It exists so callers that try to instantiate an encoder before
// the real wrappers land fail loudly rather than silently dropping
// frames.
class UnimplementedEncoder : public webrtc::VideoEncoder {
 public:
  explicit UnimplementedEncoder(std::string codec_name)
      : codec_name_(std::move(codec_name)) {}

  int32_t InitEncode(const webrtc::VideoCodec* /*codec_settings*/,
                     const webrtc::VideoEncoder::Settings& /*settings*/) override {
    assert(false && "encoder not implemented yet; see encoder_factory_stub.cc");
    return -1;
  }

  int32_t Encode(
      const webrtc::VideoFrame& /*frame*/,
      const std::vector<webrtc::VideoFrameType>* /*frame_types*/) override {
    assert(false && "encoder not implemented yet");
    return -1;
  }

  int32_t RegisterEncodeCompleteCallback(
      webrtc::EncodedImageCallback* /*callback*/) override {
    return -1;
  }

  int32_t Release() override { return 0; }

  void SetRates(const RateControlParameters& /*parameters*/) override {}

  EncoderInfo GetEncoderInfo() const override {
    EncoderInfo info;
    info.implementation_name = "cloud-browser-stub-" + codec_name_;
    info.is_hardware_accelerated = false;
    info.supports_native_handle = false;
    return info;
  }

 private:
  const std::string codec_name_;
};

}  // namespace

CloudBrowserVideoEncoderFactory::CloudBrowserVideoEncoderFactory(Config config)
    : config_(std::move(config)) {}

CloudBrowserVideoEncoderFactory::~CloudBrowserVideoEncoderFactory() = default;

std::vector<webrtc::SdpVideoFormat>
CloudBrowserVideoEncoderFactory::GetSupportedFormats() const {
  // Order is preference order. Phase 1 prefers VP9 (better quality at
  // the same bitrate, libvpx is well-trodden in libwebrtc), with H.264
  // as the universal-compat fallback. VP8 only if explicitly enabled.
  std::vector<webrtc::SdpVideoFormat> formats;
  if (config_.enable_vp9) {
    formats.emplace_back(webrtc::SdpVideoFormat("VP9"));
  }
  if (config_.enable_h264) {
    // Constrained Baseline 3.1 — broadest browser compatibility.
    webrtc::SdpVideoFormat h264("H264");
    h264.parameters["level-asymmetry-allowed"] = "1";
    h264.parameters["packetization-mode"] = "1";
    h264.parameters["profile-level-id"] = "42e01f";
    formats.push_back(std::move(h264));
  }
  if (config_.enable_vp8) {
    formats.emplace_back(webrtc::SdpVideoFormat("VP8"));
  }
  return formats;
}

std::unique_ptr<webrtc::VideoEncoder>
CloudBrowserVideoEncoderFactory::CreateVideoEncoder(
    const webrtc::SdpVideoFormat& format) {
  // Real impls land in follow-up tasks. Until then, hand back a
  // placeholder that the runtime will trip over loudly. nullptr is
  // reserved for "we never claimed to support this format".
  if (format.name == "VP9" && config_.enable_vp9) {
    return std::make_unique<UnimplementedEncoder>("vp9");
  }
  if (format.name == "H264" && config_.enable_h264) {
    return std::make_unique<UnimplementedEncoder>("h264");
  }
  if (format.name == "VP8" && config_.enable_vp8) {
    return std::make_unique<UnimplementedEncoder>("vp8");
  }
  return nullptr;
}

webrtc::VideoEncoderFactory::CodecSupport
CloudBrowserVideoEncoderFactory::QueryCodecSupport(
    const webrtc::SdpVideoFormat& format,
    absl::optional<std::string> /*scalability_mode*/) const {
  webrtc::VideoEncoderFactory::CodecSupport support;
  support.is_supported = false;
  support.is_power_efficient = false;  // SW today; Phase 4 flips this
                                       // for HW-backed formats.
  for (const auto& f : GetSupportedFormats()) {
    if (f.name == format.name) {
      support.is_supported = true;
      break;
    }
  }
  return support;
}

}  // namespace cloud_browser

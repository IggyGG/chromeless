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

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "capture/encoder/bwe_adapter.h"
#include "capture/encoder/h264_encoder.h"
#include "capture/encoder/nvenc_encoder.h"
#include "capture/encoder/simulcast_factory.h"
#include "capture/encoder/svtav1_encoder.h"
#include "capture/encoder/vaapi_encoder.h"
#include "capture/encoder/vp9_encoder.h"

namespace cloud_browser {
namespace {

// Per-codec NVENC availability is probed once at process startup
// (the probe opens + immediately closes a tiny session, see
// NvencEncoder::ProbeAvailable). The cache is process-wide because
// the underlying GPU/driver capability does not change at runtime.
//
// nullopt = "not yet probed"; bool = probe outcome.
struct NvencCache {
  absl::optional<bool> h264;
  absl::optional<bool> hevc;
  absl::optional<bool> av1;
};

NvencCache& MutableNvencCache() {
  static NvencCache cache;
  return cache;
}

bool NvencAvailable(const std::string& codec) {
  auto& cache = MutableNvencCache();
  absl::optional<bool>* slot =
      (codec == "H264")  ? &cache.h264 :
      (codec == "HEVC")  ? &cache.hevc :
      (codec == "AV1")   ? &cache.av1  : nullptr;
  if (!slot) return false;
  if (!slot->has_value()) {
    *slot = NvencEncoder::ProbeAvailable(codec);
  }
  return slot->value();
}

// VAAPI parallel of NvencCache. Same shape; one slot per codec.
struct VaapiCache {
  absl::optional<bool> h264;
  absl::optional<bool> hevc;
  absl::optional<bool> av1;
  absl::optional<bool> vp9;
};

VaapiCache& MutableVaapiCache() {
  static VaapiCache cache;
  return cache;
}

bool VaapiAvailable(const std::string& codec) {
  auto& cache = MutableVaapiCache();
  absl::optional<bool>* slot =
      (codec == "H264")  ? &cache.h264 :
      (codec == "HEVC")  ? &cache.hevc :
      (codec == "AV1")   ? &cache.av1  :
      (codec == "VP9")   ? &cache.vp9  : nullptr;
  if (!slot) return false;
  if (!slot->has_value()) {
    *slot = VaapiEncoder::ProbeAvailable(codec);
  }
  return slot->value();
}


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
  // AV1 is advertised when at least one path can produce it: NVENC
  // (Ada Lovelace+), VAAPI (Intel Arc / AMD RDNA 3+), or SVT-AV1
  // SW. The runtime probe in CreateVideoEncoder picks among them.
  // We advertise unconditionally when SVT-AV1 is enabled — it's our
  // CPU-only fallback — and rely on probe-fail-then-software to
  // route correctly per-host. See capture/encoder/README.md for the
  // resolution chain.
  const bool av1_advertise =
      config_.enable_svt_av1 ||
      config_.prefer_nvenc_av1 ||
      config_.prefer_vaapi_av1;
  if (av1_advertise) {
    formats.emplace_back(webrtc::SdpVideoFormat("AV1"));
  }
  return formats;
}

std::unique_ptr<webrtc::VideoEncoder>
CloudBrowserVideoEncoderFactory::Create(
    const webrtc::Environment& /*env*/,
    const webrtc::SdpVideoFormat& format) {
  // VP9 is the v1 default — real implementation lives in
  // capture/encoder/vp9_encoder.{h,cc} (T35). The factory propagates
  // the latency-tuning bits from its own Config into Vp9EncoderConfig
  // so the encoder doesn't have to re-derive intent.
  if (format.name == "VP9" && config_.enable_vp9) {
    // VAAPI VP9 is Intel-only (Mesa AMD drops VP9 — see vaapi-tuning-
    // rationale.md). NVENC has no VP9 path. So the resolution order
    // for VP9 is just VAAPI → SW.
    auto build_inner_vp9 = [factory_cfg = config_](
                                  const SimulcastLayer& /*layer*/)
        -> std::unique_ptr<webrtc::VideoEncoder> {
      // Inner encoder selection mirrors the non-simulcast path
      // below — the simulcast wrapper only adds the per-layer
      // dispatch + downscale.
      if (factory_cfg.prefer_vaapi_vp9 && VaapiEncoder::ProbeAvailable("VP9")) {
        VaapiEncoderConfig cfg;
        cfg.codec_type = "VP9";
        cfg.intra_refresh_period_frames = factory_cfg.intra_refresh
            ? std::max(1, factory_cfg.gop_length_frames / 4)
            : 60;
        cfg.gop_size = (factory_cfg.intra_refresh
                            ? -1 : factory_cfg.gop_length_frames);
        cfg.low_latency_tag = factory_cfg.zero_latency;
        return std::make_unique<VaapiEncoder>(cfg);
      }
      Vp9EncoderConfig cfg;
      cfg.intra_refresh_period_frames = factory_cfg.intra_refresh
          ? std::max(1, factory_cfg.gop_length_frames / 4)
          : 60;
      cfg.keyframe_interval =
          (factory_cfg.intra_refresh ? -1 : factory_cfg.gop_length_frames);
      cfg.low_latency_tag = factory_cfg.zero_latency;
      return std::make_unique<Vp9Encoder>(cfg);
    };
    if (config_.enable_simulcast) {
      return WrapWithBweAdapter(
          std::make_unique<SimulcastEncoder>(build_inner_vp9, "vp9"),
          config_.bwe_adapter);
    }
    return WrapWithBweAdapter(build_inner_vp9(SimulcastLayer{}),
                               config_.bwe_adapter);
  }
  // H264 wrapper lives in capture/encoder/h264_encoder.{h,cc} (T36).
  // We pull profile-level-id straight from the SDP fmtp parameters
  // the remote agreed to; the SDP layer normalizes "profile-level-id"
  // to lowercase before we see it.
  if (format.name == "H264" && config_.enable_h264) {
    // Resolution order: NVENC → VAAPI → SW. NVENC wins when both HW
    // prefers are set (a NVIDIA + Intel/AMD coexistent host is rare;
    // fall through happens when the active probe fails). Probe
    // failure on every HW path silently lands on the x264 SW wrapper
    // — every host always has a working H.264 path.
    auto it = format.parameters.find("profile-level-id");
    std::string profile_level_id;
    if (it != format.parameters.end() && it->second.size() == 6) {
      profile_level_id = it->second;
    }
    auto build_inner_h264 = [factory_cfg = config_, profile_level_id](
                                    const SimulcastLayer& /*layer*/)
        -> std::unique_ptr<webrtc::VideoEncoder> {
      if (factory_cfg.prefer_nvenc_h264 && NvencEncoder::ProbeAvailable("H264")) {
        NvencEncoderConfig cfg;
        cfg.codec_type = "H264";
        cfg.intra_refresh_period_frames = factory_cfg.intra_refresh
            ? std::max(1, factory_cfg.gop_length_frames / 4)
            : 60;
        if (!profile_level_id.empty()) cfg.profile_level_id = profile_level_id;
        cfg.low_latency_tag = factory_cfg.zero_latency;
        return std::make_unique<NvencEncoder>(cfg);
      }
      if (factory_cfg.prefer_vaapi_h264 && VaapiEncoder::ProbeAvailable("H264")) {
        VaapiEncoderConfig cfg;
        cfg.codec_type = "H264";
        cfg.intra_refresh_period_frames = factory_cfg.intra_refresh
            ? std::max(1, factory_cfg.gop_length_frames / 4)
            : 60;
        if (!profile_level_id.empty()) cfg.profile_level_id = profile_level_id;
        cfg.gop_size = (factory_cfg.intra_refresh
                            ? -1 : factory_cfg.gop_length_frames);
        cfg.low_latency_tag = factory_cfg.zero_latency;
        return std::make_unique<VaapiEncoder>(cfg);
      }
      H264EncoderConfig cfg;
      cfg.intra_refresh_period_frames = factory_cfg.intra_refresh
          ? std::max(1, factory_cfg.gop_length_frames / 4)
          : 60;
      if (!profile_level_id.empty()) cfg.profile_level_id = profile_level_id;
      cfg.low_latency_tag = factory_cfg.zero_latency;
      return std::make_unique<H264Encoder>(cfg);
    };
    if (config_.enable_simulcast) {
      return WrapWithBweAdapter(
          std::make_unique<SimulcastEncoder>(build_inner_h264, "h264"),
          config_.bwe_adapter);
    }
    return WrapWithBweAdapter(build_inner_h264(SimulcastLayer{}),
                               config_.bwe_adapter);
  }
  // AV1 routing (T75): NVENC AV1 (Ada+) → VAAPI AV1 (Arc / RDNA3+)
  // → SVT-AV1 SW. Per docs/research/av1-encoders.md (T43) the AV1
  // hardware coverage is sparse on cloud GPUs (T4/A10 have no AV1
  // encoder), so SVT-AV1 SW is the fallback that always exists when
  // libSvtAv1Enc is linked. If we get to AV1 with no SW link AND
  // every HW probe missing, we return nullptr and libwebrtc falls
  // back to the next negotiated codec.
  if (format.name == "AV1") {
    if (config_.prefer_nvenc_av1 && NvencAvailable("AV1")) {
      NvencEncoderConfig cfg;
      cfg.codec_type = "AV1";
      cfg.intra_refresh_period_frames = config_.intra_refresh
          ? std::max(1, config_.gop_length_frames / 4)
          : 60;
      cfg.low_latency_tag = config_.zero_latency;
      return WrapWithBweAdapter(std::make_unique<NvencEncoder>(cfg),
                                 config_.bwe_adapter);
    }
    if (config_.prefer_vaapi_av1 && VaapiAvailable("AV1")) {
      VaapiEncoderConfig cfg;
      cfg.codec_type = "AV1";
      cfg.intra_refresh_period_frames = config_.intra_refresh
          ? std::max(1, config_.gop_length_frames / 4)
          : 60;
      cfg.gop_size = (config_.intra_refresh ? -1 : config_.gop_length_frames);
      cfg.low_latency_tag = config_.zero_latency;
      return WrapWithBweAdapter(std::make_unique<VaapiEncoder>(cfg),
                                 config_.bwe_adapter);
    }
    if (config_.enable_svt_av1 && SvtAv1Encoder::ProbeAvailable()) {
      SvtAv1EncoderConfig cfg;
      cfg.intra_refresh_period_frames = config_.intra_refresh
          ? std::max(1, config_.gop_length_frames / 4)
          : 60;
      cfg.low_latency_tag = config_.zero_latency;
      return WrapWithBweAdapter(std::make_unique<SvtAv1Encoder>(cfg),
                                 config_.bwe_adapter);
    }
    // No working AV1 path. SDP advertised it because the config
    // claimed support, but the runtime probe came up empty —
    // return nullptr per the factory contract; libwebrtc will
    // pick the next negotiated codec (typically VP9 or H.264).
    return nullptr;
  }
  // VP8 wrapper still placeholder until its own task lands.
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

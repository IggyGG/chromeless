// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// NVENC hardware encoder — see nvenc_encoder.h.
//
// TODO(T17-build-env): validate compile + link once the from-source
// Chromium / libwebrtc build env is up.
// TODO(NVIDIA-Video-Codec-SDK): add the SDK headers (<nvEncodeAPI.h>,
// <cuda.h>) and libnvidia-encode.so + libcuda.so to the build (see
// capture/build-integration/BUILD.gn — a `:nvenc_sdk` config gates
// HAS_NVENC). Until both gates clear, this file is design-by-spec.
//
// Tuning rationale: docs/internal/nvenc-tuning-rationale.md.

#include "capture/encoder/nvenc_encoder.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "api/video/i420_buffer.h"
#include "modules/video_coding/codecs/h264/include/h264_globals.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"

#if defined(HAS_NVENC)
extern "C" {
#include <cuda.h>
#include <nvEncodeAPI.h>
}
#endif  // HAS_NVENC

namespace cloud_browser {

// Out-of-line lifecycle for NvencEncoderConfig (chromium-style).
NvencEncoderConfig::NvencEncoderConfig() = default;
NvencEncoderConfig::~NvencEncoderConfig() = default;
NvencEncoderConfig::NvencEncoderConfig(const NvencEncoderConfig&) = default;
NvencEncoderConfig& NvencEncoderConfig::operator=(const NvencEncoderConfig&) = default;
NvencEncoderConfig::NvencEncoderConfig(NvencEncoderConfig&&) = default;
NvencEncoderConfig& NvencEncoderConfig::operator=(NvencEncoderConfig&&) = default;
namespace {

// Resolve our string codec_type to (a) the NV_ENC_CODEC_*_GUID and
// (b) the libwebrtc kVideoCodec* enum. Returns false for unknown
// codecs.
//
// Defined as a free function (not a switch in the Impl) so the
// factory's ProbeAvailable can reuse it without instantiating the
// Impl.
struct CodecMapping {
  webrtc::VideoCodecType webrtc_type;
#if defined(HAS_NVENC)
  GUID guid;
#endif
};

bool ResolveCodecType(const std::string& codec_type,
                      CodecMapping* out) {
  if (codec_type == "H264") {
    out->webrtc_type = webrtc::kVideoCodecH264;
#if defined(HAS_NVENC)
    out->guid = NV_ENC_CODEC_H264_GUID;
#endif
    return true;
  }
  if (codec_type == "HEVC") {
    out->webrtc_type = webrtc::kVideoCodecH265;
#if defined(HAS_NVENC)
    out->guid = NV_ENC_CODEC_HEVC_GUID;
#endif
    return true;
  }
  if (codec_type == "AV1") {
    out->webrtc_type = webrtc::kVideoCodecAV1;
#if defined(HAS_NVENC)
    out->guid = NV_ENC_CODEC_AV1_GUID;
#endif
    return true;
  }
  return false;
}

#if defined(HAS_NVENC)
// Resolve our preset string to an NVENC preset GUID. Per T43, the
// "P*" presets are the modern (Ada Lovelace) shape and the "LL_*"
// presets are the legacy fallback.
bool ResolvePresetGuid(const std::string& preset, GUID* out) {
  if (preset == "P1") { *out = NV_ENC_PRESET_P1_GUID; return true; }
  if (preset == "P2") { *out = NV_ENC_PRESET_P2_GUID; return true; }
  if (preset == "P3") { *out = NV_ENC_PRESET_P3_GUID; return true; }
  if (preset == "P4") { *out = NV_ENC_PRESET_P4_GUID; return true; }
  if (preset == "P5") { *out = NV_ENC_PRESET_P5_GUID; return true; }
  if (preset == "P6") { *out = NV_ENC_PRESET_P6_GUID; return true; }
  if (preset == "P7") { *out = NV_ENC_PRESET_P7_GUID; return true; }
  // The legacy `LOW_LATENCY_HQ / _HP / _DEFAULT` preset GUIDs were
  // deprecated in NVIDIA Video Codec SDK 11.0 (2021) and removed
  // outright in SDK 12.0 (2022) — they no longer exist as symbols
  // in <nvEncodeAPI.h> against any modern header (verified against
  // /usr/include/ffnvcodec/nvEncodeAPI.h on bookworm). Referencing
  // them is now a compile error, not a runtime fallback. The
  // modern P1..P7 presets cover every NVENC-capable generation
  // back to Pascal, so the "LL_*" legacy branches no longer have
  // a hardware basis. Callers that pass "LL_HQ" / "LL_HP" /
  // "LL_DEFAULT" will hit the false return below; the factory
  // already treats that as "preset not supported -> fall back to
  // SW", so behaviour stays safe — the strings just stop
  // resolving.
  // TODO(nvenc-presets): once we run on a real NVENC card, decide
  // whether to silently remap the LL_* strings to a P-preset
  // (P5 for LL_HQ, P3 for LL_HP, P4 for LL_DEFAULT) so legacy
  // configs keep working, or to surface a clearer error to the
  // factory probe. For Phase 4 prep, the safe-fallback shape is
  // sufficient.
  return false;
}

NV_ENC_TUNING_INFO ResolveTuning(const std::string& tuning) {
  if (tuning == "ULTRA_LOW_LATENCY") return NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
  if (tuning == "HIGH_QUALITY")      return NV_ENC_TUNING_INFO_HIGH_QUALITY;
  if (tuning == "LOSSLESS")          return NV_ENC_TUNING_INFO_LOSSLESS;
  return NV_ENC_TUNING_INFO_LOW_LATENCY;  // default + safe.
}
#endif  // HAS_NVENC

}  // namespace

// ---------------------------------------------------------------------
// NvencEncoder::Impl — opaque PIMPL holding the NVENC session.
//
// All NVIDIA SDK types live in here so the public header stays SDK-
// independent.
// ---------------------------------------------------------------------
class NvencEncoder::Impl {
 public:
#if defined(HAS_NVENC)
  // Open the encode session, initialize it for `codec` at
  // `width`x`height` with `cfg`.  Returns false on failure (caller
  // should report WEBRTC_VIDEO_CODEC_ERROR).
  bool Initialize(const NvencEncoderConfig& cfg, int width, int height,
                  webrtc::VideoCodecType webrtc_type);

  // Encode one I420 frame; on success, populates `out` with the
  // produced bitstream (Annex-B for H.264 / HEVC; OBU stream for
  // AV1) and `is_keyframe`.
  bool Encode(const uint8_t* y_plane, int y_stride,
              const uint8_t* u_plane, int u_stride,
              const uint8_t* v_plane, int v_stride,
              int width, int height,
              uint64_t pts_ticks,
              bool force_idr,
              std::vector<uint8_t>* out,
              bool* is_keyframe);

  bool Reconfigure(const NvencEncoderConfig& cfg);
  void Destroy();

 private:
  void* encoder_ = nullptr;             // void* NvEnc handle.
  CUcontext cu_context_ = nullptr;       // owned cuCtx; freed on Destroy.
  NV_ENCODE_API_FUNCTION_LIST fn_{};     // SDK function table.
  NV_ENC_INITIALIZE_PARAMS init_params_{};
  NV_ENC_CONFIG enc_config_{};
  std::vector<NV_ENC_OUTPUT_PTR> output_buffers_;
  std::vector<NV_ENC_INPUT_PTR>  input_buffers_;
  size_t buffer_index_ = 0;
  webrtc::VideoCodecType webrtc_type_ = webrtc::kVideoCodecGeneric;
#else
  // Stubs so the file compiles when HAS_NVENC is undefined. Every
  // method asserts at runtime — the factory's ProbeAvailable() must
  // return false in this configuration so we never get instantiated.
  bool Initialize(const NvencEncoderConfig&, int, int,
                  webrtc::VideoCodecType) {
    return false;
  }
  bool Encode(const uint8_t*, int, const uint8_t*, int,
              const uint8_t*, int, int, int, uint64_t, bool,
              std::vector<uint8_t>*, bool*) {
    return false;
  }
  bool Reconfigure(const NvencEncoderConfig&) { return false; }
  void Destroy() {}
#endif  // HAS_NVENC
};

#if defined(HAS_NVENC)

bool NvencEncoder::Impl::Initialize(const NvencEncoderConfig& cfg,
                                      int width, int height,
                                      webrtc::VideoCodecType webrtc_type) {
  webrtc_type_ = webrtc_type;

  // 1. CUDA context. NVENC operates in the context of a CUDA device;
  //    we pin to GPU 0 (Phase 4 follow-up: device-selection policy).
  if (cuInit(0) != CUDA_SUCCESS) return false;
  CUdevice dev;
  if (cuDeviceGet(&dev, 0) != CUDA_SUCCESS) return false;
  if (cuCtxCreate(&cu_context_, 0, dev) != CUDA_SUCCESS) return false;

  // 2. SDK function table.
  fn_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
  if (NvEncodeAPICreateInstance(&fn_) != NV_ENC_SUCCESS) return false;

  // 3. Open session.
  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
  open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
  open.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
  open.device = cu_context_;
  open.apiVersion = NVENCAPI_VERSION;
  if (fn_.nvEncOpenEncodeSessionEx(&open, &encoder_) != NV_ENC_SUCCESS) {
    return false;
  }

  // 4. Initialize for the chosen codec + preset + tuning.
  CodecMapping cm;
  if (!ResolveCodecType(cfg.codec_type, &cm)) return false;
  GUID preset_guid;
  if (!ResolvePresetGuid(cfg.preset, &preset_guid)) return false;
  NV_ENC_TUNING_INFO tuning = ResolveTuning(cfg.tuning_info);

  // Pull preset config (gives us a sane NV_ENC_CONFIG to override).
  NV_ENC_PRESET_CONFIG preset_cfg{};
  preset_cfg.version = NV_ENC_PRESET_CONFIG_VER;
  preset_cfg.presetCfg.version = NV_ENC_CONFIG_VER;
  if (fn_.nvEncGetEncodePresetConfigEx(encoder_, cm.guid, preset_guid,
                                         tuning, &preset_cfg) != NV_ENC_SUCCESS) {
    return false;
  }
  enc_config_ = preset_cfg.presetCfg;

  // 5. Latency-critical overrides (see nvenc-tuning-rationale.md):
  enc_config_.gopLength = NVENC_INFINITE_GOPLENGTH;  // no auto IDR.
  enc_config_.frameIntervalP = 1;                     // no B-frames.
  enc_config_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
  enc_config_.rcParams.averageBitRate = static_cast<uint32_t>(cfg.target_bitrate_bps);
  enc_config_.rcParams.maxBitRate     = enc_config_.rcParams.averageBitRate;
  enc_config_.rcParams.vbvBufferSize  = enc_config_.rcParams.averageBitRate;  // ~1s.
  enc_config_.rcParams.vbvInitialDelay = enc_config_.rcParams.vbvBufferSize;
  enc_config_.rcParams.enableMinQP = 0;
  enc_config_.rcParams.enableMaxQP = 0;

  // Codec-specific config: enable intra-refresh, set entropy / SPS-PPS
  // shape per T43 §2.
  if (cm.guid == NV_ENC_CODEC_H264_GUID) {
    auto& h264 = enc_config_.encodeCodecConfig.h264Config;
    h264.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    h264.repeatSPSPPS = 1;          // SPS/PPS in front of every IDR.
    h264.enableIntraRefresh = 1;
    h264.intraRefreshPeriod = static_cast<uint32_t>(cfg.intra_refresh_period_frames);
    h264.intraRefreshCnt   = std::max(1u,
        static_cast<uint32_t>(cfg.intra_refresh_period_frames) / 4u);
    h264.outputFramePackingSEI = 0;
    h264.outputBufferingPeriodSEI = 0;
    h264.outputPictureTimingSEI   = 0;
  } else if (cm.guid == NV_ENC_CODEC_HEVC_GUID) {
    auto& hevc = enc_config_.encodeCodecConfig.hevcConfig;
    hevc.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    hevc.repeatSPSPPS = 1;
    hevc.enableIntraRefresh = 1;
    hevc.intraRefreshPeriod = static_cast<uint32_t>(cfg.intra_refresh_period_frames);
    hevc.intraRefreshCnt   = std::max(1u,
        static_cast<uint32_t>(cfg.intra_refresh_period_frames) / 4u);
  } else if (cm.guid == NV_ENC_CODEC_AV1_GUID) {
    auto& av1 = enc_config_.encodeCodecConfig.av1Config;
    av1.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    av1.repeatSeqHdr = 1;
    av1.enableIntraRefresh = 1;
    av1.intraRefreshPeriod = static_cast<uint32_t>(cfg.intra_refresh_period_frames);
    av1.intraRefreshCnt   = std::max(1u,
        static_cast<uint32_t>(cfg.intra_refresh_period_frames) / 4u);
  }

  init_params_.version = NV_ENC_INITIALIZE_PARAMS_VER;
  init_params_.encodeGUID = cm.guid;
  init_params_.presetGUID = preset_guid;
  init_params_.tuningInfo = tuning;
  init_params_.encodeWidth  = static_cast<uint32_t>(width);
  init_params_.encodeHeight = static_cast<uint32_t>(height);
  init_params_.darWidth   = init_params_.encodeWidth;
  init_params_.darHeight  = init_params_.encodeHeight;
  init_params_.frameRateNum = static_cast<uint32_t>(std::max(1, cfg.framerate));
  init_params_.frameRateDen = 1;
  init_params_.enablePTD  = 1;
  init_params_.encodeConfig = &enc_config_;

  if (fn_.nvEncInitializeEncoder(encoder_, &init_params_) != NV_ENC_SUCCESS) {
    return false;
  }

  // 6. Allocate I/O buffer pool — Phase-4 default 4 deep; can be
  //    grown if the consumer is slow draining.
  constexpr int kPoolDepth = 4;
  for (int i = 0; i < kPoolDepth; ++i) {
    NV_ENC_CREATE_INPUT_BUFFER in{};
    in.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
    in.width = init_params_.encodeWidth;
    in.height = init_params_.encodeHeight;
    in.bufferFmt = NV_ENC_BUFFER_FORMAT_IYUV;  // I420.
    if (fn_.nvEncCreateInputBuffer(encoder_, &in) != NV_ENC_SUCCESS) {
      return false;
    }
    input_buffers_.push_back(in.inputBuffer);

    NV_ENC_CREATE_BITSTREAM_BUFFER bs{};
    bs.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    if (fn_.nvEncCreateBitstreamBuffer(encoder_, &bs) != NV_ENC_SUCCESS) {
      return false;
    }
    output_buffers_.push_back(bs.bitstreamBuffer);
  }

  return true;
}

bool NvencEncoder::Impl::Encode(
    const uint8_t* y_plane, int y_stride,
    const uint8_t* u_plane, int u_stride,
    const uint8_t* v_plane, int v_stride,
    int width, int height,
    uint64_t pts_ticks,
    bool force_idr,
    std::vector<uint8_t>* out,
    bool* is_keyframe) {
  if (!encoder_) return false;
  const size_t i = (buffer_index_++) % input_buffers_.size();

  // Lock + memcpy I420 into the NVENC input buffer.
  NV_ENC_LOCK_INPUT_BUFFER lock{};
  lock.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
  lock.inputBuffer = input_buffers_[i];
  if (fn_.nvEncLockInputBuffer(encoder_, &lock) != NV_ENC_SUCCESS) return false;
  uint8_t* dst = static_cast<uint8_t*>(lock.bufferDataPtr);
  const uint32_t pitch = lock.pitch;
  for (int row = 0; row < height; ++row) {
    std::memcpy(dst + row * pitch, y_plane + row * y_stride, width);
  }
  uint8_t* dst_u = dst + pitch * height;
  uint8_t* dst_v = dst_u + (pitch / 2) * (height / 2);
  for (int row = 0; row < height / 2; ++row) {
    std::memcpy(dst_u + row * (pitch / 2), u_plane + row * u_stride,
                width / 2);
    std::memcpy(dst_v + row * (pitch / 2), v_plane + row * v_stride,
                width / 2);
  }
  fn_.nvEncUnlockInputBuffer(encoder_, input_buffers_[i]);

  NV_ENC_PIC_PARAMS pic{};
  pic.version = NV_ENC_PIC_PARAMS_VER;
  pic.inputBuffer = input_buffers_[i];
  pic.outputBitstream = output_buffers_[i];
  pic.bufferFmt = NV_ENC_BUFFER_FORMAT_IYUV;
  pic.inputWidth = width;
  pic.inputHeight = height;
  pic.inputPitch = pitch;
  pic.frameIdx = static_cast<uint32_t>(pts_ticks);
  pic.inputTimeStamp = pts_ticks;
  pic.pictureType = force_idr ? NV_ENC_PIC_TYPE_IDR : NV_ENC_PIC_TYPE_P;
  pic.encodePicFlags = force_idr ? NV_ENC_PIC_FLAG_FORCEIDR : 0;

  NVENCSTATUS status = fn_.nvEncEncodePicture(encoder_, &pic);
  if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT) {
    return false;
  }
  if (status == NV_ENC_ERR_NEED_MORE_INPUT) {
    out->clear();
    return true;  // not an error — encoder needs more input.
  }

  // Drain bitstream.
  NV_ENC_LOCK_BITSTREAM bs{};
  bs.version = NV_ENC_LOCK_BITSTREAM_VER;
  bs.outputBitstream = output_buffers_[i];
  if (fn_.nvEncLockBitstream(encoder_, &bs) != NV_ENC_SUCCESS) return false;
  out->assign(static_cast<const uint8_t*>(bs.bitstreamBufferPtr),
              static_cast<const uint8_t*>(bs.bitstreamBufferPtr) +
                  bs.bitstreamSizeInBytes);
  *is_keyframe = (bs.pictureType == NV_ENC_PIC_TYPE_IDR ||
                  bs.pictureType == NV_ENC_PIC_TYPE_I);
  fn_.nvEncUnlockBitstream(encoder_, output_buffers_[i]);
  return true;
}

bool NvencEncoder::Impl::Reconfigure(const NvencEncoderConfig& cfg) {
  if (!encoder_) return false;
  enc_config_.rcParams.averageBitRate =
      static_cast<uint32_t>(cfg.target_bitrate_bps);
  enc_config_.rcParams.maxBitRate     = enc_config_.rcParams.averageBitRate;
  enc_config_.rcParams.vbvBufferSize  = enc_config_.rcParams.averageBitRate;
  enc_config_.rcParams.vbvInitialDelay = enc_config_.rcParams.vbvBufferSize;
  init_params_.frameRateNum = static_cast<uint32_t>(std::max(1, cfg.framerate));
  init_params_.frameRateDen = 1;

  NV_ENC_RECONFIGURE_PARAMS rp{};
  rp.version = NV_ENC_RECONFIGURE_PARAMS_VER;
  rp.reInitEncodeParams = init_params_;
  rp.resetEncoder = 0;
  rp.forceIDR = 0;
  return fn_.nvEncReconfigureEncoder(encoder_, &rp) == NV_ENC_SUCCESS;
}

void NvencEncoder::Impl::Destroy() {
  if (!encoder_) return;
  for (auto* p : input_buffers_) fn_.nvEncDestroyInputBuffer(encoder_, p);
  for (auto* p : output_buffers_) fn_.nvEncDestroyBitstreamBuffer(encoder_, p);
  input_buffers_.clear();
  output_buffers_.clear();
  fn_.nvEncDestroyEncoder(encoder_);
  encoder_ = nullptr;
  if (cu_context_) {
    cuCtxDestroy(cu_context_);
    cu_context_ = nullptr;
  }
}

#endif  // HAS_NVENC

// ---------------------------------------------------------------------
// NvencEncoder
// ---------------------------------------------------------------------

NvencEncoder::NvencEncoder(NvencEncoderConfig config)
    : config_(std::move(config)), impl_(std::make_unique<Impl>()) {}

NvencEncoder::~NvencEncoder() { Release(); }

bool NvencEncoder::ProbeAvailable(const std::string& codec_type) {
#if defined(HAS_NVENC)
  CodecMapping cm;
  if (!ResolveCodecType(codec_type, &cm)) return false;
  // Cheap probe: open + immediately destroy a session for `cm.guid`.
  // This trips early if the driver / GPU can't encode this codec
  // (e.g., AV1 on a T4 — see T43 cloud-GPU footnote).
  Impl probe;
  // NvencEncoderConfig has user-declared (out-of-line) ctors / dtor /
  // copy / move (see nvenc_encoder.h) — that makes the type
  // non-aggregate, so designated initialisers (`{.codec_type = ...}`)
  // do NOT compile. Build the probe config by assignment instead.
  // Same shape we use elsewhere when probing configs (compare the
  // VAAPI fix in commit fa2d201 — identical class of bug).
  NvencEncoderConfig probe_cfg;
  probe_cfg.codec_type = codec_type;
  bool ok = probe.Initialize(std::move(probe_cfg),
                              160, 120,
                              cm.webrtc_type);
  probe.Destroy();
  return ok;
#else
  (void)codec_type;
  return false;
#endif
}

int32_t NvencEncoder::InitEncode(
    const webrtc::VideoCodec* codec_settings,
    const webrtc::VideoEncoder::Settings& /*settings*/) {
  if (!codec_settings || codec_settings->width == 0 ||
      codec_settings->height == 0) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  width_ = codec_settings->width;
  height_ = codec_settings->height;

  CodecMapping cm;
  if (!ResolveCodecType(config_.codec_type, &cm)) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (!impl_->Initialize(config_, width_, height_, cm.webrtc_type)) {
    RTC_LOG(LS_ERROR) << "NvencEncoder::Initialize failed for "
                      << config_.codec_type;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  initialized_ = true;
  frames_in_ = 0;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvencEncoder::Encode(
    const webrtc::VideoFrame& frame,
    const std::vector<webrtc::VideoFrameType>* frame_types) {
  if (!initialized_ || !callback_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  if (frame.width() != width_ || frame.height() != height_) {
    // Release() nulls callback_ (see its body). The entry guard above has
    // already been passed for THIS frame, so nothing re-checks it before
    // the OnEncodedImage call below — carrying the registration across the
    // re-init by hand is what stops that from being a nullptr dereference.
    // Symptom if you remove it: browser-process SIGSEGV on the first frame
    // at a new geometry, i.e. the guest dies the moment the user resizes.
    // Latent until the capturer's resolution became mutable (it pinned
    // min==max at 1280x720, so no frame ever changed size mid-session).
    webrtc::EncodedImageCallback* const saved_callback = callback_;
    Release();
    webrtc::VideoCodec settings{};
    settings.width = frame.width();
    settings.height = frame.height();
    if (InitEncode(&settings, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200))
          != WEBRTC_VIDEO_CODEC_OK) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    callback_ = saved_callback;
  }

  webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 =
      frame.video_frame_buffer()->ToI420();
  if (!i420) return WEBRTC_VIDEO_CODEC_ERROR;

  bool force_idr = false;
  if (frame_types) {
    for (const auto& t : *frame_types) {
      if (t == webrtc::VideoFrameType::kVideoFrameKey) {
        force_idr = true;
        break;
      }
    }
  }

  std::vector<uint8_t> out;
  bool is_keyframe = false;
  if (!impl_->Encode(i420->DataY(), i420->StrideY(),
                     i420->DataU(), i420->StrideU(),
                     i420->DataV(), i420->StrideV(),
                     width_, height_,
                     frames_in_++,
                     force_idr,
                     &out, &is_keyframe)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  if (out.empty()) {
    return WEBRTC_VIDEO_CODEC_OK;  // need-more-input — not an error.
  }

  webrtc::EncodedImage encoded_image;
  encoded_image.SetEncodedData(
      webrtc::EncodedImageBuffer::Create(out.data(), out.size()));
  encoded_image._frameType = is_keyframe
      ? webrtc::VideoFrameType::kVideoFrameKey
      : webrtc::VideoFrameType::kVideoFrameDelta;
  encoded_image._encodedWidth = width_;
  encoded_image._encodedHeight = height_;
  encoded_image.SetRtpTimestamp(frame.rtp_timestamp());
  encoded_image.capture_time_ms_ = frame.render_time_ms();
  encoded_image.rotation_ = frame.rotation();

  webrtc::CodecSpecificInfo csi{};
  CodecMapping cm;
  ResolveCodecType(config_.codec_type, &cm);
  csi.codecType = cm.webrtc_type;
  if (cm.webrtc_type == webrtc::kVideoCodecH264) {
    csi.codecSpecific.H264.packetization_mode =
        webrtc::H264PacketizationMode::NonInterleaved;
  }

  auto result = callback_->OnEncodedImage(encoded_image, &csi);
  return (result.error == webrtc::EncodedImageCallback::Result::OK)
      ? WEBRTC_VIDEO_CODEC_OK
      : WEBRTC_VIDEO_CODEC_ERROR;
}

int32_t NvencEncoder::RegisterEncodeCompleteCallback(
    webrtc::EncodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvencEncoder::Release() {
  if (impl_) impl_->Destroy();
  initialized_ = false;
  callback_ = nullptr;
  return WEBRTC_VIDEO_CODEC_OK;
}

void NvencEncoder::SetRates(const RateControlParameters& parameters) {
  if (!initialized_) return;
  config_.target_bitrate_bps =
      static_cast<int>(parameters.bitrate.get_sum_bps());
  config_.framerate =
      std::max(1, static_cast<int>(parameters.framerate_fps));
  if (!impl_->Reconfigure(config_)) {
    RTC_LOG(LS_WARNING) << "NvencEncoder::Reconfigure failed";
  }
}

webrtc::VideoEncoder::EncoderInfo NvencEncoder::GetEncoderInfo() const {
  EncoderInfo info;
  info.implementation_name = std::string("cloud-browser-nvenc-")
      + config_.codec_type
      + (config_.low_latency_tag ? "-lowlatency" : "");
  info.is_hardware_accelerated = true;
  info.supports_native_handle = false;
  info.supports_simulcast = false;
  info.has_trusted_rate_controller = true;  // CBR + no dropframe.
  return info;
}

}  // namespace cloud_browser

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// SVT-AV1 software encoder — see svtav1_encoder.h.
//
// TODO(T17-build-env): validate compile + link once the from-source
// Chromium / libwebrtc build env is up.
// TODO(SVT-AV1-lib): add libSvtAv1Enc headers (<EbSvtAv1Enc.h>,
// <EbSvtAv1Formats.h>, <EbSvtAv1ErrorCodes.h>) and libSvtAv1Enc.so to
// the build (see capture/build-integration/BUILD.gn — a `:svt_av1`
// config gates HAS_SVT_AV1). Until both gates clear, this file is
// design-by-spec.

#include "capture/encoder/svtav1_encoder.h"

#include <algorithm>
#include <cstring>
#include <thread>
#include <utility>

#include "api/video/i420_buffer.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"

#if defined(HAS_SVT_AV1)
extern "C" {
#include <EbSvtAv1Enc.h>
}
#endif

namespace cloud_browser {

// Out-of-line lifecycle for SvtAv1EncoderConfig (chromium-style).
SvtAv1EncoderConfig::SvtAv1EncoderConfig() = default;
SvtAv1EncoderConfig::~SvtAv1EncoderConfig() = default;
SvtAv1EncoderConfig::SvtAv1EncoderConfig(const SvtAv1EncoderConfig&) = default;
SvtAv1EncoderConfig& SvtAv1EncoderConfig::operator=(const SvtAv1EncoderConfig&) = default;
SvtAv1EncoderConfig::SvtAv1EncoderConfig(SvtAv1EncoderConfig&&) = default;
SvtAv1EncoderConfig& SvtAv1EncoderConfig::operator=(SvtAv1EncoderConfig&&) = default;
namespace {

#if defined(HAS_SVT_AV1)
uint32_t ResolveTune(const std::string& tune) {
  if (tune == "PSNR") return 1;
  if (tune == "SSIM") return 2;
  return 0;  // VQ (default).
}
#endif

}  // namespace

// ---------------------------------------------------------------------
// SvtAv1Encoder::Impl — opaque PIMPL holding the SVT-AV1 handle.
// ---------------------------------------------------------------------
class SvtAv1Encoder::Impl {
 public:
#if defined(HAS_SVT_AV1)
  bool Initialize(const SvtAv1EncoderConfig& cfg, int width, int height);

  bool Encode(const uint8_t* y, int y_stride,
              const uint8_t* u, int u_stride,
              const uint8_t* v, int v_stride,
              int width, int height,
              uint64_t pts,
              bool force_idr,
              std::vector<uint8_t>* out,
              bool* is_keyframe);

  bool Reconfigure(const SvtAv1EncoderConfig& cfg);
  void Destroy();

 private:
  EbComponentType*           handle_ = nullptr;
  EbSvtAv1EncConfiguration   svt_cfg_{};
  bool                        running_ = false;
#else
  bool Initialize(const SvtAv1EncoderConfig&, int, int) { return false; }
  bool Encode(const uint8_t*, int, const uint8_t*, int,
              const uint8_t*, int, int, int, uint64_t, bool,
              std::vector<uint8_t>*, bool*) { return false; }
  bool Reconfigure(const SvtAv1EncoderConfig&) { return false; }
  void Destroy() {}
#endif
};

#if defined(HAS_SVT_AV1)

bool SvtAv1Encoder::Impl::Initialize(const SvtAv1EncoderConfig& cfg,
                                      int width, int height) {
  // 1. Allocate the encoder handle and populate the configuration
  //    with SVT-AV1's defaults — we override only the latency-
  //    critical bits.
  if (svt_av1_enc_init_handle(&handle_, nullptr, &svt_cfg_) != EB_ErrorNone) {
    return false;
  }

  // 2. Geometry + framerate.
  svt_cfg_.source_width  = static_cast<uint32_t>(width);
  svt_cfg_.source_height = static_cast<uint32_t>(height);
  svt_cfg_.frame_rate_numerator   = static_cast<uint32_t>(std::max(1, cfg.framerate));
  svt_cfg_.frame_rate_denominator = 1;

  // 3. Latency-critical knobs (see svtav1-tuning-rationale.md).
  svt_cfg_.enc_mode = static_cast<int8_t>(std::clamp(cfg.preset_m, 0, 13));
  svt_cfg_.encoder_bit_depth = 8;
  svt_cfg_.encoder_color_format = EB_YUV420;
  // `EbSvtAv1EncConfiguration::profile` is an unscoped enum
  // (`EbAv1SeqProfile` — MAIN_PROFILE / HIGH_PROFILE / PROFESSIONAL_PROFILE).
  // C++ does not implicitly convert int (or uint8_t) to an unscoped
  // enum, so the original `static_cast<uint8_t>(...)` triggered
  // "cannot initialize EbAv1SeqProfile from uint8_t". Cast straight
  // to the enum type. Validation of the input range (0..2) lives in
  // SvtAv1EncoderConfig (header doc); out-of-range values would be
  // rejected by svt_av1_enc_set_parameter at init time.
  svt_cfg_.profile = static_cast<EbAv1SeqProfile>(cfg.profile);

  // No B-frames, no lookahead, no scene-change detection — same
  // rationale as VP9 / H.264 SW siblings.
  svt_cfg_.hierarchical_levels   = 0;
  svt_cfg_.pred_structure        = 0;   // low-delay-P.
  svt_cfg_.look_ahead_distance   = 0;
  svt_cfg_.enable_tpl_la         = 0;   // temporal-pyramid lookahead off.
  svt_cfg_.scene_change_detection = 0;

  // CBR rate control. SVT-AV1 mode 2 = CBR.
  svt_cfg_.rate_control_mode = 2;
  svt_cfg_.target_bit_rate   = static_cast<uint32_t>(cfg.target_bitrate_bps);
  svt_cfg_.max_bit_rate      = svt_cfg_.target_bit_rate;
  svt_cfg_.maximum_buffer_size_ms = 1000;  // 1s VBV.
  svt_cfg_.starting_buffer_level_ms = 1000;
  svt_cfg_.optimal_buffer_level_ms = 600;
  svt_cfg_.max_qp_allowed = 63;
  svt_cfg_.min_qp_allowed = 1;

  // Open-GOP intra period; relies on our IDR-on-demand path. -1 in
  // SVT-AV1 means "infinite" / never insert another key frame
  // automatically.
  svt_cfg_.intra_period_length = -1;
  // Per <EbSvtAv1Enc.h>: 1 = SVT_AV1_FWDKF_REFRESH (CRA, open GOP),
  // 2 = SVT_AV1_KF_REFRESH (IDR, closed GOP). The earlier draft
  // wrote a bare `2` with the comment inverted ("open-GOP key");
  // (a) C++ rejects implicit int -> unscoped-enum conversion, so
  // assignment of a literal int to the SvtAv1IntraRefreshType field
  // didn't compile, and (b) the rationale paragraph above asks for
  // an open-GOP intra cadence ("relies on our IDR-on-demand path")
  // which is value 1, not 2. Use the named constant.
  svt_cfg_.intra_refresh_type  = SVT_AV1_FWDKF_REFRESH;

  // Browser content is text + UI + occasional video. Screen-content
  // mode is the right pick — it beats the default tune by 10–19%
  // BD-rate per T43.
  svt_cfg_.screen_content_mode = 1;
  svt_cfg_.tune = static_cast<uint8_t>(ResolveTune(cfg.tune));

  // Threading: leave a core for capture / signaling / supervisord.
  // 0 -> SVT-AV1 default (which picks all cores); we override.
  if (cfg.num_threads > 0) {
    svt_cfg_.logical_processors = static_cast<uint32_t>(cfg.num_threads);
  } else {
    unsigned hc = std::thread::hardware_concurrency();
    svt_cfg_.logical_processors =
        std::max(1u, hc > 1 ? hc - 1 : hc);
  }
  svt_cfg_.tile_rows = 0;     // single tile row — keeps decoder side simple.
  svt_cfg_.tile_columns = 0;

  // 4. Push config + start.
  if (svt_av1_enc_set_parameter(handle_, &svt_cfg_) != EB_ErrorNone) return false;
  if (svt_av1_enc_init(handle_) != EB_ErrorNone) return false;

  running_ = true;
  return true;
}

bool SvtAv1Encoder::Impl::Encode(
    const uint8_t* y, int y_stride,
    const uint8_t* u, int u_stride,
    const uint8_t* v, int v_stride,
    int width, int height,
    uint64_t pts,
    bool force_idr,
    std::vector<uint8_t>* out,
    bool* is_keyframe) {
  if (!running_) return false;

  // Build the input EbBufferHeaderType pointing at the I420 planes.
  // SVT-AV1 reads from the pointers we hand it — we don't memcpy.
  EbSvtIOFormat input{};
  // TODO(svt-av1-runtime): EbSvtIOFormat::color_fmt defaults to
  // EB_YUV400 (= 0) under `{}` zero-init, but our encoder is
  // configured for EB_YUV420 (see Initialize). The library may
  // ignore the per-buffer color_fmt when the encoder-level
  // encoder_color_format is set, but we should set it explicitly
  // (input.color_fmt = EB_YUV420; input.bit_depth = EB_EIGHT_BIT;)
  // once we exercise the real encode path (Track F3 runtime).
  input.luma = const_cast<uint8_t*>(y);
  input.cb   = const_cast<uint8_t*>(u);
  input.cr   = const_cast<uint8_t*>(v);
  input.y_stride = static_cast<uint32_t>(y_stride);
  input.cb_stride = static_cast<uint32_t>(u_stride);
  input.cr_stride = static_cast<uint32_t>(v_stride);
  input.width  = static_cast<uint32_t>(width);
  input.height = static_cast<uint32_t>(height);

  EbBufferHeaderType header{};
  header.size      = sizeof(header);
  header.p_buffer  = reinterpret_cast<uint8_t*>(&input);
  header.n_filled_len = sizeof(input);
  header.pts       = static_cast<int64_t>(pts);
  header.pic_type  = force_idr ? EB_AV1_KEY_PICTURE
                                : EB_AV1_INVALID_PICTURE;  // let SVT pick.
  header.flags     = 0;

  if (svt_av1_enc_send_picture(handle_, &header) != EB_ErrorNone) {
    return false;
  }

  // Drain a packet (SVT-AV1's get_packet is synchronous in
  // low-latency mode). pic_send_done = 0 — we'll send more.
  out->clear();
  *is_keyframe = false;
  EbBufferHeaderType* outbuf = nullptr;
  EbErrorType s = svt_av1_enc_get_packet(handle_, &outbuf, /*pic_send_done=*/0);
  if (s == EB_NoErrorEmptyQueue) {
    // No output yet — not an error. Retry next frame.
    return true;
  }
  if (s != EB_ErrorNone || outbuf == nullptr) return false;

  out->assign(outbuf->p_buffer, outbuf->p_buffer + outbuf->n_filled_len);
  *is_keyframe = (outbuf->pic_type == EB_AV1_KEY_PICTURE);
  svt_av1_enc_release_out_buffer(&outbuf);
  return true;
}

bool SvtAv1Encoder::Impl::Reconfigure(const SvtAv1EncoderConfig& cfg) {
  if (!running_) return false;
  svt_cfg_.target_bit_rate = static_cast<uint32_t>(cfg.target_bitrate_bps);
  svt_cfg_.max_bit_rate    = svt_cfg_.target_bit_rate;
  svt_cfg_.frame_rate_numerator =
      static_cast<uint32_t>(std::max(1, cfg.framerate));
  // svt_av1_enc_set_parameter is documented as live-reconfigurable
  // for the bitrate and framerate fields.
  return svt_av1_enc_set_parameter(handle_, &svt_cfg_) == EB_ErrorNone;
}

void SvtAv1Encoder::Impl::Destroy() {
  if (!handle_) return;
  if (running_) {
    // Flush: pic_send_done=1 + sentinel header tells the encoder
    // to drain. We swallow remaining packets — the contract
    // expects libwebrtc's Release to terminate the pipeline.
    EbBufferHeaderType eos{};
    eos.size = sizeof(eos);
    eos.flags = EB_BUFFERFLAG_EOS;
    svt_av1_enc_send_picture(handle_, &eos);
    EbBufferHeaderType* drain = nullptr;
    while (svt_av1_enc_get_packet(handle_, &drain, /*pic_send_done=*/1)
              == EB_ErrorNone && drain != nullptr) {
      svt_av1_enc_release_out_buffer(&drain);
    }
    svt_av1_enc_deinit(handle_);
    running_ = false;
  }
  svt_av1_enc_deinit_handle(handle_);
  handle_ = nullptr;
}

#endif  // HAS_SVT_AV1

// ---------------------------------------------------------------------
// SvtAv1Encoder
// ---------------------------------------------------------------------

SvtAv1Encoder::SvtAv1Encoder(SvtAv1EncoderConfig config)
    : config_(std::move(config)), impl_(std::make_unique<Impl>()) {}

SvtAv1Encoder::~SvtAv1Encoder() { Release(); }

bool SvtAv1Encoder::ProbeAvailable() {
#if defined(HAS_SVT_AV1)
  // Cheapest possible probe — allocate a handle, free it. If the
  // library isn't linkable the call won't resolve and we return
  // false. svt_av1_enc_init_handle is a constant-time alloc.
  EbComponentType* h = nullptr;
  EbSvtAv1EncConfiguration cfg{};
  if (svt_av1_enc_init_handle(&h, nullptr, &cfg) != EB_ErrorNone) {
    return false;
  }
  svt_av1_enc_deinit_handle(h);
  return true;
#else
  return false;
#endif
}

int32_t SvtAv1Encoder::InitEncode(
    const webrtc::VideoCodec* codec_settings,
    const webrtc::VideoEncoder::Settings& /*settings*/) {
  if (!codec_settings || codec_settings->width == 0 ||
      codec_settings->height == 0) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  width_ = codec_settings->width;
  height_ = codec_settings->height;
  if (!impl_->Initialize(config_, width_, height_)) {
    RTC_LOG(LS_ERROR) << "SvtAv1Encoder::Initialize failed";
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  initialized_ = true;
  frames_in_ = 0;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t SvtAv1Encoder::Encode(
    const webrtc::VideoFrame& frame,
    const std::vector<webrtc::VideoFrameType>* frame_types) {
  if (!initialized_ || !callback_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  if (frame.width() != width_ || frame.height() != height_) {
    Release();
    webrtc::VideoCodec settings{};
    settings.width = frame.width();
    settings.height = frame.height();
    if (InitEncode(&settings, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200))
          != WEBRTC_VIDEO_CODEC_OK) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
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
                     width_, height_, frames_in_++, force_idr,
                     &out, &is_keyframe)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  if (out.empty()) return WEBRTC_VIDEO_CODEC_OK;

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
  csi.codecType = webrtc::kVideoCodecAV1;

  auto result = callback_->OnEncodedImage(encoded_image, &csi);
  return (result.error == webrtc::EncodedImageCallback::Result::OK)
      ? WEBRTC_VIDEO_CODEC_OK
      : WEBRTC_VIDEO_CODEC_ERROR;
}

int32_t SvtAv1Encoder::RegisterEncodeCompleteCallback(
    webrtc::EncodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t SvtAv1Encoder::Release() {
  if (impl_) impl_->Destroy();
  initialized_ = false;
  callback_ = nullptr;
  return WEBRTC_VIDEO_CODEC_OK;
}

void SvtAv1Encoder::SetRates(const RateControlParameters& parameters) {
  if (!initialized_) return;
  config_.target_bitrate_bps =
      static_cast<int>(parameters.bitrate.get_sum_bps());
  config_.framerate =
      std::max(1, static_cast<int>(parameters.framerate_fps));
  if (!impl_->Reconfigure(config_)) {
    RTC_LOG(LS_WARNING) << "SvtAv1Encoder::Reconfigure failed";
  }
}

webrtc::VideoEncoder::EncoderInfo SvtAv1Encoder::GetEncoderInfo() const {
  EncoderInfo info;
  info.implementation_name = config_.low_latency_tag
      ? "cloud-browser-av1-svtav1-lowlatency"
      : "cloud-browser-av1-svtav1";
  info.is_hardware_accelerated = false;
  info.supports_native_handle = false;
  info.supports_simulcast = false;
  info.has_trusted_rate_controller = true;  // CBR.
  return info;
}

}  // namespace cloud_browser

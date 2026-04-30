// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// libvpx VP9 software encoder — see vp9_encoder.h.
//
// TODO(T17-build-env): validate compile + link once the from-source
// Chromium / libwebrtc / libvpx build environment is provisioned (see
// docs/build/chromium-from-source.md). This file is authored against
// the documented libvpx and libwebrtc headers; until the build env is
// up, treat it as design-by-spec.
//
// Tuning rationale (every flag choice is justified):
//   docs/internal/vp9-tuning-rationale.md.

#include "capture/encoder/vp9_encoder.h"

#include <algorithm>
#include <cstring>
#include <thread>

#include "api/video/i420_buffer.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"

// libvpx headers.  We deliberately pull these in here rather than in
// the header so callers don't transitively get the C codec API.
extern "C" {
#include "third_party/libvpx/source/libvpx/vpx/vp8cx.h"
#include "third_party/libvpx/source/libvpx/vpx/vpx_codec.h"
#include "third_party/libvpx/source/libvpx/vpx/vpx_encoder.h"
#include "third_party/libvpx/source/libvpx/vpx/vpx_image.h"
}

namespace cloud_browser {
namespace {

// Helper for VPX control with a single-int argument; returns true on
// success and logs on failure (libvpx returns non-zero on error).
bool VpxControl(vpx_codec_ctx_t* ctx, int ctrl_id, int value) {
  vpx_codec_err_t err = vpx_codec_control_(ctx, ctrl_id, value);
  if (err != VPX_CODEC_OK) {
    RTC_LOG(LS_ERROR) << "vpx_codec_control failed: ctrl=" << ctrl_id
                      << " value=" << value
                      << " err=" << vpx_codec_err_to_string(err);
    return false;
  }
  return true;
}

}  // namespace

Vp9Encoder::Vp9Encoder(Vp9EncoderConfig config)
    : config_(config) {}

Vp9Encoder::~Vp9Encoder() {
  Release();
}

int32_t Vp9Encoder::InitEncode(
    const webrtc::VideoCodec* codec_settings,
    const webrtc::VideoEncoder::Settings& /*settings*/) {
  if (codec_settings == nullptr ||
      codec_settings->width == 0 ||
      codec_settings->height == 0) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  width_ = codec_settings->width;
  height_ = codec_settings->height;

  vpx_codec_enc_cfg_t cfg{};
  if (vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &cfg, 0) != VPX_CODEC_OK) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // Geometry.
  cfg.g_w = width_;
  cfg.g_h = height_;
  cfg.g_timebase.num = 1;
  cfg.g_timebase.den = 90'000;  // 90 kHz, the RTP video clock.

  // Latency-critical knobs (see docs/internal/vp9-tuning-rationale.md):
  cfg.g_lag_in_frames = 0;
  cfg.g_pass = VPX_RC_ONE_PASS;
  cfg.rc_end_usage = VPX_CBR;
  cfg.rc_target_bitrate = std::max(1, config_.target_bitrate_bps / 1000);
  cfg.rc_min_quantizer = 2;
  cfg.rc_max_quantizer = 56;
  cfg.rc_undershoot_pct = 50;
  cfg.rc_overshoot_pct = 50;
  cfg.rc_buf_initial_sz = 500;
  cfg.rc_buf_optimal_sz = 600;
  cfg.rc_buf_sz = 1000;
  cfg.rc_dropframe_thresh = 0;        // never drop in encoder; libwebrtc
                                       // pacer handles backpressure.
  cfg.rc_resize_allowed = 0;          // resolution changes are an upper-
                                       // layer decision in v1.
  cfg.kf_mode = VPX_KF_DISABLED;
  if (config_.keyframe_interval > 0) {
    cfg.kf_mode = VPX_KF_AUTO;
    cfg.kf_min_dist = config_.keyframe_interval;
    cfg.kf_max_dist = config_.keyframe_interval;
  }
  cfg.g_threads = config_.num_threads > 0
      ? static_cast<unsigned int>(config_.num_threads)
      : std::max(1u, std::thread::hardware_concurrency() - 1);
  cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;

  codec_ = std::make_unique<vpx_codec_ctx>();
  std::memset(codec_.get(), 0, sizeof(vpx_codec_ctx));

  if (vpx_codec_enc_init(codec_.get(), vpx_codec_vp9_cx(), &cfg, 0)
        != VPX_CODEC_OK) {
    RTC_LOG(LS_ERROR) << "vpx_codec_enc_init failed";
    codec_.reset();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  raw_image_ = std::make_unique<vpx_image>();
  if (vpx_img_alloc(raw_image_.get(), VPX_IMG_FMT_I420,
                    width_, height_, 32) == nullptr) {
    vpx_codec_destroy(codec_.get());
    codec_.reset();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  if (!ApplyVp9Controls()) {
    Release();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  initialized_ = true;
  frames_in_ = 0;
  return WEBRTC_VIDEO_CODEC_OK;
}

bool Vp9Encoder::ApplyVp9Controls() {
  if (!codec_) return false;
  bool ok = true;
  // Realtime/screen content + intra-refresh + no temporal denoising.
  // Each of these is justified in vp9-tuning-rationale.md.
  ok &= VpxControl(codec_.get(), VP8E_SET_CPUUSED, 8);  // fastest preset.
  ok &= VpxControl(codec_.get(), VP9E_SET_TUNE_CONTENT, VP9E_CONTENT_SCREEN);
  ok &= VpxControl(codec_.get(), VP9E_SET_AQ_MODE, 3);  // cyclic refresh.
  ok &= VpxControl(codec_.get(), VP9E_SET_DELTAQ_MODE, 0);
  ok &= VpxControl(codec_.get(), VP9E_SET_NOISE_SENSITIVITY, 0);
  ok &= VpxControl(codec_.get(), VP9E_SET_FRAME_PARALLEL_DECODING, 0);
  ok &= VpxControl(codec_.get(), VP9E_SET_ROW_MT, 1);
  ok &= VpxControl(codec_.get(),
                   VP9E_SET_AQ_MODE_CYCLIC_REFRESH_PERIOD,
                   std::max(1, config_.intra_refresh_period_frames));
  return ok;
}

int32_t Vp9Encoder::Encode(
    const webrtc::VideoFrame& frame,
    const std::vector<webrtc::VideoFrameType>* frame_types) {
  if (!initialized_ || !callback_ || !codec_ || !raw_image_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (frame.width() != width_ || frame.height() != height_) {
    // v1: re-init on resolution change. Phase 2 may instead drop the
    // resize to a separate scaler upstream.
    Release();
    webrtc::VideoCodec settings{};
    settings.width = frame.width();
    settings.height = frame.height();
    if (InitEncode(&settings, /*settings=*/{}) != WEBRTC_VIDEO_CODEC_OK) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
  }

  // Convert to libvpx's I420 view. We assume the buffer is already
  // I420; if not, the caller must ToI420() upstream.  This keeps the
  // hot path allocation-free.
  rtc::scoped_refptr<webrtc::I420BufferInterface> i420 =
      frame.video_frame_buffer()->ToI420();
  if (!i420) return WEBRTC_VIDEO_CODEC_ERROR;
  raw_image_->planes[VPX_PLANE_Y] = const_cast<uint8_t*>(i420->DataY());
  raw_image_->planes[VPX_PLANE_U] = const_cast<uint8_t*>(i420->DataU());
  raw_image_->planes[VPX_PLANE_V] = const_cast<uint8_t*>(i420->DataV());
  raw_image_->stride[VPX_PLANE_Y] = i420->StrideY();
  raw_image_->stride[VPX_PLANE_U] = i420->StrideU();
  raw_image_->stride[VPX_PLANE_V] = i420->StrideV();

  // Force keyframe if libwebrtc asked for one.
  vpx_enc_frame_flags_t flags = 0;
  if (frame_types != nullptr) {
    for (const auto& t : *frame_types) {
      if (t == webrtc::VideoFrameType::kVideoFrameKey) {
        flags |= VPX_EFLAG_FORCE_KF;
        break;
      }
    }
  }

  // 90 kHz timebase: ticks per frame at our framerate.
  const int64_t duration = config_.framerate > 0
      ? 90'000 / config_.framerate
      : 3000;  // 30 fps fallback.
  if (vpx_codec_encode(codec_.get(), raw_image_.get(),
                       /*pts=*/static_cast<int64_t>(frames_in_ * duration),
                       /*duration=*/static_cast<unsigned long>(duration),
                       flags, config_.deadline) != VPX_CODEC_OK) {
    RTC_LOG(LS_ERROR) << "vpx_codec_encode failed";
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  ++frames_in_;

  // Drain encoded packets and forward each to libwebrtc.
  vpx_codec_iter_t iter = nullptr;
  const vpx_codec_cx_pkt_t* pkt = nullptr;
  while ((pkt = vpx_codec_get_cx_data(codec_.get(), &iter)) != nullptr) {
    if (pkt->kind != VPX_CODEC_CX_FRAME_PKT) continue;
    webrtc::EncodedImage encoded_image;
    encoded_image.SetEncodedData(webrtc::EncodedImageBuffer::Create(
        static_cast<const uint8_t*>(pkt->data.frame.buf),
        pkt->data.frame.sz));
    encoded_image._frameType = (pkt->data.frame.flags & VPX_FRAME_IS_KEY)
        ? webrtc::VideoFrameType::kVideoFrameKey
        : webrtc::VideoFrameType::kVideoFrameDelta;
    encoded_image._encodedWidth = width_;
    encoded_image._encodedHeight = height_;
    encoded_image.SetTimestamp(frame.timestamp());
    encoded_image.capture_time_ms_ = frame.render_time_ms();
    encoded_image.rotation_ = frame.rotation();

    webrtc::CodecSpecificInfo codec_specific{};
    codec_specific.codecType = webrtc::kVideoCodecVP9;

    auto result = callback_->OnEncodedImage(encoded_image, &codec_specific);
    if (result.error != webrtc::EncodedImageCallback::Result::OK) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t Vp9Encoder::RegisterEncodeCompleteCallback(
    webrtc::EncodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t Vp9Encoder::Release() {
  if (codec_) {
    vpx_codec_destroy(codec_.get());
    codec_.reset();
  }
  if (raw_image_) {
    vpx_img_free(raw_image_.get());
    raw_image_.reset();
  }
  initialized_ = false;
  callback_ = nullptr;
  return WEBRTC_VIDEO_CODEC_OK;
}

void Vp9Encoder::SetRates(const RateControlParameters& parameters) {
  if (!codec_) return;
  config_.target_bitrate_bps =
      static_cast<int>(parameters.bitrate.get_sum_bps());
  config_.framerate = std::max(1, static_cast<int>(parameters.framerate_fps));

  vpx_codec_enc_cfg_t cfg{};
  // libvpx doesn't expose a getter; we keep our own and re-push the
  // bitrate-relevant fields. Geometry is unchanged here.
  if (vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &cfg, 0)
        != VPX_CODEC_OK) {
    return;
  }
  cfg.g_w = width_;
  cfg.g_h = height_;
  cfg.g_timebase.num = 1;
  cfg.g_timebase.den = 90'000;
  cfg.g_lag_in_frames = 0;
  cfg.g_pass = VPX_RC_ONE_PASS;
  cfg.rc_end_usage = VPX_CBR;
  cfg.rc_target_bitrate = std::max(1, config_.target_bitrate_bps / 1000);
  cfg.kf_mode = VPX_KF_DISABLED;

  if (vpx_codec_enc_config_set(codec_.get(), &cfg) != VPX_CODEC_OK) {
    RTC_LOG(LS_WARNING) << "vpx_codec_enc_config_set failed";
  }
}

webrtc::VideoEncoder::EncoderInfo Vp9Encoder::GetEncoderInfo() const {
  EncoderInfo info;
  info.implementation_name = config_.low_latency_tag
      ? "cloud-browser-vp9-libvpx-lowlatency"
      : "cloud-browser-vp9-libvpx";
  info.is_hardware_accelerated = false;
  info.supports_native_handle = false;
  info.supports_simulcast = false;
  info.has_trusted_rate_controller = true;  // CBR + dropframe=0.
  return info;
}

}  // namespace cloud_browser

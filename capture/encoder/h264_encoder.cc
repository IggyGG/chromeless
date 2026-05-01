// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// x264 H.264 software encoder — see h264_encoder.h.
//
// TODO(T17-build-env): validate compile + link once the from-source
// Chromium / libwebrtc / x264 build is up. Authored against the
// documented x264 (Annex-B output, intra-refresh) and libwebrtc
// (modules/video_coding/include) headers.
//
// Tuning rationale: docs/internal/h264-tuning-rationale.md.

#include "capture/encoder/h264_encoder.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>
#include <thread>

#include "api/video/i420_buffer.h"
#include "modules/video_coding/codecs/h264/include/h264_globals.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"

#if defined(HAS_X264)
extern "C" {
#include "third_party/x264/x264.h"
}
#endif  // HAS_X264
// In the disabled path, the field declarations themselves are gated out
// in h264_encoder.h, so no x264_t / x264_picture_t storage exists in
// this TU. The header keeps opaque forward declarations for any future
// caller that needs to refer to the typenames.

namespace cloud_browser {
namespace {

constexpr int kFallbackThreads = 4;

// Decode the 6-hex-char `profile-level-id` per RFC 6184 §8.1:
//   bytes 0..1: profile_idc, byte 2 packs constraint_set flags +
//   reserved, bytes 3..5 (decimal) are level_idc * 10. We accept the
//   short list our SDP advertises:
//     "42e01f" — Constrained Baseline 3.1
//     "4d401f" — Main 3.1 (no B-frames variant)
//     "640c1f" — Constrained High 3.1
#if defined(HAS_X264)
bool ParseProfileLevelId(const std::string& s, std::string* profile,
                         int* level_idc) {
  if (s.size() != 6) return false;
  // Profile detection by first three hex chars + constraint set hint.
  if (s.compare(0, 4, "42e0") == 0) {
    *profile = "baseline";
  } else if (s.compare(0, 4, "4d40") == 0 ||
             s.compare(0, 4, "4d00") == 0) {
    *profile = "main";
  } else if (s.compare(0, 4, "640c") == 0 ||
             s.compare(0, 4, "6400") == 0) {
    *profile = "high";
  } else {
    return false;
  }
  // level_idc = last two hex chars * 1 (e.g. "1f" = 31 = level 3.1).
  char* end = nullptr;
  long lvl = std::strtol(s.substr(4, 2).c_str(), &end, 16);
  if (end == s.c_str() + 4) return false;
  *level_idc = static_cast<int>(lvl);
  return true;
}
#endif  // HAS_X264

}  // namespace

// Out-of-line lifecycle for H264EncoderConfig (chromium-style: the struct
// holds std::string members with non-trivial destructors, which the
// chromium plugin wants pinned to the .cc rather than inlined into every
// TU that #includes h264_encoder.h).
H264EncoderConfig::H264EncoderConfig() = default;
H264EncoderConfig::~H264EncoderConfig() = default;
H264EncoderConfig::H264EncoderConfig(const H264EncoderConfig&) = default;
H264EncoderConfig& H264EncoderConfig::operator=(const H264EncoderConfig&) =
    default;
H264EncoderConfig::H264EncoderConfig(H264EncoderConfig&&) = default;
H264EncoderConfig& H264EncoderConfig::operator=(H264EncoderConfig&&) = default;

H264Encoder::H264Encoder(H264EncoderConfig config) : config_(std::move(config)) {}

H264Encoder::~H264Encoder() { Release(); }

bool H264Encoder::ResolveProfileLevel(std::string* profile,
                                      int* level_idc) const {
#if defined(HAS_X264)
  return ParseProfileLevelId(config_.profile_level_id, profile, level_idc);
#else
  // Without x264, profile resolution is meaningless.
  (void)profile;
  (void)level_idc;
  return false;
#endif  // HAS_X264
}

int32_t H264Encoder::InitEncode(
    const webrtc::VideoCodec* codec_settings,
    const webrtc::VideoEncoder::Settings& /*settings*/) {
#if defined(HAS_X264)
  if (codec_settings == nullptr ||
      codec_settings->width == 0 ||
      codec_settings->height == 0) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  width_ = codec_settings->width;
  height_ = codec_settings->height;

  x264_param_t params{};
  if (x264_param_default_preset(&params,
                                config_.preset.c_str(),
                                config_.tune.c_str()) != 0) {
    RTC_LOG(LS_ERROR) << "x264_param_default_preset failed";
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // Geometry / format.
  params.i_width  = width_;
  params.i_height = height_;
  params.i_csp    = X264_CSP_I420;
  params.i_fps_num = static_cast<uint32_t>(std::max(1, config_.framerate));
  params.i_fps_den = 1;
  params.b_vfr_input = 0;     // CFR; pts is an ever-increasing tick.
  params.i_timebase_num = 1;
  params.i_timebase_den = 90'000;
  params.i_log_level = X264_LOG_ERROR;

  // Latency-critical knobs (see h264-tuning-rationale.md).
  params.i_keyint_max = std::numeric_limits<int>::max();  // no auto IDR.
  params.i_keyint_min = std::numeric_limits<int>::max();
  params.b_intra_refresh = 1;
  params.i_bframe = 0;
  params.i_sync_lookahead = 0;
  params.rc.i_lookahead = 0;
  params.rc.i_rc_method = X264_RC_ABR;
  params.rc.i_bitrate = std::max(1, config_.target_bitrate_bps / 1000);
  params.rc.i_vbv_max_bitrate = params.rc.i_bitrate;
  params.rc.i_vbv_buffer_size = std::max(1, params.rc.i_bitrate);  // ~1s buffer
  params.b_repeat_headers = 1;   // SPS/PPS in front of every IDR-equivalent.
  params.b_annexb = 1;           // Annex-B start codes; libwebrtc expects this.

  // Threading: slice-based threading at slow speeds buys latency back
  // by avoiding x264's frame-pipelined parallelism (which costs us
  // ~1 frame of lag per worker).
  int n_threads = config_.num_threads > 0
      ? config_.num_threads
      : std::max<int>(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);
  if (n_threads <= 0) n_threads = kFallbackThreads;
  params.i_threads = n_threads;
  params.i_lookahead_threads = 1;
  params.b_sliced_threads = 1;

  // Profile / level.
  std::string profile;
  int level_idc = 31;
  if (ResolveProfileLevel(&profile, &level_idc)) {
    if (x264_param_apply_profile(&params, profile.c_str()) != 0) {
      RTC_LOG(LS_WARNING) << "x264_param_apply_profile(" << profile
                          << ") failed; using preset default profile";
    }
    params.i_level_idc = level_idc;
  }

  encoder_ = x264_encoder_open(&params);
  if (!encoder_) {
    RTC_LOG(LS_ERROR) << "x264_encoder_open failed";
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  pic_in_  = std::make_unique<x264_picture_t>();
  pic_out_ = std::make_unique<x264_picture_t>();
  if (x264_picture_alloc(pic_in_.get(), X264_CSP_I420, width_, height_) != 0) {
    x264_encoder_close(encoder_);
    encoder_ = nullptr;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  std::memset(pic_out_.get(), 0, sizeof(*pic_out_));

  initialized_ = true;
  frames_in_ = 0;
  return WEBRTC_VIDEO_CODEC_OK;
#else
  // x264 not built in; signal that we cannot initialize. The factory's
  // ProbeAvailable check should have ruled this codec out before
  // CreateVideoEncoder, so this path is defensive.
  (void)codec_settings;
  return WEBRTC_VIDEO_CODEC_ERROR;
#endif  // HAS_X264
}

int32_t H264Encoder::Encode(
    const webrtc::VideoFrame& frame,
    const std::vector<webrtc::VideoFrameType>* frame_types) {
#if defined(HAS_X264)
  if (!initialized_ || encoder_ == nullptr || callback_ == nullptr) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (frame.width() != width_ || frame.height() != height_) {
    Release();
    webrtc::VideoCodec settings{};
    settings.width = frame.width();
    settings.height = frame.height();
    if (InitEncode(&settings, webrtc::VideoEncoder::Settings())
          != WEBRTC_VIDEO_CODEC_OK) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
  }

  rtc::scoped_refptr<webrtc::I420BufferInterface> i420 =
      frame.video_frame_buffer()->ToI420();
  if (!i420) return WEBRTC_VIDEO_CODEC_ERROR;

  pic_in_->img.i_csp = X264_CSP_I420;
  pic_in_->img.i_plane = 3;
  pic_in_->img.plane[0] = const_cast<uint8_t*>(i420->DataY());
  pic_in_->img.plane[1] = const_cast<uint8_t*>(i420->DataU());
  pic_in_->img.plane[2] = const_cast<uint8_t*>(i420->DataV());
  pic_in_->img.i_stride[0] = i420->StrideY();
  pic_in_->img.i_stride[1] = i420->StrideU();
  pic_in_->img.i_stride[2] = i420->StrideV();
  pic_in_->i_pts = static_cast<int64_t>(frames_in_++);

  // Forced keyframe: x264 has no separate "force IDR" outside
  // intra-refresh, but setting i_type = X264_TYPE_IDR is the documented
  // way to request one.
  pic_in_->i_type = X264_TYPE_AUTO;
  if (frame_types != nullptr) {
    for (const auto& t : *frame_types) {
      if (t == webrtc::VideoFrameType::kVideoFrameKey) {
        pic_in_->i_type = X264_TYPE_IDR;
        break;
      }
    }
  }

  x264_nal_t* nals = nullptr;
  int num_nals = 0;
  int frame_size =
      x264_encoder_encode(encoder_, &nals, &num_nals, pic_in_.get(), pic_out_.get());
  if (frame_size < 0) {
    RTC_LOG(LS_ERROR) << "x264_encoder_encode failed";
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  if (frame_size == 0 || num_nals == 0) {
    return WEBRTC_VIDEO_CODEC_OK;  // Encoder swallowed the frame; no
                                    // output yet (shouldn't happen with
                                    // lookahead = 0, but be safe).
  }

  // x264 hands us a contiguous buffer covering all NALs (annex-B-prefixed
  // because we set b_annexb=1). Wrap it without copying.
  const uint8_t* annex_b = nals[0].p_payload;
  size_t total_size = static_cast<size_t>(frame_size);

  webrtc::EncodedImage encoded_image;
  encoded_image.SetEncodedData(
      webrtc::EncodedImageBuffer::Create(annex_b, total_size));
  encoded_image._frameType = pic_out_->b_keyframe
      ? webrtc::VideoFrameType::kVideoFrameKey
      : webrtc::VideoFrameType::kVideoFrameDelta;
  encoded_image._encodedWidth = width_;
  encoded_image._encodedHeight = height_;
  encoded_image.SetTimestamp(frame.timestamp());
  encoded_image.capture_time_ms_ = frame.render_time_ms();
  encoded_image.rotation_ = frame.rotation();

  webrtc::CodecSpecificInfo csi{};
  csi.codecType = webrtc::kVideoCodecH264;
  csi.codecSpecific.H264.packetization_mode =
      webrtc::H264PacketizationMode::NonInterleaved;

  auto result = callback_->OnEncodedImage(encoded_image, &csi);
  if (result.error != webrtc::EncodedImageCallback::Result::OK) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  return WEBRTC_VIDEO_CODEC_OK;
#else
  (void)frame;
  (void)frame_types;
  return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
#endif  // HAS_X264
}

int32_t H264Encoder::RegisterEncodeCompleteCallback(
    webrtc::EncodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t H264Encoder::Release() {
#if defined(HAS_X264)
  if (encoder_ != nullptr) {
    x264_encoder_close(encoder_);
    encoder_ = nullptr;
  }
  if (pic_in_) {
    x264_picture_clean(pic_in_.get());
    pic_in_.reset();
  }
  pic_out_.reset();
#endif  // HAS_X264
  initialized_ = false;
  callback_ = nullptr;
  return WEBRTC_VIDEO_CODEC_OK;
}

void H264Encoder::SetRates(const RateControlParameters& parameters) {
#if defined(HAS_X264)
  if (encoder_ == nullptr) return;
  config_.target_bitrate_bps =
      static_cast<int>(parameters.bitrate.get_sum_bps());
  config_.framerate = std::max(1, static_cast<int>(parameters.framerate_fps));

  x264_param_t cur;
  x264_encoder_parameters(encoder_, &cur);
  cur.rc.i_bitrate = std::max(1, config_.target_bitrate_bps / 1000);
  cur.rc.i_vbv_max_bitrate = cur.rc.i_bitrate;
  cur.rc.i_vbv_buffer_size = std::max(1, cur.rc.i_bitrate);
  cur.i_fps_num = static_cast<uint32_t>(config_.framerate);
  cur.i_fps_den = 1;
  if (x264_encoder_reconfig(encoder_, &cur) != 0) {
    RTC_LOG(LS_WARNING) << "x264_encoder_reconfig failed";
  }
#else
  // Track the requested rate so observability still sees it; no encoder
  // to reconfigure when x264 isn't built in.
  config_.target_bitrate_bps =
      static_cast<int>(parameters.bitrate.get_sum_bps());
  config_.framerate = std::max(1, static_cast<int>(parameters.framerate_fps));
#endif  // HAS_X264
}

webrtc::VideoEncoder::EncoderInfo H264Encoder::GetEncoderInfo() const {
  EncoderInfo info;
#if defined(HAS_X264)
  info.implementation_name = config_.low_latency_tag
      ? "cloud-browser-h264-x264-lowlatency"
      : "cloud-browser-h264-x264";
#else
  info.implementation_name = "cloud-browser-h264-stub";
#endif  // HAS_X264
  info.is_hardware_accelerated = false;
  info.supports_native_handle = false;
  info.supports_simulcast = false;
  info.has_trusted_rate_controller = true;  // ABR + no dropframe.
  return info;
}

}  // namespace cloud_browser

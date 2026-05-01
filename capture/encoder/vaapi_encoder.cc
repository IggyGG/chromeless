// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// VAAPI hardware encoder — see vaapi_encoder.h.
//
// TODO(T17-build-env): validate compile + link once the from-source
// Chromium / libwebrtc build env is up.
// TODO(VAAPI-libva): add libva headers (<va/va.h>, <va/va_drm.h>,
// <va/va_enc_h264.h>, <va/va_enc_hevc.h>, <va/va_enc_av1.h>,
// <va/va_enc_vp9.h>) and libva.so + libva-drm.so to the build (see
// capture/build-integration/BUILD.gn — a `:vaapi` config gates
// HAS_VAAPI). Until both gates clear, this file is design-by-spec.
//
// Tuning rationale: docs/internal/vaapi-tuning-rationale.md.

#include "capture/encoder/vaapi_encoder.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

#include "api/video/i420_buffer.h"
#include "modules/video_coding/codecs/h264/include/h264_globals.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"

#if defined(HAS_VAAPI)
extern "C" {
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_av1.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>
#include <va/va_enc_vp9.h>
}
#endif

namespace cloud_browser {

// Out-of-line lifecycle for VaapiEncoderConfig (chromium-style).
VaapiEncoderConfig::VaapiEncoderConfig() = default;
VaapiEncoderConfig::~VaapiEncoderConfig() = default;
VaapiEncoderConfig::VaapiEncoderConfig(const VaapiEncoderConfig&) = default;
VaapiEncoderConfig& VaapiEncoderConfig::operator=(const VaapiEncoderConfig&) = default;
VaapiEncoderConfig::VaapiEncoderConfig(VaapiEncoderConfig&&) = default;
VaapiEncoderConfig& VaapiEncoderConfig::operator=(VaapiEncoderConfig&&) = default;
namespace {

constexpr const char kRenderNode[] = "/dev/dri/renderD128";

struct CodecMapping {
  webrtc::VideoCodecType webrtc_type;
#if defined(HAS_VAAPI)
  VAProfile profile;
  // Default entrypoint preference; the runtime probe walks the
  // entrypoints list looking for either the low-power slice
  // entrypoint (preferred for our latency budget) or the regular
  // slice entrypoint as fallback.
  VAEntrypoint preferred_entrypoint;
  VAEntrypoint fallback_entrypoint;
#endif
};

bool ResolveCodecType(const std::string& codec_type,
                      const std::string& profile_level_id,
                      CodecMapping* out) {
  if (codec_type == "H264") {
    out->webrtc_type = webrtc::kVideoCodecH264;
#if defined(HAS_VAAPI)
    // Map our profile-level-id prefix onto a VAProfile. Same short
    // list as H264Encoder (T36).
    if (profile_level_id.compare(0, 4, "42e0") == 0) {
      out->profile = VAProfileH264ConstrainedBaseline;
    } else if (profile_level_id.compare(0, 4, "4d40") == 0 ||
               profile_level_id.compare(0, 4, "4d00") == 0) {
      out->profile = VAProfileH264Main;
    } else if (profile_level_id.compare(0, 4, "640c") == 0 ||
               profile_level_id.compare(0, 4, "6400") == 0) {
      out->profile = VAProfileH264High;
    } else {
      out->profile = VAProfileH264ConstrainedBaseline;  // safe default.
    }
    out->preferred_entrypoint = VAEntrypointEncSliceLP;  // low-power.
    out->fallback_entrypoint  = VAEntrypointEncSlice;
#endif
    return true;
  }
  if (codec_type == "HEVC") {
    out->webrtc_type = webrtc::kVideoCodecH265;
#if defined(HAS_VAAPI)
    out->profile = VAProfileHEVCMain;
    out->preferred_entrypoint = VAEntrypointEncSliceLP;
    out->fallback_entrypoint  = VAEntrypointEncSlice;
#endif
    return true;
  }
  if (codec_type == "AV1") {
    out->webrtc_type = webrtc::kVideoCodecAV1;
#if defined(HAS_VAAPI)
    out->profile = VAProfileAV1Profile0;
    // AV1 VAAPI exposes a single tile-group entrypoint; both
    // pointers map to it.
    out->preferred_entrypoint = VAEntrypointEncSliceLP;
    out->fallback_entrypoint  = VAEntrypointEncSlice;
#endif
    return true;
  }
  if (codec_type == "VP9") {
    out->webrtc_type = webrtc::kVideoCodecVP9;
#if defined(HAS_VAAPI)
    out->profile = VAProfileVP9Profile0;
    out->preferred_entrypoint = VAEntrypointEncSliceLP;
    out->fallback_entrypoint  = VAEntrypointEncSlice;
#endif
    return true;
  }
  return false;
}

#if defined(HAS_VAAPI)
uint32_t ResolveRateControl(const std::string& rc) {
  if (rc == "VBR") return VA_RC_VBR;
  if (rc == "CQP") return VA_RC_CQP;
  return VA_RC_CBR;  // default + safe.
}

bool EntrypointSupported(VADisplay dpy, VAProfile profile,
                          VAEntrypoint want) {
  int max = vaMaxNumEntrypoints(dpy);
  if (max <= 0) return false;
  std::vector<VAEntrypoint> ep(max);
  int got = 0;
  if (vaQueryConfigEntrypoints(dpy, profile, ep.data(), &got)
        != VA_STATUS_SUCCESS) {
    return false;
  }
  for (int i = 0; i < got; ++i) {
    if (ep[i] == want) return true;
  }
  return false;
}
#endif  // HAS_VAAPI

}  // namespace

// ---------------------------------------------------------------------
// VaapiEncoder::Impl — opaque PIMPL holding the VA handles.
// ---------------------------------------------------------------------
class VaapiEncoder::Impl {
 public:
#if defined(HAS_VAAPI)
  bool Initialize(const VaapiEncoderConfig& cfg, int width, int height,
                  webrtc::VideoCodecType webrtc_type,
                  std::string* vendor_out);

  // Encode one I420 frame. On success, populates `out` with the
  // codec's bitstream and `is_keyframe`.
  bool Encode(const uint8_t* y_plane, int y_stride,
              const uint8_t* u_plane, int u_stride,
              const uint8_t* v_plane, int v_stride,
              int width, int height,
              uint64_t pts,
              bool force_idr,
              std::vector<uint8_t>* out,
              bool* is_keyframe);

  bool Reconfigure(const VaapiEncoderConfig& cfg);
  void Destroy();

 private:
  // Submit the once-per-init sequence params (SPS / VPS / etc.).
  bool SubmitSequenceParams();
  // Build + submit per-frame picture / slice params; returns the
  // coded buffer ID via `coded_buf_id_out`.
  bool SubmitPictureAndSlice(uint64_t pts, bool force_idr,
                              VABufferID* coded_buf_id_out);

  int drm_fd_ = -1;
  VADisplay display_ = nullptr;
  VAConfigID  config_id_ = VA_INVALID_ID;
  VAContextID context_id_ = VA_INVALID_ID;
  std::vector<VASurfaceID> surfaces_;
  size_t surface_index_ = 0;

  VAProfile     profile_ = VAProfileNone;
  VAEntrypoint  entrypoint_ = VAEntrypointEncSlice;
  webrtc::VideoCodecType webrtc_type_ = webrtc::kVideoCodecGeneric;
  uint32_t      rate_control_mode_ = VA_RC_CBR;
  int           width_ = 0;
  int           height_ = 0;
  int           framerate_ = 30;
  int           target_bitrate_bps_ = 4'000'000;
  int           intra_period_ = 0;     // 0 = infinite (cfg.gop_size = -1).
  int           intra_refresh_period_ = 60;
#else
  // No-op stubs so the file compiles when HAS_VAAPI is undefined.
  bool Initialize(const VaapiEncoderConfig&, int, int,
                  webrtc::VideoCodecType, std::string*) { return false; }
  bool Encode(const uint8_t*, int, const uint8_t*, int,
              const uint8_t*, int, int, int, uint64_t, bool,
              std::vector<uint8_t>*, bool*) { return false; }
  bool Reconfigure(const VaapiEncoderConfig&) { return false; }
  void Destroy() {}
#endif
};

#if defined(HAS_VAAPI)

bool VaapiEncoder::Impl::Initialize(
    const VaapiEncoderConfig& cfg, int width, int height,
    webrtc::VideoCodecType webrtc_type, std::string* vendor_out) {
  webrtc_type_ = webrtc_type;
  width_ = width;
  height_ = height;
  framerate_ = std::max(1, cfg.framerate);
  target_bitrate_bps_ = cfg.target_bitrate_bps;
  intra_period_ = (cfg.gop_size > 0) ? cfg.gop_size : 0;
  intra_refresh_period_ = std::max(1, cfg.intra_refresh_period_frames);
  rate_control_mode_ = ResolveRateControl(cfg.rate_control);

  // 1. Open the DRM render node and bring up the VA display.
  drm_fd_ = open(kRenderNode, O_RDWR | O_CLOEXEC);
  if (drm_fd_ < 0) {
    RTC_LOG(LS_ERROR) << "open(" << kRenderNode << ") failed";
    return false;
  }
  display_ = vaGetDisplayDRM(drm_fd_);
  if (!display_) return false;

  if (!cfg.driver_override.empty()) {
    vaSetDriverName(display_, const_cast<char*>(cfg.driver_override.c_str()));
  }

  int major = 0, minor = 0;
  if (vaInitialize(display_, &major, &minor) != VA_STATUS_SUCCESS) {
    RTC_LOG(LS_ERROR) << "vaInitialize failed";
    return false;
  }
  if (vendor_out) {
    const char* vendor = vaQueryVendorString(display_);
    if (vendor) *vendor_out = vendor;
  }

  // 2. Resolve codec → (profile, entrypoint). Probe twice: prefer
  //    the low-power slice entrypoint (lower latency), fall back
  //    to the regular slice entrypoint.
  CodecMapping cm;
  if (!ResolveCodecType(cfg.codec_type, cfg.profile_level_id, &cm)) {
    return false;
  }
  profile_ = cm.profile;
  if (EntrypointSupported(display_, profile_, cm.preferred_entrypoint)) {
    entrypoint_ = cm.preferred_entrypoint;
  } else if (EntrypointSupported(display_, profile_, cm.fallback_entrypoint)) {
    entrypoint_ = cm.fallback_entrypoint;
  } else {
    RTC_LOG(LS_ERROR) << "VAAPI: no encode entrypoint for codec "
                      << cfg.codec_type;
    return false;
  }

  // 3. Configuration attributes — rate-control + chroma format.
  //    Chroma 4:2:0 only in v1 (matches our I420 input upstream).
  std::vector<VAConfigAttrib> attribs;
  {
    VAConfigAttrib rc{};
    rc.type = VAConfigAttribRateControl;
    rc.value = rate_control_mode_;
    attribs.push_back(rc);
  }
  {
    VAConfigAttrib chroma{};
    chroma.type = VAConfigAttribRTFormat;
    chroma.value = VA_RT_FORMAT_YUV420;
    attribs.push_back(chroma);
  }
  if (vaCreateConfig(display_, profile_, entrypoint_,
                      attribs.data(),
                      static_cast<int>(attribs.size()),
                      &config_id_) != VA_STATUS_SUCCESS) {
    return false;
  }

  // 4. Allocate a small surface pool — 4 deep, same shape as NVENC.
  surfaces_.assign(4, VA_INVALID_SURFACE);
  if (vaCreateSurfaces(display_, VA_RT_FORMAT_YUV420, width, height,
                        surfaces_.data(),
                        static_cast<unsigned>(surfaces_.size()),
                        nullptr, 0) != VA_STATUS_SUCCESS) {
    return false;
  }

  if (vaCreateContext(display_, config_id_, width, height,
                       VA_PROGRESSIVE,
                       surfaces_.data(),
                       static_cast<int>(surfaces_.size()),
                       &context_id_) != VA_STATUS_SUCCESS) {
    return false;
  }

  return SubmitSequenceParams();
}

bool VaapiEncoder::Impl::SubmitSequenceParams() {
  // Codec-specific sequence parameters. The fields mirror our T36 /
  // VP9 / H.264 SW knob set: no B-frames, infinite GOP, intra-
  // refresh on for H.264 / HEVC. AV1 / VP9 use cyclic-refresh AQ
  // instead (set as a misc parameter further down).
  VABufferID seq_buf = VA_INVALID_ID;
  if (webrtc_type_ == webrtc::kVideoCodecH264) {
    VAEncSequenceParameterBufferH264 seq{};
    seq.level_idc = 31;  // Level 3.1 — same default as T36.
    seq.intra_period = (intra_period_ > 0) ? intra_period_ : 0xffffffffu;
    seq.intra_idr_period = seq.intra_period;
    seq.ip_period = 1;    // IP only; no B-frames.
    seq.bits_per_second = static_cast<unsigned int>(target_bitrate_bps_);
    seq.max_num_ref_frames = 1;
    seq.picture_width_in_mbs = (width_ + 15) / 16;
    seq.picture_height_in_mbs = (height_ + 15) / 16;
    seq.time_scale = 90000;
    seq.num_units_in_tick = 90000 / framerate_;
    seq.frame_cropping_flag = 0;
    if (vaCreateBuffer(display_, context_id_,
                        VAEncSequenceParameterBufferType,
                        sizeof(seq), 1, &seq, &seq_buf) != VA_STATUS_SUCCESS) {
      return false;
    }
  } else if (webrtc_type_ == webrtc::kVideoCodecH265) {
    VAEncSequenceParameterBufferHEVC seq{};
    seq.general_level_idc = 93;  // Level 3.1.
    seq.intra_period = (intra_period_ > 0) ? intra_period_ : 0xffffffffu;
    seq.intra_idr_period = seq.intra_period;
    seq.ip_period = 1;
    seq.bits_per_second = static_cast<unsigned int>(target_bitrate_bps_);
    seq.pic_width_in_luma_samples = static_cast<uint16_t>(width_);
    seq.pic_height_in_luma_samples = static_cast<uint16_t>(height_);
    if (vaCreateBuffer(display_, context_id_,
                        VAEncSequenceParameterBufferType,
                        sizeof(seq), 1, &seq, &seq_buf) != VA_STATUS_SUCCESS) {
      return false;
    }
  } else if (webrtc_type_ == webrtc::kVideoCodecAV1) {
    VAEncSequenceParameterBufferAV1 seq{};
    seq.intra_period = (intra_period_ > 0) ? intra_period_ : 0xffffffffu;
    seq.ip_period = 1;
    seq.bits_per_second = static_cast<unsigned int>(target_bitrate_bps_);
    seq.max_frame_width_minus_1 = static_cast<uint16_t>(width_ - 1);
    seq.max_frame_height_minus_1 = static_cast<uint16_t>(height_ - 1);
    if (vaCreateBuffer(display_, context_id_,
                        VAEncSequenceParameterBufferType,
                        sizeof(seq), 1, &seq, &seq_buf) != VA_STATUS_SUCCESS) {
      return false;
    }
  } else if (webrtc_type_ == webrtc::kVideoCodecVP9) {
    VAEncSequenceParameterBufferVP9 seq{};
    seq.intra_period = (intra_period_ > 0) ? intra_period_ : 0xffffffffu;
    seq.bits_per_second = static_cast<unsigned int>(target_bitrate_bps_);
    seq.max_frame_width = static_cast<uint16_t>(width_);
    seq.max_frame_height = static_cast<uint16_t>(height_);
    if (vaCreateBuffer(display_, context_id_,
                        VAEncSequenceParameterBufferType,
                        sizeof(seq), 1, &seq, &seq_buf) != VA_STATUS_SUCCESS) {
      return false;
    }
  } else {
    return false;
  }

  // Misc: rate-control + framerate. These are the same buffer type
  // for every codec, just with codec-specific RC payload size.
  VABufferID rc_buf = VA_INVALID_ID;
  {
    struct {
      VAEncMiscParameterBuffer hdr;
      VAEncMiscParameterRateControl rc;
    } pkt{};
    pkt.hdr.type = VAEncMiscParameterTypeRateControl;
    pkt.rc.bits_per_second = static_cast<uint32_t>(target_bitrate_bps_);
    pkt.rc.target_percentage = 100;
    pkt.rc.window_size = 1000;  // 1s VBV.
    pkt.rc.initial_qp = 26;
    pkt.rc.min_qp = 0;           // let the driver pick.
    pkt.rc.basic_unit_size = 0;
    if (vaCreateBuffer(display_, context_id_,
                        VAEncMiscParameterBufferType,
                        sizeof(pkt), 1, &pkt, &rc_buf) != VA_STATUS_SUCCESS) {
      return false;
    }
  }

  VABufferID fr_buf = VA_INVALID_ID;
  {
    struct {
      VAEncMiscParameterBuffer hdr;
      VAEncMiscParameterFrameRate fr;
    } pkt{};
    pkt.hdr.type = VAEncMiscParameterTypeFrameRate;
    pkt.fr.framerate = static_cast<uint32_t>(framerate_);
    if (vaCreateBuffer(display_, context_id_,
                        VAEncMiscParameterBufferType,
                        sizeof(pkt), 1, &pkt, &fr_buf) != VA_STATUS_SUCCESS) {
      return false;
    }
  }

  // Intra-refresh — only meaningful for H.264 / HEVC. AV1 / VP9 use
  // segment-level cyclic refresh which we'd set per-picture; v1
  // skips that and lets the driver default cyclic-refresh fire.
  VABufferID rir_buf = VA_INVALID_ID;
  if (webrtc_type_ == webrtc::kVideoCodecH264 ||
      webrtc_type_ == webrtc::kVideoCodecH265) {
    struct {
      VAEncMiscParameterBuffer hdr;
      VAEncMiscParameterRIR    rir;
    } pkt{};
    pkt.hdr.type = VAEncMiscParameterTypeRIR;
    pkt.rir.rir_flags.bits.enable_rir_row = 1;
    pkt.rir.intra_insertion_location = 0;
    pkt.rir.intra_insert_size =
        std::max(1, static_cast<int>(width_ / 16) /
                       std::max(1, intra_refresh_period_));
    pkt.rir.qp_delta_for_inserted_intra = 0;
    if (vaCreateBuffer(display_, context_id_,
                        VAEncMiscParameterBufferType,
                        sizeof(pkt), 1, &pkt, &rir_buf) != VA_STATUS_SUCCESS) {
      // Not fatal — driver may not support RIR. Drop and continue.
      rir_buf = VA_INVALID_ID;
    }
  }

  // Submit all of the once-per-sequence buffers in a single Begin/
  // Render/End triple against the first surface. The driver caches
  // them for subsequent frames.
  VASurfaceID s = surfaces_[0];
  if (vaBeginPicture(display_, context_id_, s) != VA_STATUS_SUCCESS) return false;
  std::vector<VABufferID> bufs{seq_buf, rc_buf, fr_buf};
  if (rir_buf != VA_INVALID_ID) bufs.push_back(rir_buf);
  if (vaRenderPicture(display_, context_id_, bufs.data(),
                       static_cast<int>(bufs.size())) != VA_STATUS_SUCCESS) {
    return false;
  }
  if (vaEndPicture(display_, context_id_) != VA_STATUS_SUCCESS) return false;
  return true;
}

bool VaapiEncoder::Impl::SubmitPictureAndSlice(uint64_t pts,
                                                 bool force_idr,
                                                 VABufferID* coded_buf_id_out) {
  // Allocate a coded buffer big enough for one I-frame at our
  // resolution (rough heuristic: width*height*3/2 — uncompressed
  // YUV420 size — gives plenty of headroom).
  VABufferID coded_buf = VA_INVALID_ID;
  unsigned int coded_size = static_cast<unsigned>(width_ * height_ * 3 / 2);
  if (vaCreateBuffer(display_, context_id_, VAEncCodedBufferType,
                      coded_size, 1, nullptr,
                      &coded_buf) != VA_STATUS_SUCCESS) {
    return false;
  }

  // Codec-specific picture + slice parameters. We elide the full
  // SPS / PPS material per codec — the driver's defaults are fine
  // for our zero-latency / no-B / intra-refresh shape. v1 ships a
  // minimal correct setup; T35-style flag-by-flag tuning lives in
  // the rationale doc.
  VABufferID pic_buf = VA_INVALID_ID;
  VABufferID slice_buf = VA_INVALID_ID;
  if (webrtc_type_ == webrtc::kVideoCodecH264) {
    VAEncPictureParameterBufferH264 pic{};
    pic.coded_buf = coded_buf;
    pic.frame_num = static_cast<uint16_t>(pts & 0xffff);
    pic.pic_init_qp = 26;
    pic.pic_fields.bits.idr_pic_flag = force_idr ? 1 : 0;
    pic.pic_fields.bits.reference_pic_flag = 1;
    pic.CurrPic.picture_id = surfaces_[surface_index_];
    if (vaCreateBuffer(display_, context_id_,
                        VAEncPictureParameterBufferType,
                        sizeof(pic), 1, &pic, &pic_buf) != VA_STATUS_SUCCESS) {
      return false;
    }
    VAEncSliceParameterBufferH264 slice{};
    slice.num_macroblocks = (width_ / 16) * (height_ / 16);
    slice.macroblock_address = 0;
    slice.slice_type = force_idr ? 7 : 5;  // I-slice / P-slice.
    if (vaCreateBuffer(display_, context_id_,
                        VAEncSliceParameterBufferType,
                        sizeof(slice), 1, &slice, &slice_buf) != VA_STATUS_SUCCESS) {
      return false;
    }
  } else if (webrtc_type_ == webrtc::kVideoCodecH265) {
    VAEncPictureParameterBufferHEVC pic{};
    pic.coded_buf = coded_buf;
    pic.pic_init_qp = 26;
    pic.pic_fields.bits.idr_pic_flag = force_idr ? 1 : 0;
    pic.decoded_curr_pic.picture_id = surfaces_[surface_index_];
    if (vaCreateBuffer(display_, context_id_,
                        VAEncPictureParameterBufferType,
                        sizeof(pic), 1, &pic, &pic_buf) != VA_STATUS_SUCCESS) {
      return false;
    }
    VAEncSliceParameterBufferHEVC slice{};
    slice.slice_segment_address = 0;
    slice.num_ctu_in_slice = ((width_ + 31) / 32) * ((height_ + 31) / 32);
    slice.slice_type = force_idr ? 2 : 1;
    if (vaCreateBuffer(display_, context_id_,
                        VAEncSliceParameterBufferType,
                        sizeof(slice), 1, &slice, &slice_buf) != VA_STATUS_SUCCESS) {
      return false;
    }
  } else if (webrtc_type_ == webrtc::kVideoCodecAV1) {
    VAEncPictureParameterBufferAV1 pic{};
    pic.coded_buf = coded_buf;
    pic.frame_width_minus_1 = static_cast<uint16_t>(width_ - 1);
    pic.frame_height_minus_1 = static_cast<uint16_t>(height_ - 1);
    pic.picture_flags.bits.frame_type =
        force_idr ? 0 /*KEY_FRAME*/ : 1 /*INTER_FRAME*/;
    if (vaCreateBuffer(display_, context_id_,
                        VAEncPictureParameterBufferType,
                        sizeof(pic), 1, &pic, &pic_buf) != VA_STATUS_SUCCESS) {
      return false;
    }
    VAEncTileGroupBufferAV1 tg{};
    tg.tg_start = 0;
    tg.tg_end = 0;  // single tile group.
    if (vaCreateBuffer(display_, context_id_,
                        VAEncSliceParameterBufferType,
                        sizeof(tg), 1, &tg, &slice_buf) != VA_STATUS_SUCCESS) {
      return false;
    }
  } else if (webrtc_type_ == webrtc::kVideoCodecVP9) {
    VAEncPictureParameterBufferVP9 pic{};
    pic.coded_buf = coded_buf;
    pic.frame_width_dst = static_cast<uint16_t>(width_);
    pic.frame_height_dst = static_cast<uint16_t>(height_);
    pic.pic_flags.bits.frame_type = force_idr ? 0 : 1;
    if (vaCreateBuffer(display_, context_id_,
                        VAEncPictureParameterBufferType,
                        sizeof(pic), 1, &pic, &pic_buf) != VA_STATUS_SUCCESS) {
      return false;
    }
    VAEncSliceParameterBufferVP9 slice{};
    if (vaCreateBuffer(display_, context_id_,
                        VAEncSliceParameterBufferType,
                        sizeof(slice), 1, &slice, &slice_buf) != VA_STATUS_SUCCESS) {
      return false;
    }
  } else {
    return false;
  }

  VABufferID bufs[2] = {pic_buf, slice_buf};
  if (vaBeginPicture(display_, context_id_,
                      surfaces_[surface_index_]) != VA_STATUS_SUCCESS) {
    return false;
  }
  if (vaRenderPicture(display_, context_id_, bufs, 2) != VA_STATUS_SUCCESS) {
    return false;
  }
  if (vaEndPicture(display_, context_id_) != VA_STATUS_SUCCESS) return false;

  *coded_buf_id_out = coded_buf;
  return true;
}

bool VaapiEncoder::Impl::Encode(
    const uint8_t* y_plane, int y_stride,
    const uint8_t* u_plane, int u_stride,
    const uint8_t* v_plane, int v_stride,
    int width, int height,
    uint64_t pts,
    bool force_idr,
    std::vector<uint8_t>* out,
    bool* is_keyframe) {
  if (display_ == nullptr) return false;
  // 1. Lock the surface and copy the I420 planes in.  Phase 4.5
  //    follow-up: replace this CPU copy with a DMA-buf import from
  //    T55's GpuMemoryBuffer-backed media::VideoFrame.
  VAImage image{};
  if (vaDeriveImage(display_, surfaces_[surface_index_], &image)
        != VA_STATUS_SUCCESS) {
    return false;
  }
  void* mapped = nullptr;
  if (vaMapBuffer(display_, image.buf, &mapped) != VA_STATUS_SUCCESS ||
      !mapped) {
    vaDestroyImage(display_, image.image_id);
    return false;
  }
  uint8_t* dst = static_cast<uint8_t*>(mapped);
  for (int row = 0; row < height; ++row) {
    std::memcpy(dst + image.offsets[0] + row * image.pitches[0],
                y_plane + row * y_stride, width);
  }
  for (int row = 0; row < height / 2; ++row) {
    std::memcpy(dst + image.offsets[1] + row * image.pitches[1],
                u_plane + row * u_stride, width / 2);
    std::memcpy(dst + image.offsets[2] + row * image.pitches[2],
                v_plane + row * v_stride, width / 2);
  }
  vaUnmapBuffer(display_, image.buf);
  vaDestroyImage(display_, image.image_id);

  // 2. Submit picture + slice + drain the coded buffer.
  VABufferID coded_buf = VA_INVALID_ID;
  if (!SubmitPictureAndSlice(pts, force_idr, &coded_buf)) return false;

  if (vaSyncSurface(display_, surfaces_[surface_index_])
        != VA_STATUS_SUCCESS) {
    return false;
  }
  void* coded_mem = nullptr;
  if (vaMapBuffer(display_, coded_buf, &coded_mem) != VA_STATUS_SUCCESS ||
      !coded_mem) {
    vaDestroyBuffer(display_, coded_buf);
    return false;
  }
  // Walk the VACodedBufferSegment linked list — VAAPI emits the
  // bitstream in one or more segments depending on the driver.
  out->clear();
  *is_keyframe = false;
  auto* seg = static_cast<VACodedBufferSegment*>(coded_mem);
  for (; seg; seg = static_cast<VACodedBufferSegment*>(seg->next)) {
    const uint8_t* p = static_cast<const uint8_t*>(seg->buf);
    out->insert(out->end(), p, p + seg->size);
    if (seg->status & VA_CODED_BUF_STATUS_BAD_BITSTREAM) {
      vaUnmapBuffer(display_, coded_buf);
      vaDestroyBuffer(display_, coded_buf);
      return false;
    }
    if (seg->status & VA_CODED_BUF_STATUS_PICTURE_AVE_QP_MASK) {
      // Driver-side AVG QP — ignored here; metric surface lives in
      // the bwe-adapter follow-up (T58 §7).
    }
  }
  vaUnmapBuffer(display_, coded_buf);
  vaDestroyBuffer(display_, coded_buf);

  // libwebrtc's frame-type contract: KEY only on actual IDR. VAAPI
  // exposes that via the AVE-QP segment status; pragmatically, our
  // intra-refresh shape means non-forced frames are never KEY.
  *is_keyframe = force_idr;

  surface_index_ = (surface_index_ + 1) % surfaces_.size();
  return true;
}

bool VaapiEncoder::Impl::Reconfigure(const VaapiEncoderConfig& cfg) {
  if (display_ == nullptr) return false;
  target_bitrate_bps_ = cfg.target_bitrate_bps;
  framerate_ = std::max(1, cfg.framerate);
  // Re-emit the rate-control + framerate misc params on the next
  // sequence boundary. Cheapest correct path: just call
  // SubmitSequenceParams again — drivers accept mid-stream
  // re-issue.
  return SubmitSequenceParams();
}

void VaapiEncoder::Impl::Destroy() {
  if (context_id_ != VA_INVALID_ID && display_) {
    vaDestroyContext(display_, context_id_);
    context_id_ = VA_INVALID_ID;
  }
  if (config_id_ != VA_INVALID_ID && display_) {
    vaDestroyConfig(display_, config_id_);
    config_id_ = VA_INVALID_ID;
  }
  if (!surfaces_.empty() && display_) {
    vaDestroySurfaces(display_, surfaces_.data(),
                       static_cast<int>(surfaces_.size()));
    surfaces_.clear();
  }
  if (display_) {
    vaTerminate(display_);
    display_ = nullptr;
  }
  if (drm_fd_ >= 0) {
    close(drm_fd_);
    drm_fd_ = -1;
  }
}

#endif  // HAS_VAAPI

// ---------------------------------------------------------------------
// VaapiEncoder
// ---------------------------------------------------------------------

VaapiEncoder::VaapiEncoder(VaapiEncoderConfig config)
    : config_(std::move(config)), impl_(std::make_unique<Impl>()) {}

VaapiEncoder::~VaapiEncoder() { Release(); }

bool VaapiEncoder::ProbeAvailable(const std::string& codec_type) {
#if defined(HAS_VAAPI)
  CodecMapping cm;
  if (!ResolveCodecType(codec_type, "42e01f", &cm)) return false;
  Impl probe;
  std::string vendor;
  bool ok = probe.Initialize(VaapiEncoderConfig{.codec_type = codec_type},
                              160, 120, cm.webrtc_type, &vendor);
  probe.Destroy();
  return ok;
#else
  (void)codec_type;
  return false;
#endif
}

int32_t VaapiEncoder::InitEncode(
    const webrtc::VideoCodec* codec_settings,
    const webrtc::VideoEncoder::Settings& /*settings*/) {
  if (!codec_settings || codec_settings->width == 0 ||
      codec_settings->height == 0) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  width_ = codec_settings->width;
  height_ = codec_settings->height;

  CodecMapping cm;
  if (!ResolveCodecType(config_.codec_type, config_.profile_level_id, &cm)) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (!impl_->Initialize(config_, width_, height_, cm.webrtc_type,
                          &vendor_string_)) {
    RTC_LOG(LS_ERROR) << "VaapiEncoder::Initialize failed for "
                      << config_.codec_type;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  initialized_ = true;
  frames_in_ = 0;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t VaapiEncoder::Encode(
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
  CodecMapping cm;
  ResolveCodecType(config_.codec_type, config_.profile_level_id, &cm);
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

int32_t VaapiEncoder::RegisterEncodeCompleteCallback(
    webrtc::EncodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t VaapiEncoder::Release() {
  if (impl_) impl_->Destroy();
  initialized_ = false;
  callback_ = nullptr;
  return WEBRTC_VIDEO_CODEC_OK;
}

void VaapiEncoder::SetRates(const RateControlParameters& parameters) {
  if (!initialized_) return;
  config_.target_bitrate_bps =
      static_cast<int>(parameters.bitrate.get_sum_bps());
  config_.framerate =
      std::max(1, static_cast<int>(parameters.framerate_fps));
  if (!impl_->Reconfigure(config_)) {
    RTC_LOG(LS_WARNING) << "VaapiEncoder::Reconfigure failed";
  }
}

webrtc::VideoEncoder::EncoderInfo VaapiEncoder::GetEncoderInfo() const {
  EncoderInfo info;
  std::string name = std::string("cloud-browser-vaapi-") + config_.codec_type;
  if (!vendor_string_.empty()) {
    name += "-";
    // Squeeze the vendor string into a stable token (drop spaces).
    for (char c : vendor_string_) {
      if (c != ' ') name += static_cast<char>(::tolower(c));
    }
  }
  if (config_.low_latency_tag) name += "-lowlatency";
  info.implementation_name = name;
  info.is_hardware_accelerated = true;
  info.supports_native_handle = false;
  info.supports_simulcast = false;
  info.has_trusted_rate_controller = true;
  return info;
}

}  // namespace cloud_browser

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// See video_frame_conversion.h for the design.

#include "capture/framesink-capturer/video_frame_conversion.h"

#include <atomic>
#include <utility>

#include "base/check.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "media/base/video_types.h"
#include "third_party/libyuv/include/libyuv/convert.h"
#include "ui/gfx/geometry/rect.h"

// webrtc-side helpers — visible via the patches/0003 passthrough.
#include "api/make_ref_counted.h"
#include "api/video/color_space.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

namespace cloud_browser {

namespace {

// File-scope state for RebaseMediaTimestampToWebrtcMicros — the
// captured cross-clock delta. Initialized lazily on first call.
//
// Using std::atomic with memory_order_relaxed is sufficient here:
//   * Single-writer (the first call); subsequent callers observe the
//     same value monotonically once it's written.
//   * The "first call wins" race is benign — competing callers each
//     compute essentially the same delta within microseconds, and
//     whichever store lands first is the one used permanently.
//   * No ordering with other memory is required; the value itself is
//     the only thing read.
std::atomic<int64_t> g_clock_delta_us{0};
std::atomic<bool> g_clock_delta_set{false};

// Returns true if |format| is one of the two pixel formats R2 supports.
bool IsSupportedPixelFormat(media::VideoPixelFormat format) {
  return format == media::PIXEL_FORMAT_NV12 ||
         format == media::PIXEL_FORMAT_I420;
}

}  // namespace

// ===================================================================
// CloudBrowserMediaVideoFrameNV12Buffer
// ===================================================================

// static
webrtc::scoped_refptr<CloudBrowserMediaVideoFrameNV12Buffer>
CloudBrowserMediaVideoFrameNV12Buffer::Create(
    scoped_refptr<media::VideoFrame> frame) {
  if (!frame) {
    return nullptr;
  }
  if (frame->format() != media::PIXEL_FORMAT_NV12) {
    return nullptr;
  }
  const gfx::Rect visible = frame->visible_rect();
  if (visible.IsEmpty()) {
    return nullptr;
  }
  // webrtc::make_ref_counted handles the protected-ctor + refcount-base
  // wiring; this is the canonical libwebrtc construction pattern (see
  // api/make_ref_counted.h).
  return webrtc::make_ref_counted<CloudBrowserMediaVideoFrameNV12Buffer>(
      std::move(frame));
}

CloudBrowserMediaVideoFrameNV12Buffer::CloudBrowserMediaVideoFrameNV12Buffer(
    scoped_refptr<media::VideoFrame> frame)
    : frame_(std::move(frame)),
      width_(frame_->visible_rect().width()),
      height_(frame_->visible_rect().height()) {
  DCHECK(frame_);
  DCHECK_EQ(frame_->format(), media::PIXEL_FORMAT_NV12);
}

CloudBrowserMediaVideoFrameNV12Buffer::
    ~CloudBrowserMediaVideoFrameNV12Buffer() = default;

int CloudBrowserMediaVideoFrameNV12Buffer::width() const {
  return width_;
}

int CloudBrowserMediaVideoFrameNV12Buffer::height() const {
  return height_;
}

const uint8_t* CloudBrowserMediaVideoFrameNV12Buffer::DataY() const {
  // visible_data(plane) returns the pointer offset to the visible_rect
  // origin, which is exactly what webrtc expects on the plane
  // accessors (libwebrtc treats the buffer as visible-sized).
  return frame_->visible_data(media::VideoFrame::Plane::kY);
}

const uint8_t* CloudBrowserMediaVideoFrameNV12Buffer::DataUV() const {
  return frame_->visible_data(media::VideoFrame::Plane::kUV);
}

int CloudBrowserMediaVideoFrameNV12Buffer::StrideY() const {
  return frame_->stride(media::VideoFrame::Plane::kY);
}

int CloudBrowserMediaVideoFrameNV12Buffer::StrideUV() const {
  return frame_->stride(media::VideoFrame::Plane::kUV);
}

webrtc::scoped_refptr<webrtc::I420BufferInterface>
CloudBrowserMediaVideoFrameNV12Buffer::ToI420() {
  // Allocate a fresh I420 buffer at this wrapper's visible dimensions
  // and convert via libyuv. webrtc::I420Buffer::Create allocates
  // tightly-strided contiguous planes.
  webrtc::scoped_refptr<webrtc::I420Buffer> i420 =
      webrtc::I420Buffer::Create(width_, height_);
  if (!i420) {
    return nullptr;
  }

  // libyuv::NV12ToI420 returns 0 on success; non-zero is "invalid args"
  // (libyuv has no error sub-codes). If this ever fails in production
  // it indicates an internal invariant violation (e.g. negative
  // stride) — log loudly so triage doesn't have to guess.
  const int rv = libyuv::NV12ToI420(
      DataY(), StrideY(),
      DataUV(), StrideUV(),
      i420->MutableDataY(), i420->StrideY(),
      i420->MutableDataU(), i420->StrideU(),
      i420->MutableDataV(), i420->StrideV(),
      width_, height_);
  if (rv != 0) {
    RTC_LOG(LS_ERROR) << "NV12ToI420 failed: rv=" << rv
                      << " dims=" << width_ << "x" << height_;
    return nullptr;
  }

  return i420;
}

// ===================================================================
// CloudBrowserMediaVideoFrameI420Buffer
// ===================================================================

// static
webrtc::scoped_refptr<CloudBrowserMediaVideoFrameI420Buffer>
CloudBrowserMediaVideoFrameI420Buffer::Create(
    scoped_refptr<media::VideoFrame> frame) {
  if (!frame) {
    return nullptr;
  }
  if (frame->format() != media::PIXEL_FORMAT_I420) {
    return nullptr;
  }
  const gfx::Rect visible = frame->visible_rect();
  if (visible.IsEmpty()) {
    return nullptr;
  }
  return webrtc::make_ref_counted<CloudBrowserMediaVideoFrameI420Buffer>(
      std::move(frame));
}

CloudBrowserMediaVideoFrameI420Buffer::CloudBrowserMediaVideoFrameI420Buffer(
    scoped_refptr<media::VideoFrame> frame)
    : frame_(std::move(frame)),
      width_(frame_->visible_rect().width()),
      height_(frame_->visible_rect().height()) {
  DCHECK(frame_);
  DCHECK_EQ(frame_->format(), media::PIXEL_FORMAT_I420);
}

CloudBrowserMediaVideoFrameI420Buffer::
    ~CloudBrowserMediaVideoFrameI420Buffer() = default;

int CloudBrowserMediaVideoFrameI420Buffer::width() const {
  return width_;
}

int CloudBrowserMediaVideoFrameI420Buffer::height() const {
  return height_;
}

const uint8_t* CloudBrowserMediaVideoFrameI420Buffer::DataY() const {
  return frame_->visible_data(media::VideoFrame::Plane::kY);
}

const uint8_t* CloudBrowserMediaVideoFrameI420Buffer::DataU() const {
  return frame_->visible_data(media::VideoFrame::Plane::kU);
}

const uint8_t* CloudBrowserMediaVideoFrameI420Buffer::DataV() const {
  return frame_->visible_data(media::VideoFrame::Plane::kV);
}

int CloudBrowserMediaVideoFrameI420Buffer::StrideY() const {
  return frame_->stride(media::VideoFrame::Plane::kY);
}

int CloudBrowserMediaVideoFrameI420Buffer::StrideU() const {
  return frame_->stride(media::VideoFrame::Plane::kU);
}

int CloudBrowserMediaVideoFrameI420Buffer::StrideV() const {
  return frame_->stride(media::VideoFrame::Plane::kV);
}

// I420BufferInterface inherits ToI420() returning |this| (the default
// I420 path is a no-op identity). Confirmed by inspection of
// api/video/i420_buffer.h: the base provides a non-virtual GetI420
// + the VideoFrameBuffer::ToI420 default that returns self for I420
// buffer types. No override needed here.

// ===================================================================
// WrapMediaVideoFrameAsWebrtcFrame
// ===================================================================

absl::optional<webrtc::VideoFrame> WrapMediaVideoFrameAsWebrtcFrame(
    scoped_refptr<media::VideoFrame> media_frame,
    VideoFrameConversionStats* stats) {
  if (!media_frame) {
    // Defensive — capturer.cc should never deliver null, but if it
    // does, we don't count it (no format to attribute the drop to).
    return absl::nullopt;
  }

  const gfx::Rect visible = media_frame->visible_rect();
  if (visible.IsEmpty()) {
    if (stats) {
      ++stats->frames_dropped_empty_visible_rect;
    }
    RTC_LOG(LS_WARNING)
        << "WrapMediaVideoFrameAsWebrtcFrame: empty visible_rect";
    return absl::nullopt;
  }

  if (!IsSupportedPixelFormat(media_frame->format())) {
    if (stats) {
      ++stats->frames_dropped_unknown_format;
    }
    RTC_LOG(LS_WARNING)
        << "WrapMediaVideoFrameAsWebrtcFrame: unsupported pixel format = "
        << media::VideoPixelFormatToString(media_frame->format());
    return absl::nullopt;
  }

  // Capture the timestamp BEFORE we move media_frame into the buffer
  // wrapper — base::TimeDelta is trivially copyable, but the call site
  // is clearer this way.
  const base::TimeDelta media_ts = media_frame->timestamp();

  // TODO(M2-R2-color-space): media::VideoFrame::ColorSpace() returns a
  // gfx::ColorSpace; libwebrtc's webrtc::ColorSpace has a different
  // shape and there's no direct conversion utility exported. Renderer-
  // side WebRTC has helpers in media/capture/video_capturer_source.h
  // that do this rebasing, but they aren't in the passthrough yet.
  // For first-light we leave color_space unset (libwebrtc treats this
  // as "unknown — encoder uses sane default"), which matches what the
  // renderer-side path did for opaque-tab captures. R3 / a follow-up
  // can wire the proper conversion once the helper is in the
  // passthrough.

  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer;
  switch (media_frame->format()) {
    case media::PIXEL_FORMAT_NV12:
      buffer = CloudBrowserMediaVideoFrameNV12Buffer::Create(
          std::move(media_frame));
      if (buffer && stats) {
        ++stats->frames_wrapped_nv12;
      }
      break;
    case media::PIXEL_FORMAT_I420:
      buffer = CloudBrowserMediaVideoFrameI420Buffer::Create(
          std::move(media_frame));
      if (buffer && stats) {
        ++stats->frames_wrapped_i420;
      }
      break;
    default:
      // Already filtered by IsSupportedPixelFormat above; unreachable.
      NOTREACHED();
  }

  if (!buffer) {
    // Create() rejected the frame (post-IsSupportedPixelFormat — must
    // be a degenerate frame that slipped through). Count under the
    // closest matching bucket; "unknown_format" is the catch-all.
    if (stats) {
      ++stats->frames_dropped_unknown_format;
    }
    return absl::nullopt;
  }

  // Builder pattern is the canonical libwebrtc construction path for
  // VideoFrame (see api/video/video_frame.h::Builder). Avoids the
  // older multi-arg constructor whose param order has changed across
  // libwebrtc versions.
  webrtc::VideoFrame frame =
      webrtc::VideoFrame::Builder()
          .set_video_frame_buffer(std::move(buffer))
          .set_rotation(webrtc::kVideoRotation_0)
          .set_timestamp_us(RebaseMediaTimestampToWebrtcMicros(media_ts))
          .build();
  return frame;
}

// ===================================================================
// RebaseMediaTimestampToWebrtcMicros
// ===================================================================

int64_t RebaseMediaTimestampToWebrtcMicros(base::TimeDelta media_ts) {
  const int64_t media_us = media_ts.InMicroseconds();

  // First-call path: capture the delta between rtc::TimeMicros() and
  // the media-frame clock.
  if (!g_clock_delta_set.load(std::memory_order_acquire)) {
    const int64_t now_rtc_us = webrtc::TimeMicros();
    const int64_t delta = now_rtc_us - media_us;
    // exchange-if-not-set so concurrent first calls converge on one
    // value. Either of them stores; whoever wins is fine — both deltas
    // are within microseconds of each other.
    bool expected = false;
    if (g_clock_delta_set.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      g_clock_delta_us.store(delta, std::memory_order_release);
      return now_rtc_us;
    }
    // Lost the race; fall through to read the established delta.
  }

  return media_us + g_clock_delta_us.load(std::memory_order_acquire);
}

}  // namespace cloud_browser

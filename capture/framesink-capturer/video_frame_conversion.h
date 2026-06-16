// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// media::VideoFrame -> webrtc::VideoFrame conversion + lifetime-pinning
// VideoFrameBuffer wrappers.
//
// Module M2 R2 (CV2-37) of the ChromelessV2 native-peer migration.
// Bridges CloudBrowserFrameSinkCapturer (capturer.h — produces
// scoped_refptr<media::VideoFrame> over the Viz Mojo seam) to
// libwebrtc's track-source contract (R3 — VideoTrackSource publishes
// webrtc::VideoFrame to the PCF).
//
// What this file owns:
//
//   1. CloudBrowserMediaVideoFrameNV12Buffer
//   2. CloudBrowserMediaVideoFrameI420Buffer
//      Two webrtc::VideoFrameBuffer subclasses (one per pixel format the
//      capturer emits in M2 scope — see Non-goals below). Each holds a
//      scoped_refptr<media::VideoFrame>, so the underlying memory + the
//      capturer's BufferHandleScope stay alive while libwebrtc reads
//      Y/U/V plane pointers directly. No pixel copy on construction.
//      Refcount-driven cleanup: when libwebrtc (encoder, RTP sender,
//      whatever holds the last ref) releases the buffer, the
//      media::VideoFrame ref drops, capturer.cc's BufferHandleScope
//      destructor fires, and the Mojo Done() ack returns the buffer to
//      Viz's pool. End-to-end zero-leak by construction.
//
//   3. WrapMediaVideoFrameAsWebrtcFrame(media_frame)
//      The frame-level conversion. Wraps the lifetime-pinning buffer +
//      propagates timestamp / rotation / color-space onto a
//      webrtc::VideoFrame value. Returns absl::nullopt on:
//        * unknown / unsupported pixel format (counter incremented via
//          out-param); R2 supports I420 (capturer default,
//          PIXEL_FORMAT_I420) and NV12 (PIXEL_FORMAT_NV12) only.
//        * zero-area visible_rect (degenerate frame; counter incremented).
//
//   4. RebaseMediaTimestampToWebrtcMicros(media_ts)
//      Pure helper exposed for testing. Converts a media::VideoFrame
//      timestamp (capturer monotonic base, base::TimeDelta) to a
//      webrtc-side rtc::TimeMicros() epoch. The first call captures the
//      delta between the two clocks; subsequent calls reuse it so the
//      output stream is monotonic + ordered as long as the input is.
//
// Lifetime contract (LOAD-BEARING — Plane CV2-37 acceptance §last
// bullet: "assert source media::VideoFrame ref held while webrtc frame
// alive and released exactly when it drops"):
//
//   media::VideoFrame::AddDestructionObserver in capturer.cc binds the
//   BufferHandleScope to the frame's release. THIS file's buffer
//   subclasses hold a scoped_refptr<media::VideoFrame>, so the
//   destruction observer doesn't fire until libwebrtc drops the buffer
//   AND nothing else holds the media frame. The two refcounts (libwebrtc
//   buffer refcount on the OUR wrapper, capturer-side
//   scoped_refptr<media::VideoFrame> on the wrapper) chain together —
//   one frame in, one Done() out.
//
// Non-goals (per Plane CV2-37):
//   * GPU-memory-buffer zero-copy. capturer.cc's GMB branch is #if 0 /
//     T17 unreachable until MappableSharedImage is wired; even when it
//     ships, R2 takes the shmem-backed path and leaves the GMB lane to
//     a future R#.
//   * Scaling / cropping. The sink (R3 — CloudBrowserFrameSinkVideo
//     TrackSource) handles adapter-side scaling via libwebrtc's
//     VideoAdapter.
//   * Encoder interaction. R2 produces a frame; the encoder factory (M1)
//     consumes it via the standard libwebrtc encode path.
//   * Audio. M5.5.
//
// Cross-references:
//   * capture/framesink-capturer/capturer.h            (producer)
//   * patches/0003-add-cloud-browser-webrtc-overrides.patch (M2 R1
//                                                       widened deps)
//   * third_party/webrtc/api/video/video_frame_buffer.h (base class)
//   * third_party/webrtc/api/video/nv12_buffer.h        (interface)
//   * third_party/webrtc/api/video/i420_buffer.h        (interface)
//   * third_party/libyuv/include/libyuv/convert.h       (NV12 -> I420)

#ifndef CAPTURE_FRAMESINK_CAPTURER_VIDEO_FRAME_CONVERSION_H_
#define CAPTURE_FRAMESINK_CAPTURER_VIDEO_FRAME_CONVERSION_H_

#include <cstdint>

#include "base/memory/scoped_refptr.h"
#include "media/base/video_frame.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

// webrtc public-API headers — routed through M2 R1's widened
// patches/0003 passthrough. Direct includes are correct here because
// gn check resolves visibility via the public_deps chain on the
// :framesink_capture source_set's eventual :webrtc_api_passthrough dep
// (added in this R# — see BUILD.gn change below).
#include "api/scoped_refptr.h"
#include "api/make_ref_counted.h"
#include "api/video/i420_buffer.h"
// rtc_base/ref_counted_object.h is included so the
// `friend class webrtc::RefCountedObject` declarations in the buffer
// classes below compile cleanly. It's also indirectly available via
// api/make_ref_counted.h, but the explicit include documents intent
// and shields against transitive-include shuffling in upstream.
#include "rtc_base/ref_counted_object.h"
#include "api/video/nv12_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/video_rotation.h"

namespace cloud_browser {

// Counters surfaced to the caller (R3's CloudBrowserFrameSinkVideoTrack
// Source aggregates these into its own stats struct). Kept local to
// this TU so this file has no dependency on R3 yet.
//
// Increment-only; reset is the caller's responsibility. ALL fields are
// monotonic; divergence between frames_wrapped and frames_dropped sums
// vs. caller's input count is the leak signal.
struct VideoFrameConversionStats {
  uint64_t frames_wrapped_nv12 = 0;
  uint64_t frames_wrapped_i420 = 0;
  uint64_t frames_dropped_unknown_format = 0;
  uint64_t frames_dropped_empty_visible_rect = 0;
};

// Lifetime-pinning NV12 buffer wrapper.
//
// Implements webrtc::NV12BufferInterface by exposing the underlying
// media::VideoFrame's Y / UV plane pointers + strides. Construction
// captures a scoped_refptr<media::VideoFrame>; the source frame stays
// alive for the wrapper's entire libwebrtc-side lifetime.
//
// IMPORTANT — the source media::VideoFrame MUST have format
// PIXEL_FORMAT_NV12 and a non-empty visible_rect. Create() returns
// nullptr otherwise.
//
// Thread-safety: webrtc::VideoFrameBuffer is refcounted thread-safe by
// contract (see api/video/video_frame_buffer.h). Plane pointer reads
// are safe from any thread; the underlying media::VideoFrame's memory
// is immutable post-capture.
class CloudBrowserMediaVideoFrameNV12Buffer
    : public webrtc::NV12BufferInterface {
 public:
  // Factory. Returns nullptr if |frame| is null, not NV12, or has an
  // empty visible_rect.
  static webrtc::scoped_refptr<CloudBrowserMediaVideoFrameNV12Buffer> Create(
      scoped_refptr<media::VideoFrame> frame);

  // webrtc::VideoFrameBuffer (via NV12BufferInterface):
  int width() const override;
  int height() const override;
  webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override;

  // webrtc::NV12BufferInterface:
  const uint8_t* DataY() const override;
  const uint8_t* DataUV() const override;
  int StrideY() const override;
  int StrideUV() const override;

  CloudBrowserMediaVideoFrameNV12Buffer(
      const CloudBrowserMediaVideoFrameNV12Buffer&) = delete;
  CloudBrowserMediaVideoFrameNV12Buffer& operator=(
      const CloudBrowserMediaVideoFrameNV12Buffer&) = delete;

 protected:
  ~CloudBrowserMediaVideoFrameNV12Buffer() override;

 private:
  // Construction is via Create(); libwebrtc's
  // `webrtc::make_ref_counted<T>(args)` instantiates
  // `webrtc::RefCountedObject<T>` which subclasses T and needs ctor
  // access. The friend declaration grants exactly that without
  // making the ctor public — preserves the factory-only invariant.
  template <class T>
  friend class webrtc::RefCountedObject;

  explicit CloudBrowserMediaVideoFrameNV12Buffer(
      scoped_refptr<media::VideoFrame> frame);

  // The pinned frame. Outlives this wrapper. Released exactly when
  // libwebrtc drops its last ref to *this and our refcount hits zero,
  // which is when the capturer's BufferHandleScope destructor fires
  // and Mojo Done() acks the buffer back to Viz.
  const scoped_refptr<media::VideoFrame> frame_;

  // Cached dims (visible_rect()) so the hot path on width()/height()
  // is one load, not a virtual call into media::VideoFrame.
  const int width_;
  const int height_;
};

// Lifetime-pinning I420 buffer wrapper.
//
// Same shape as the NV12 variant above but implements
// webrtc::I420BufferInterface. Used when the capturer is configured
// with PIXEL_FORMAT_I420 (capturer_test.cc default — see Plane CV2-37
// "capturer honors info.pixel_format; test sends I420").
class CloudBrowserMediaVideoFrameI420Buffer
    : public webrtc::I420BufferInterface {
 public:
  // Factory. Returns nullptr if |frame| is null, not I420, or has an
  // empty visible_rect.
  static webrtc::scoped_refptr<CloudBrowserMediaVideoFrameI420Buffer> Create(
      scoped_refptr<media::VideoFrame> frame);

  // webrtc::VideoFrameBuffer (via I420BufferInterface):
  int width() const override;
  int height() const override;

  // webrtc::I420BufferInterface:
  const uint8_t* DataY() const override;
  const uint8_t* DataU() const override;
  const uint8_t* DataV() const override;
  int StrideY() const override;
  int StrideU() const override;
  int StrideV() const override;

  CloudBrowserMediaVideoFrameI420Buffer(
      const CloudBrowserMediaVideoFrameI420Buffer&) = delete;
  CloudBrowserMediaVideoFrameI420Buffer& operator=(
      const CloudBrowserMediaVideoFrameI420Buffer&) = delete;

 protected:
  ~CloudBrowserMediaVideoFrameI420Buffer() override;

 private:
  // Same friend-of-RefCountedObject pattern as the NV12 sibling above.
  template <class T>
  friend class webrtc::RefCountedObject;

  explicit CloudBrowserMediaVideoFrameI420Buffer(
      scoped_refptr<media::VideoFrame> frame);

  const scoped_refptr<media::VideoFrame> frame_;
  const int width_;
  const int height_;
};

// Convert a media::VideoFrame to a webrtc::VideoFrame.
//
// Selects the right lifetime-pinning buffer subclass by inspecting
// |media_frame|->format(), wraps it, and stamps the resulting
// webrtc::VideoFrame with:
//   * id              — auto-incremented per converter instance (not
//                       used yet; libwebrtc populates RTP sequence
//                       numbers downstream). For R2, we leave id=0 so
//                       downstream behaviour is identical to renderer-
//                       side capture; R3 can swap in a counter if a
//                       deterministic id is needed for trace joining.
//   * timestamp_us    — RebaseMediaTimestampToWebrtcMicros(
//                         media_frame->timestamp()) — see helper below.
//   * rotation        — kVideoRotation_0 (capturer has no rotation
//                       concept; the Aura surface is captured as-is
//                       and any rotation is handled by the embedder
//                       browser content, not the capture pipeline).
//   * color_space     — propagated from media::VideoFrame::ColorSpace()
//                       when set; nullopt-equivalent otherwise.
//
// On unsupported pixel format or zero-area visible_rect: returns
// absl::nullopt and increments the matching counter on |stats|.
// |stats| may be null (counters discarded).
absl::optional<webrtc::VideoFrame> WrapMediaVideoFrameAsWebrtcFrame(
    scoped_refptr<media::VideoFrame> media_frame,
    VideoFrameConversionStats* stats);

// Rebase a media::VideoFrame timestamp to webrtc's rtc::TimeMicros()
// epoch.
//
// media::VideoFrame::timestamp() is a base::TimeDelta from the
// capturer's monotonic base (Viz uses CAPTURE_END_TIME by default —
// see media/base/video_frame_metadata.h). libwebrtc's
// webrtc::VideoFrame::set_timestamp_us takes a microsecond value on
// the rtc::TimeMicros() clock (a monotonic system clock).
//
// On the first call, captures the delta between the two clocks and
// uses it for every subsequent call. The delta is intentionally
// captured per-process (file-scope), so:
//   * the first frame's timestamp_us == rtc::TimeMicros() at that
//     moment (gives a sensible glass-to-glass measurement origin);
//   * subsequent frames are monotonically ordered as long as
//     |media_ts| is monotonic (which the capturer guarantees — see
//     capturer.cc's reliance on CAPTURE_END_TIME).
//
// Exposed in the header purely for unit-testability (R2 acceptance
// "timestamps monotonic+ordered" can pin behaviour without booting
// the full converter).
//
// TODO(M2-R2-monoclock): if a sub-millisecond clock skew is ever
// observed in production (the two clocks ARE both monotonic, but their
// epochs may differ by ~100us per restart), revisit whether to
// recompute the delta periodically. For first-light the once-per-
// process capture is sufficient.
int64_t RebaseMediaTimestampToWebrtcMicros(base::TimeDelta media_ts);

}  // namespace cloud_browser

#endif  // CAPTURE_FRAMESINK_CAPTURER_VIDEO_FRAME_CONVERSION_H_

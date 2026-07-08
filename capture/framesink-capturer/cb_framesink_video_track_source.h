// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserFrameSinkVideoTrackSource — the libwebrtc-side video
// track source that publishes captured frames from
// CloudBrowserFrameSinkCapturer (capturer.h — the Viz Mojo consumer)
// to attached webrtc::VideoSinkInterface<webrtc::VideoFrame> consumers
// (PCF tracks → RtpSender → encoder factory → outbound RTP).
//
// Module M2 R3 (Plane CV2-38) of the ChromelessV2 native-peer migration.
// Sits between R2 (video_frame_conversion.h — media::VideoFrame →
// webrtc::VideoFrame wrapping + lifetime pinning) and M3 (the signaling
// + PeerConnection construction that creates a webrtc::VideoTrack out
// of this source).
//
// Shape (per Plane CV2-38 Intent + Scope):
//
//   * Subclasses webrtc::VideoTrackSource (= NotifierInterface<
//     VideoTrackSourceInterface> + the SourceInterface<VideoFrame>
//     forwarding) and owns a webrtc::VideoBroadcaster as the actual
//     fanout. webrtc::VideoTrackSource::source() returns a pointer
//     into the broadcaster so AddOrUpdateSink/RemoveSink calls on the
//     interface are forwarded to broadcaster_ automatically by the
//     base class.
//
//   * Owns the CloudBrowserFrameSinkCapturer it ingests from (via
//     std::unique_ptr — capturer is sequence-affine chromium code,
//     the track source is webrtc-side refcounted; the unique_ptr
//     lives on the chromium-side capturer sequence).
//
//   * Created via webrtc::make_ref_counted<…>(…). Refcount semantics
//     match the standard webrtc track-source contract: the PCF holds
//     the source via the track it creates; outliving consumers means
//     no early teardown.
//
//   * is_screencast() / needs_denoising() values are M3-coupled (the
//     M3 peer-track contract decides what hints to surface to the
//     encoder factory's GetEncoderInfo()). R3 ships the safe defaults
//     (is_screencast=true since this IS a tab-content capture in
//     practice; needs_denoising=nullopt) and a TODO marker so M3 R#
//     flips the explicit value when the peer-track contract lands.
//
// Threading model (LOAD-BEARING — Plane CV2-38 Scope §"Document the
// capturer-sequence vs libwebrtc-worker-thread model (no UAF)"):
//
//   * The capturer's OnFrameCallback fires on the capturer's chromium
//     sequence (whatever sequence Start() was called on — usually the
//     embedder browser-main thread per M1's CloudBrowserBrowserMainParts
//     wiring). OnCapturerFrame runs there.
//
//   * AddOrUpdateSink / RemoveSink may be called from libwebrtc's
//     worker thread (the PCF's worker_thread_ from M1's
//     CreateCloudBrowserPcf). webrtc::VideoBroadcaster is internally
//     thread-safe via its own mutex (see media/base/video_broadcaster.h);
//     no extra synchronization needed on our side.
//
//   * broadcaster_.OnFrame(wf) fans out synchronously to all attached
//     sinks UNDER the broadcaster's lock — so by the time OnFrame
//     returns, every sink has either copied the frame's pixels or
//     extended the buffer refcount (libwebrtc encoder path takes the
//     latter). The webrtc::VideoFrame value we pass dies on this stack
//     frame; the underlying scoped_refptr<media::VideoFrame> survives
//     iff some sink kept the buffer alive.
//
//   * UAF avoidance: capturer_ is owned by us. Destruction order
//     (members destruct in REVERSE declaration order) ensures
//     capturer_ destructs FIRST — the capturer's mojo::Receiver
//     destructor drops the binding, no further callbacks can fire.
//     After that, broadcaster_ + stats_ can be destroyed safely.
//
// Backpressure consequence (LOAD-BEARING — Plane CV2-38 Scope §"the
// backpressure consequence (held webrtc frame = held Viz pool slot)"):
//
//   The webrtc::VideoFrame this source publishes wraps a
//   CloudBrowserMediaVideoFrame{NV12,I420}Buffer (R2). That buffer
//   holds a scoped_refptr<media::VideoFrame>, which in turn holds
//   the capturer's BufferHandleScope (a refcounted RAII guard whose
//   destructor posts/binds Mojo Done() on the capture sequence). If a
//   downstream sink (encoder, RTP sender) holds the webrtc::VideoFrame
//   for longer than a single OnFrame fanout, it ALSO holds the Viz
//   capture-pool slot the frame came from. Viz's pool has a finite
//   size (set by the producer's args.buffer_count + content_rect);
//   if every slot is held, the producer drops new frames at the
//   FrameSinkVideoCapturer level (visible in the capturer's
//   FrameSinkCapturerStats.frames_dropped_by_capturer counter).
//
//   This is BY DESIGN: if the encoder is slow, we want backpressure
//   to surface as producer-side drops, not as our process growing
//   unbounded buffers. The OnFrame fanout pattern preserves this:
//   we don't queue, we don't copy, we hand the frame to sinks and
//   move on.
//
// Acceptance criteria (Plane CV2-38 M2 UAT — exercised by
// cb_framesink_video_track_source_test.cc once R6 lands):
//
//   1. Drive mock frames through the capturer OnFrameCallback at
//      known FPS; attach a mock VideoSinkInterface<webrtc::VideoFrame>;
//      assert expected count + resolution + pixel format on every
//      OnFrame invocation.
//
//   2. Assert Done() fires per frame after the sink releases the
//      webrtc::VideoFrame (sink-side ref drop → buffer wrapper ref
//      drop → media::VideoFrame ref drop → BufferHandleScope dtor →
//      capture-sequence Mojo Done()). No pool leak after N frames in
//      steady state.
//
// Non-goals (per Plane CV2-38 — explicitly OUT of R3 scope):
//   * signaling / PeerConnection / track creation — M3.
//   * construction / ownership site (where the source is instantiated
//     in the browser-process lifecycle) — R4.
//   * capture start / stop policy (when to call capturer_->Start /
//     ->Stop based on sink-attachment count) — R5.
//   * audio — M5.5.
//
// Cross-references:
//   * capture/framesink-capturer/capturer.h            (producer)
//   * capture/framesink-capturer/video_frame_conversion.h (R2 helpers)
//   * patches/0003-add-cloud-browser-webrtc-overrides.patch (M2 R1
//     widened webrtc passthrough — adds pc:video_track_source +
//     api:media_stream_interface)
//   * third_party/webrtc/pc/video_track_source.h       (base class)
//   * third_party/webrtc/media/base/video_broadcaster.h (fanout)
//   * third_party/webrtc/api/media_stream_interface.h  (sink iface)

#ifndef CAPTURE_FRAMESINK_CAPTURER_CB_FRAMESINK_VIDEO_TRACK_SOURCE_H_
#define CAPTURE_FRAMESINK_CAPTURER_CB_FRAMESINK_VIDEO_TRACK_SOURCE_H_

#include <cstdint>
#include <memory>

#include "base/memory/scoped_refptr.h"
#include "base/sequence_checker.h"
#include "capture/framesink-capturer/capturer.h"
#include "capture/framesink-capturer/video_frame_conversion.h"
#include "media/base/video_frame.h"

// webrtc public-API headers — routed through M2 R1's widened
// patches/0003 passthrough (pc:video_track_source +
// api:media_stream_interface have been added there).
#include "api/scoped_refptr.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "media/base/video_broadcaster.h"
#include "pc/video_track_source.h"
#include "rtc_base/system/rtc_export.h"

namespace cloud_browser {

// Stats surfaced to callers (M3 R# will scrape these alongside
// FrameSinkCapturerStats + VideoFrameConversionStats for the metrics
// sidecar). Monotonic; reset is the caller's responsibility.
//
// Invariants in steady state (no shutdown, no encoder backpressure):
//   frames_received_from_capturer == frames_published_to_sinks
//                                  + frames_dropped_conversion_failed
struct CloudBrowserFrameSinkVideoTrackSourceStats {
  // Frames delivered by capturer's OnFrameCallback into our
  // OnCapturerFrame ingress.
  uint64_t frames_received_from_capturer = 0;

  // Frames successfully converted (R2 returned a webrtc::VideoFrame)
  // AND handed to broadcaster_.OnFrame(). Equals
  // (frames_received_from_capturer - frames_dropped_conversion_failed)
  // unless something is leaking.
  uint64_t frames_published_to_sinks = 0;

  // R2 returned absl::nullopt — unsupported pixel format, empty
  // visible_rect, etc. The capturer's media::VideoFrame is dropped on
  // our side immediately, so the BufferHandleScope dtor fires and
  // Mojo Done() acks; no pool leak from this path.
  uint64_t frames_dropped_conversion_failed = 0;
};

// The libwebrtc-side video track source.
//
// Refcounted (webrtc::VideoTrackSource → NotifierInterface chain).
// Create via webrtc::make_ref_counted<…>(std::move(capturer)). The
// capturer's OnFrameCallback is rebound to OnCapturerFrame inside
// the constructor so the caller does NOT pre-bind a callback.
class CloudBrowserFrameSinkVideoTrackSource : public webrtc::VideoTrackSource {
 public:
  // Factory. The caller hands us a constructed (but not yet Started)
  // CloudBrowserFrameSinkCapturer; we rebind its OnFrameCallback to
  // route into OnCapturerFrame and take ownership.
  //
  // Constructor is public so webrtc::make_ref_counted<…> can reach it.
  // Prefer the factory in test / production code:
  //
  //   auto src = webrtc::make_ref_counted<
  //       CloudBrowserFrameSinkVideoTrackSource>(std::move(capturer));
  //
  // Capture start/stop is NOT triggered here — that's R5's lifecycle
  // policy. Caller (R4 / R5) drives capturer_->Configure +
  // capturer_->Start at the appropriate moment.
  explicit CloudBrowserFrameSinkVideoTrackSource(
      std::unique_ptr<CloudBrowserFrameSinkCapturer> capturer);

  CloudBrowserFrameSinkVideoTrackSource(
      const CloudBrowserFrameSinkVideoTrackSource&) = delete;
  CloudBrowserFrameSinkVideoTrackSource& operator=(
      const CloudBrowserFrameSinkVideoTrackSource&) = delete;

  // webrtc::VideoTrackSourceInterface (via VideoTrackSource):
  //
  // state() = kLive — VideoTrackSource's default behaviour after
  // construction; we never call SetState(kEnded) ourselves. R5's
  // capture lifecycle will decide whether kMuted needs surfacing
  // when capturer_ is stopped (currently: not surfaced; sinks just
  // see no OnFrame calls).
  //
  // remote() = false — passed through the VideoTrackSource(remote=false)
  // base constructor below. We are a local source (capture-side),
  // never a remote-RTP-receive source.
  //
  // is_screencast() — Tab-content capture from a chromium FrameSink
  // IS conceptually screencast (the encoder factory uses this to
  // bias toward keyframe-frequent / detail-preserving settings).
  // TODO(M2-R3-m3-coupling): M3's peer-track contract may flip this
  // per-track based on what the track is FOR (e.g. share-screen vs.
  // ambient video). For R3 we ship the safe-for-tab-content value
  // and let M3 override via a setter if needed.
  bool is_screencast() const override;

  // needs_denoising() — nullopt = "let libwebrtc decide based on its
  // own heuristics". M3 may flip this to false explicitly to disable
  // denoising on synthetic tab content (denoising on rendered web
  // content tends to hurt text crispness without helping SNR).
  // TODO(M2-R3-m3-coupling): see is_screencast above; same M3 hook.
  std::optional<bool> needs_denoising() const override;

  // chromium-7727 API drift (CV2-69 cleanup, #176): the custom
  // diagnostics accessor below is named GetStats() — a 0-param
  // method returning our own CloudBrowserFrameSinkVideoTrackSource
  // Stats struct. It collides by name with the inherited
  // webrtc::VideoTrackSource::GetStats(Stats*) (1-param, out-param,
  // bool return) and would HIDE it, tripping -Woverloaded-virtual
  // (now -Werror on the chromium-7727 build config). This using-
  // declaration un-hides the base overload so both coexist: callers
  // of the libwebrtc-facing GetStats(Stats*) and callers of our
  // 0-param diagnostics accessor each resolve unambiguously. Zero
  // behaviour change — VideoTrackSource's GetStats(Stats*) keeps its
  // default (returns false; we surface real diagnostics via the
  // custom accessor + the metrics sidecar, not via the libwebrtc
  // Stats struct).
  using webrtc::VideoTrackSource::GetStats;

  // Diagnostics. Snapshot of the running counters. Safe to call from
  // any thread (atomic reads — implementation copies under no lock
  // for performance; counters are monotonic so a torn read is harmless
  // for monitoring purposes).
  CloudBrowserFrameSinkVideoTrackSourceStats GetStats() const;

  // R2's converter accumulates its own counters; expose them so the
  // metrics sidecar gets the full picture without re-plumbing through
  // the capturer. Snapshot semantics same as GetStats.
  VideoFrameConversionStats GetConversionStats() const;

  // Test accessor. Returns the underlying capturer pointer (NOT
  // ownership). Lifetime tied to *this. Used by R4/R5 wiring to call
  // Configure / Start / Stop, and by unit tests to drive synthetic
  // frames through the ingress.
  //
  // Returns nullptr if the source was constructed with a null
  // capturer (only possible in tests that bypass the factory; the
  // public constructor accepts null and degrades to a no-op source
  // for header-only compile tests).
  CloudBrowserFrameSinkCapturer* capturer_for_test() { return capturer_.get(); }

  // Explicit capture start. M2 R4 (cb_devtools_agent.cc) drives this
  // from the devtools wire-up path. Long-term (M2 R5), capture start/
  // stop becomes a sink-attachment-driven policy owned by this class
  // — this method then becomes a no-op or is removed entirely. For
  // the current R4-only landing (R5 reverted pending re-arch in
  // capture/build-integration/cb_framesink_video_track_source.{h,cc}
  // direct-edit form, not as a chromium patch), this method delegates
  // straight to capturer_->Start(target) so the devtools start path
  // works end-to-end.
  //
  // Per capturer_->Start's contract: a second call with the SAME target
  // is a no-op, but a call with a DIFFERENT FrameSinkId RE-TARGETS the
  // running capturer (ChangeTarget) — this is how capture follows a
  // cross-document navigation's RenderWidgetHost swap. Safe to call
  // repeatedly.
  //
  // No-op if capturer_ is null (header-only / test bypass case).
  void StartCapture(viz::VideoCaptureTarget target);

 protected:
  ~CloudBrowserFrameSinkVideoTrackSource() override;

  // webrtc::VideoTrackSource override — returns the actual fanout
  // source the base class forwards AddOrUpdateSink / RemoveSink to.
  // We use webrtc::VideoBroadcaster (the canonical multi-sink
  // fanout that ships with libwebrtc and is what every other
  // webrtc::VideoTrackSource subclass uses).
  webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override;

 private:
  // Ingress called by the capturer's OnFrameCallback. Runs on the
  // capturer's chromium sequence. NOT called by libwebrtc directly.
  //
  // Pipeline:
  //   1. Bump frames_received_from_capturer.
  //   2. Call WrapMediaVideoFrameAsWebrtcFrame (R2). On nullopt:
  //      bump frames_dropped_conversion_failed, drop the media frame,
  //      return. (The drop here releases the only ref to the
  //      media::VideoFrame, so the BufferHandleScope dtor fires and
  //      Mojo Done() acks the buffer back to Viz — no pool leak.)
  //   3. broadcaster_.OnFrame(wf) — synchronous fanout to every
  //      attached sink. By the time this returns, sinks have either
  //      copied or extended-refcount the underlying buffer.
  //   4. Bump frames_published_to_sinks.
  //
  // Local |wf| dies on stack-unwind; the underlying scoped_refptr<
  // media::VideoFrame> survives iff some sink kept the buffer.
  void OnCapturerFrame(scoped_refptr<media::VideoFrame> media_frame);

  // webrtc::VideoBroadcaster is the standard libwebrtc multi-sink
  // fanout: holds a list of VideoSinkInterface<VideoFrame>* and
  // delivers each OnFrame() to all of them under its own mutex.
  // Lives FIRST (declaration order = destruction order reversed) so
  // it outlives any in-flight capturer callback.
  webrtc::VideoBroadcaster broadcaster_;

  // Per-source stats. Updated only on the capturer sequence (in
  // OnCapturerFrame), so the writes are unsynchronized; reads from
  // GetStats() may observe a torn value but the counters are
  // monotonic uint64_t so a torn read is at worst a one-frame
  // under-count for a single instant.
  // TODO(M2-R3-stats): if the metrics sidecar starts scraping at
  // sub-second cadence and torn reads become user-visible, promote
  // these to std::atomic<uint64_t> with relaxed ordering. For first-
  // light, plain uint64_t is sufficient.
  CloudBrowserFrameSinkVideoTrackSourceStats stats_;

  // R2's own counters, owned by this source (R2's API takes a pointer
  // and accumulates into whatever stats struct the caller hands it).
  // Same torn-read caveat as stats_.
  VideoFrameConversionStats conversion_stats_;

  // The capturer. Declared LAST so it destructs FIRST when *this dies
  // — that order is LOAD-BEARING per the threading-model commentary
  // at the top of this file. unique_ptr so we own the chromium-side
  // sequence-affine lifetime cleanly.
  //
  // May be null in tests that bypass the factory; OnCapturerFrame
  // can never be invoked in that case (no callback was ever bound),
  // so the null case is effectively "no-op source for sinks to
  // attach to without crashing".
  std::unique_ptr<CloudBrowserFrameSinkCapturer> capturer_;

  // SEQUENCE_CHECKER guards OnCapturerFrame against accidental cross-
  // thread invocation. Bound on the FIRST OnCapturerFrame call (the
  // first frame delivered by the capturer's bound sequence) — webrtc
  // construction may happen on any thread; we don't care.
  SEQUENCE_CHECKER(capturer_sequence_checker_);
};

}  // namespace cloud_browser

#endif  // CAPTURE_FRAMESINK_CAPTURER_CB_FRAMESINK_VIDEO_TRACK_SOURCE_H_

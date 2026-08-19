// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserFrameSinkCapturer — the consumer end of Viz's video
// capture Mojo. It binds a viz::mojom::FrameSinkVideoConsumer
// receiver, drives the producer (a `mojo::Remote<viz::mojom::
// FrameSinkVideoCapturer>` — what the upstream code historically
// called `FrameSinkVideoCapturerPtr`), and delivers each captured
// frame as a `webrtc::VideoFrame` into our libwebrtc track source.
//
// Pair this with the encoder-factory injection added by patch
// patches/0001-expose-encoder-factory-injection.patch (T49) so the
// frames produced here get encoded by CloudBrowserVideoEncoderFactory
// (T19 / T35 / T36).
//
// Cross-references:
//   * docs/capture/framesink-design.md      (T47, the design)
//   * capture/encoder/encoder_factory.h     (T19, downstream sink)
//   * services/viz/privileged/mojom/compositing/frame_sink_video_capture.mojom

#ifndef CAPTURE_FRAMESINK_CAPTURER_CAPTURER_H_
#define CAPTURE_FRAMESINK_CAPTURER_CAPTURER_H_

#include <cstdint>
#include <memory>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/sequence_checker.h"
#include "base/timer/timer.h"
#include "components/viz/common/surfaces/video_capture_target.h"
#include "media/base/capture_version.h"
#include "media/base/video_frame.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/viz/privileged/mojom/compositing/frame_sink_video_capture.mojom.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace webrtc {
class VideoFrame;
}

namespace cloud_browser {

// Stats surfaced through GetStats() so the metrics sidecar (T38) can
// scrape per-second counts. The fields are monotonic.
struct FrameSinkCapturerStats {
  uint64_t frames_received = 0;
  uint64_t frames_delivered = 0;
  uint64_t frames_dropped_by_capturer = 0;  // info.metadata.frame_count_dropped.
  uint64_t frames_failed_to_wrap = 0;
  uint64_t buffers_done = 0;                 // matches frames_received in steady
                                              // state — divergence means a leak.
  // Count of RequestRefreshFrame() calls the idle-refresh deadline timer has
  // issued to the producer (see the class doc's IDLE REFRESH section). On an
  // ANIMATING / continuously-damaging page this stays ~0 (natural production
  // re-arms the deadline before it expires); on an IDLE / sporadically-damaging
  // page (e.g. animejs.com between animations) it climbs in lockstep with
  // frames_received, which is exactly the signal that the hold-and-repeat
  // cadence is what is keeping the wire alive. Lets a log/metrics scrape
  // attribute frames_received deltas to refresh re-delivery vs natural paint.
  uint64_t idle_refreshes_requested = 0;
};

// ---------------------------------------------------------------------
// IDLE REFRESH — constant-frame-rate hold-and-repeat for static content
// ---------------------------------------------------------------------
//
// THE FAILURE THIS CLOSES (byte-proven on staging, 2026-06-25):
// WebRTC video produced ZERO frames for IDLE / sporadic-animation content
// (e.g. animejs.com sitting between its animations), while continuously-
// damaging content (scrolling sites, the portal UI) streamed fine at ~29fps.
// Guest serial: animejs guests logged
//   VERDICT=RENDERER-STARVED(...) / VERDICT=BURSTING, frames_received +0
// while portal-UI guests logged
//   VERDICT=PRODUCING, frames_received +147.
//
// ROOT CAUSE: the FrameSinkVideoCapturer is a PULL consumer — OnFrameCaptured
// fires ONLY when the captured renderer commits a NEW, damaging
// CompositorFrame. CbBeginFrameDriver (capture/build-integration/
// cb_begin_frame_driver.h) issues external BeginFrames at 30fps and forces the
// root Display to redraw, but those ticks do NOT reach an IDLE renderer's
// cc::Scheduler: an idle/static document has client_needs_begin_frame_=false,
// so its CompositorFrameSinkSupport never subscribes to our BeginFrameSource
// and produces nothing. The driver's own header (lines 125-134) states this is
// architecturally unavoidable from the browser process and explicitly defers
// the cure to "the encoder/track-source layer [must] hold-and-repeat the last
// frame (the capturer does not synthesize duplicates)" — i.e. HERE.
//
// THE FIX (this class): an idle-refresh DEADLINE timer. viz's
// FrameSinkVideoCapturer exposes RequestRefreshFrame(), whose documented
// contract (services/viz/privileged/mojom/compositing/frame_sink_video_
// capture.mojom) is: "Requests a frame of current content even if the source
// is not generating new compositor frames" — it re-runs the capturer's
// CopyOutputRequest against the LAST aggregated surface and delivers it as a
// normal OnFrameCaptured. It does NOT force the renderer to repaint (cheap +
// correct for static content) and it re-delivers as a genuinely NEW
// OnFrameCaptured (so it advances frames_received — the ground-truth counter
// the driver scrapes — unlike a pure encoder-side duplicate, which would not).
//
// WHY A DEADLINE TIMER (not a free-running RepeatingTimer): we want the
// refresh to fire ONLY when natural production has gone quiet, so that the
// working PRODUCING path is never touched (no double-delivery, no extra
// composites on a page already painting at full rate). Every OnFrameCaptured —
// whether driven by the renderer OR by a refresh we requested — RE-ARMS the
// timer one idle_refresh_period_ into the future. So:
//   * Animating page (frames arrive < period apart): the deadline is pushed
//     out before it ever fires → ZERO RequestRefreshFrame calls → the fast
//     path is bit-for-bit unchanged.
//   * Idle page (no natural frame for a full period): the deadline fires once,
//     we RequestRefreshFrame(), the re-delivered frame re-arms the deadline,
//     and we settle into a steady period-cadence hold-and-repeat of the last
//     painted content — a constant-frame-rate wire WebRTC can keep decoding.
// This is the standard viz refresh-timer pattern (FrameSinkVideoCapturerImpl's
// own refresh_frame_retry_timer_ works the same way; that timer lives in the
// viz process and is NOT reachable/controllable from our consumer, so we run
// our own at our chosen, faster-than-viz's-1s cadence). The timer is a no-op
// unless explicitly enabled via SetIdleRefreshPeriod() (R5/main_parts wiring).
//
class CloudBrowserFrameSinkCapturer
    : public viz::mojom::FrameSinkVideoConsumer {
 public:
  // The pipeline-side delivery hook. Implementations must be
  // thread-safe and may not block (libwebrtc track sources call
  // OnFrame from this exact callback). The frame is owned by the
  // callback once delivered.
  using OnFrameCallback =
      base::RepeatingCallback<void(scoped_refptr<media::VideoFrame>)>;

  // Construction takes the producer-side Remote (whose connection to
  // Viz the caller has already established via HostFrameSinkManager
  // ::CreateVideoCapturer) plus the delivery callback. The consumer
  // receiver is bound on Start().
  CloudBrowserFrameSinkCapturer(
      mojo::Remote<viz::mojom::FrameSinkVideoCapturer> producer,
      OnFrameCallback on_frame);

  ~CloudBrowserFrameSinkCapturer() override;

  CloudBrowserFrameSinkCapturer(const CloudBrowserFrameSinkCapturer&) = delete;
  CloudBrowserFrameSinkCapturer& operator=(
      const CloudBrowserFrameSinkCapturer&) = delete;

  // Configure the producer-side parameters. Must be called before
  // Start(). All fields have library defaults; callers typically
  // override resolution + format.
  void Configure(const gfx::Size& resolution,
                 media::VideoPixelFormat format,
                 base::TimeDelta min_capture_period);

  // Replace the delivery callback before the first Start(). This lets
  // higher-level owners construct a capturer with a placeholder callback,
  // then bind the final track-source ingress after ownership is established.
  void SetOnFrameCallback(OnFrameCallback on_frame);

  // Enable (or, with a zero/negative period, disable) the idle-refresh
  // deadline — the constant-frame-rate hold-and-repeat that keeps WebRTC
  // streaming the last painted frame when the captured renderer goes idle
  // and stops producing CompositorFrames (see the IDLE REFRESH class doc).
  //
  // |period| is the maximum gap tolerated between delivered frames before we
  // ask the producer to re-deliver the last composited surface via
  // RequestRefreshFrame(). It should be >= the BeginFrame driver's target
  // interval (33ms @ 30fps); a slightly longer value (e.g. one or two frame
  // intervals) avoids racing a renderer that is about to paint on its own.
  // base::TimeDelta() (the default) leaves the feature OFF — existing callers
  // and tests are unaffected until they opt in. Safe to call before or after
  // Start(): if running, it (re-)arms the deadline immediately; if not, the
  // first delivered frame (or Start's priming refresh) arms it.
  void SetIdleRefreshPeriod(base::TimeDelta period);

  // Change the captured output resolution on a RUNNING capturer.
  //
  // Configure() is the before-Start() path and stays that way (its
  // DCHECK(!started_) is load-bearing — it guards the fields Start()
  // latches). This is the mid-session path, and it exists because the
  // viewport is no longer fixed: the user can resize their window.
  //
  // WHY min == max IS PRESERVED. It is tempting to hand viz a RANGE and
  // let it adapt. Don't. With min != max the FrameSinkVideoCapturer
  // re-derives frame geometry from whatever surface it is pointed at,
  // and after a cross-document navigation to a differently-sized page
  // that produced corrupt frames — a small capture anchored top-left
  // that flickered between the old and new surface (M2-R4-MULTI-TAB;
  // see the long comment in Start()). The fix then was to pin the
  // constraints. Nothing about that changed: we still pin them, we just
  // pin them to a value the embedder can move deliberately.
  //
  // |resolution| must be even in both dimensions — I420 chroma is
  // subsampled 2x2 and an odd dimension yields a half-sampled edge row
  // or column. Callers are expected to have aligned already; this
  // rounds DOWN defensively rather than trusting them.
  //
  // No-op when the resolution is unchanged, so a caller may drive this
  // from an unconditional resize handler without churning the producer.
  // Safe before Start(): it updates the field, and Start() applies it.
  void SetCaptureResolution(const gfx::Size& resolution);

  // Bind our consumer receiver, hand the remote to the producer, and
  // call producer->Start(...). A second call retargets the running
  // producer without rebinding the consumer.
  void Start(viz::VideoCaptureTarget target);

  // Tell the producer to stop. Outstanding frames in flight are
  // still delivered + Done()'d.
  void Stop();

  // Snapshot of the running counters — used by the metrics sidecar.
  FrameSinkCapturerStats GetStats() const;

  // viz::mojom::FrameSinkVideoConsumer:
  void OnFrameCaptured(
      media::mojom::VideoBufferHandlePtr data,
      media::mojom::VideoFrameInfoPtr info,
      const gfx::Rect& content_rect,
      mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
          callbacks) override;
  void OnNewCaptureVersion(
      const ::media::CaptureVersion& capture_version) override;
  void OnFrameWithEmptyRegionCapture() override;
  void OnLog(const std::string& message) override;
  void OnStopped() override;

 private:
  // BufferHandleScope is the RAII guard required by the upstream
  // Mojo contract (see services/viz/privileged/.../mojom doc): the
  // consumer MUST call Done() on the FrameCallbacks remote when it
  // is finished reading the buffer, or the producer's buffer pool
  // stalls. Constructing this object captures the remote; destroying
  // it calls Done(). The wrapped media::VideoFrame holds a refcount
  // on this guard so libwebrtc's reference cycle naturally drives
  // the ack.
  class BufferHandleScope;

  // Convert (data, info, content_rect) into a media::VideoFrame
  // wrapping the underlying memory.  Returns nullptr if the wrap
  // fails (we increment frames_failed_to_wrap).
  scoped_refptr<media::VideoFrame> WrapAsMediaFrame(
      media::mojom::VideoBufferHandlePtr data,
      const media::mojom::VideoFrameInfoPtr& info,
      const gfx::Rect& content_rect,
      scoped_refptr<BufferHandleScope> scope);

  // (Re-)arm the idle-refresh deadline if the feature is enabled and capture is
  // running. Called from Start() and from every OnFrameCaptured() so that any
  // delivered frame — natural OR refresh-driven — pushes the deadline out one
  // idle_refresh_period_. A no-op when the feature is off (period <= 0) or
  // capture is stopped. See the IDLE REFRESH class doc.
  void ArmIdleRefreshDeadline();

  // Fired by idle_refresh_timer_ when no frame has been delivered for a whole
  // idle_refresh_period_: the captured renderer is idle and has produced
  // nothing. Asks the producer to re-deliver the last composited surface via
  // RequestRefreshFrame(). The resulting OnFrameCaptured re-arms the deadline,
  // settling into a steady hold-and-repeat cadence. See the IDLE REFRESH doc.
  void OnIdleRefreshDeadline();

  mojo::Remote<viz::mojom::FrameSinkVideoCapturer> producer_;
  mojo::Receiver<viz::mojom::FrameSinkVideoConsumer> consumer_{this};

  OnFrameCallback on_frame_;

  // Configuration cached for Start().
  gfx::Size resolution_{1280, 720};
  // Default to the CPU/shared-memory path. The NV12 mappable-SharedImage/GMB
  // lane is an opt-in Configure() mode for deployments with a real GPU buffer
  // backing; GPU-less pods use I420 for first-light capture.
  media::VideoPixelFormat format_ = media::PIXEL_FORMAT_I420;
  base::TimeDelta min_capture_period_ = base::Hertz(60);

  // Idle-refresh deadline period. base::TimeDelta() (zero) => feature OFF (the
  // default, so existing callers/tests are unchanged until they opt in via
  // SetIdleRefreshPeriod). When positive, idle_refresh_timer_ fires
  // OnIdleRefreshDeadline this long after the last delivered frame. See the
  // IDLE REFRESH class doc.
  base::TimeDelta idle_refresh_period_;

  // The deadline timer itself. A base::OneShotTimer re-armed via .Start() on
  // every delivered frame (which cancels any pending fire and re-schedules —
  // the same re-arm idiom the sibling CbBeginFrameDriver uses for its
  // next_frame_timer_/stall_watchdog_timer_). Owned by this; Stop()/dtor stop
  // it, so base::Unretained(this) in its bound callback is safe (it cannot
  // outlive *this). Runs on the capture sequence, same as every other member.
  base::OneShotTimer idle_refresh_timer_;

  bool started_ = false;
  FrameSinkCapturerStats stats_;

  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace cloud_browser

#endif  // CAPTURE_FRAMESINK_CAPTURER_CAPTURER_H_

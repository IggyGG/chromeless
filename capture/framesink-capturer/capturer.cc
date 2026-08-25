// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserFrameSinkCapturer — see capturer.h.
//
// TODO(T17-build-env): exercise this on a Linux box with depot_tools
// against the libwebrtc + Viz headers from the pinned Chromium tree
// (docs/build/chromium-from-source.md §6). Authored against the
// documented mojom + media::VideoFrame APIs.

#include "capture/framesink-capturer/capturer.h"

#include <utility>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "media/base/video_frame.h"
#include "media/base/video_types.h"
#include "media/capture/mojom/video_capture_buffer.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace cloud_browser {

// ---------------------------------------------------------------------
// BufferHandleScope — RAII Done() ack.
// ---------------------------------------------------------------------
//
// Every OnFrameCaptured() carries a FrameSinkVideoConsumerFrameCallbacks
// remote; the producer's buffer pool will not reclaim the frame's
// memory until we call Done() on it. Forgetting Done() is the single
// most common bug in implementations of this consumer (per T47's
// design doc §3) — so we wrap the remote in a refcounted RAII guard
// whose destructor calls Done().
//
// We attach the guard to the wrapped media::VideoFrame's destruction
// callback. When libwebrtc, the encoder, or anyone else releases their
// last reference to the frame, the guard's refcount hits zero and
// Done() fires automatically. No frame -> no Done leak.
class CloudBrowserFrameSinkCapturer::BufferHandleScope
    : public base::RefCountedThreadSafe<BufferHandleScope> {
 public:
  BufferHandleScope(
      mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
          callbacks,
      base::RepeatingClosure on_done_metric)
      : callbacks_(std::move(callbacks)),
        on_done_metric_(std::move(on_done_metric)),
        callback_task_runner_(base::SequencedTaskRunner::GetCurrentDefault()) {
    DCHECK(callback_task_runner_);
  }

 private:
  friend class base::RefCountedThreadSafe<BufferHandleScope>;
  ~BufferHandleScope() {
    // Ack the buffer back to the producer on the same sequence that received
    // the PendingRemote. Encoder/libwebrtc frame release can happen on a
    // worker thread, and a bound Mojo Remote must not be touched there.
    if (!callbacks_.is_valid()) {
      return;
    }

    if (callback_task_runner_->RunsTasksInCurrentSequence()) {
      RunDone(std::move(callbacks_), std::move(on_done_metric_));
      return;
    }

    callback_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&BufferHandleScope::RunDone, std::move(callbacks_),
                       std::move(on_done_metric_)));
  }

  static void RunDone(
      mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
          callbacks,
      base::RepeatingClosure on_done_metric) {
    mojo::Remote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks> remote(
        std::move(callbacks));
    if (remote.is_bound()) {
      remote->Done();
    }
    if (!on_done_metric.is_null()) {
      on_done_metric.Run();
    }
  }

  mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
      callbacks_;
  base::RepeatingClosure on_done_metric_;
  scoped_refptr<base::SequencedTaskRunner> callback_task_runner_;
};

// ---------------------------------------------------------------------
// CloudBrowserFrameSinkCapturer
// ---------------------------------------------------------------------

CloudBrowserFrameSinkCapturer::CloudBrowserFrameSinkCapturer(
    mojo::Remote<viz::mojom::FrameSinkVideoCapturer> producer,
    OnFrameCallback on_frame)
    : producer_(std::move(producer)), on_frame_(std::move(on_frame)) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
  DCHECK(producer_.is_bound());
  DCHECK(!on_frame_.is_null());
}

CloudBrowserFrameSinkCapturer::~CloudBrowserFrameSinkCapturer() = default;

void CloudBrowserFrameSinkCapturer::Configure(
    const gfx::Size& resolution,
    media::VideoPixelFormat format,
    base::TimeDelta min_capture_period) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(!started_) << "Configure must be called before Start.";
  resolution_ = resolution;
  format_ = format;
  min_capture_period_ = min_capture_period;
}

void CloudBrowserFrameSinkCapturer::SetOnFrameCallback(
    OnFrameCallback on_frame) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(!on_frame.is_null());
  DCHECK(!started_) << "SetOnFrameCallback must be called before Start.";
  DCHECK(!consumer_.is_bound())
      << "SetOnFrameCallback must not run while a consumer pipe is bound.";
  on_frame_ = std::move(on_frame);
}

void CloudBrowserFrameSinkCapturer::SetIdleRefreshPeriod(
    base::TimeDelta period) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Clamp negatives to zero so "off" has a single canonical representation
  // (a negative deadline would be nonsensical to pass to timer.Start anyway).
  idle_refresh_period_ = period.is_positive() ? period : base::TimeDelta();
  if (!idle_refresh_period_.is_positive()) {
    // Disabling: cancel any pending deadline so we stop re-delivering.
    idle_refresh_timer_.Stop();
    LOG(INFO) << "CloudBrowserFrameSinkCapturer: idle-refresh DISABLED";
    return;
  }
  LOG(INFO) << "CloudBrowserFrameSinkCapturer: idle-refresh enabled @ "
            << idle_refresh_period_.InMillisecondsF()
            << "ms deadline (hold-and-repeat last frame when the captured "
               "renderer goes idle)";
  // If capture is already running, arm immediately so an already-idle page
  // starts getting refreshed without waiting for a (never-coming) next frame.
  ArmIdleRefreshDeadline();
}

void CloudBrowserFrameSinkCapturer::SetCaptureResolution(
    const gfx::Size& resolution) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Even-align. I420 subsamples chroma 2x2, so an odd width or height
  // leaves a half-sampled edge. Round DOWN: rounding up could exceed a
  // caller's tier cap, and one pixel of letterbox is invisible while a
  // cap violation is not.
  const gfx::Size aligned(resolution.width() & ~1, resolution.height() & ~1);

  if (aligned.IsEmpty()) {
    LOG(WARNING) << "CloudBrowserFrameSinkCapturer: ignoring empty capture "
                    "resolution "
                 << resolution.ToString()
                 << " (staying at " << resolution_.ToString() << ")";
    return;
  }
  if (aligned == resolution_) {
    return;  // Unconditional-resize-handler friendly: nothing to do.
  }

  const gfx::Size previous = resolution_;
  resolution_ = aligned;

  if (!started_) {
    // Pre-Start: the field is all there is. Start() applies it.
    LOG(INFO) << "CloudBrowserFrameSinkCapturer: capture resolution set to "
              << resolution_.ToString() << " (not yet started)";
    return;
  }

  // Running: push the new constraints at the producer. Same triple
  // Start() applies, for the same reason — see the M2-R4 comment there.
  producer_->SetResolutionConstraints(resolution_, resolution_,
                                      /*use_fixed_aspect_ratio=*/true);

  // Ask for a frame immediately. Without this the new geometry does not
  // reach the wire until the page happens to paint — on a static page
  // that can be seconds, and the user sees their resize do nothing.
  producer_->RequestRefreshFrame();

  LOG(INFO) << "CloudBrowserFrameSinkCapturer: capture resolution "
            << previous.ToString() << " -> " << resolution_.ToString()
            << " (constraints re-pinned; refresh requested)";
}

void CloudBrowserFrameSinkCapturer::Start(viz::VideoCaptureTarget target) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (started_) {
    // Re-assert the output format + fixed resolution constraints BEFORE
    // pointing the running producer at the new FrameSink. A bare
    // ChangeTarget() leaves the Viz FrameSinkVideoCapturer free to derive
    // its frame geometry from the *new* compositor surface's natural size
    // instead of our pinned `resolution_` — which, after a cross-document
    // navigation to a differently-sized page, produced corrupt frames: a
    // small capture anchored in the top-left corner that flickered between
    // the old and new surface (the symptom that surfaced once physics began
    // re-firing capture on FrameNavigated / tab-switch — M2-R4-MULTI-TAB).
    // SetFormat / SetMinCapturePeriod / SetResolutionConstraints are
    // idempotent on the producer and our members are stable across
    // retargets (Configure() runs once, before the first Start()), so
    // re-applying them here is safe and forces every retargeted surface to
    // be scaled into the same fixed 1280x720 fixed-aspect output the
    // initial Start() established.
    producer_->SetFormat(format_);
    producer_->SetMinCapturePeriod(min_capture_period_);
    producer_->SetResolutionConstraints(resolution_, resolution_,
                                        /*use_fixed_aspect_ratio=*/true);
    producer_->ChangeTarget(std::move(target),
                            /*sub_capture_target_version=*/0);
    LOG(INFO) << "CloudBrowserFrameSinkCapturer already running; "
              << "retargeted producer to latest FrameSink target "
              << "(re-applied format + " << resolution_.ToString()
              << " resolution constraints)";
    // A retarget can land on a renderer that is already idle (e.g. a static
    // page after a tab-switch). Re-arm so the idle deadline still fires even
    // if the new surface never produces a natural frame.
    ArmIdleRefreshDeadline();

    // TODO(CV2-KEYFRAME-ON-RETARGET): request an encoder keyframe here.
    //
    // This branch is the moment the streamed CONTENT changes completely — a
    // navigation, a tab switch — and nothing tells the encoder. x264 is
    // configured with i_keyint_max = INT_MAX ("no auto IDR", h264_encoder.cc:147)
    // plus b_intra_refresh, so after a reload the new page is coded against a
    // stale reference and recovers over a slow intra-refresh sweep instead of
    // an instant IDR. That is a concrete mechanism for the user report
    // "after a page reload it feels a bit pixelated" (2026-08-25).
    //
    // The encoder half ALREADY WORKS: h264_encoder.cc:268-276 honours
    // VideoFrameType::kVideoFrameKey by setting X264_TYPE_IDR. What is missing
    // is a caller. Doing it properly means a path from here through
    // CbFramesinkVideoTrackSource to the libwebrtc encoder, which is a real
    // design decision (whose thread? what if no encoder is attached yet?) and
    // does not belong bolted onto a one-line bitrate fix.
    //
    // Deliberately filed as a TODO that names its own verification: after
    // wiring it, a reload should show a bitrate SPIKE (the IDR) in the
    // CV2-RTP outbound[video] samples rather than a slow climb. Per CLAUDE.md
    // this is a task, not a note — it has a measured symptom behind it.
    return;
  }

  // 1. Configure the producer side.
  producer_->SetFormat(format_);
  producer_->SetMinCapturePeriod(min_capture_period_);
  producer_->SetResolutionConstraints(resolution_, resolution_,
                                      /*use_fixed_aspect_ratio=*/true);
  producer_->ChangeTarget(std::move(target),
                          /*sub_capture_target_version=*/0);

  // 2. Bind our consumer receiver and hand the remote to the producer.
  //    kPreferMappableSharedImage is only safe for the explicitly configured
  //    NV12 lane; GPU-less pods default to I420 so Viz gives us CPU shared
  //    memory instead of a GBM/shared-context-backed frame.
  const auto buffer_pref =
      (format_ == media::PIXEL_FORMAT_NV12)
          ? viz::mojom::BufferFormatPreference::kPreferMappableSharedImage
          : viz::mojom::BufferFormatPreference::kDefault;
  if (consumer_.is_bound()) {
    // Viz may have called OnStopped() after a prior capture session,
    // which flips started_ back to false but leaves our consumer pipe
    // bound. A later Cb.startFrameSinkCapture must not call
    // BindNewPipeAndPassRemote() on an already-bound receiver; reset
    // the stale pipe first and bind a fresh one for the new Start().
    consumer_.reset();
  }
  producer_->Start(consumer_.BindNewPipeAndPassRemote(), buffer_pref);
  started_ = true;

  // Arm the idle-refresh deadline (no-op unless SetIdleRefreshPeriod enabled
  // it). If the very first thing the captured page does is sit idle — exactly
  // the animejs.com-between-animations case — the deadline fires and we
  // RequestRefreshFrame() the boot surface, so the wire never stays at 0fps
  // waiting for a natural frame that may not come for seconds.
  ArmIdleRefreshDeadline();
}

void CloudBrowserFrameSinkCapturer::Stop() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!started_) {
    return;
  }
  // The producer will continue to deliver any frames already in
  // flight; OnStopped() fires once the queue is drained. Outstanding
  // BufferHandleScopes still call Done() through their own RAII path.
  producer_->Stop();
  started_ = false;
  // Stop re-delivering: a stopped producer's RequestRefreshFrame would be a
  // no-op at best (and we don't want a refresh racing the OnStopped drain).
  idle_refresh_timer_.Stop();
}

FrameSinkCapturerStats CloudBrowserFrameSinkCapturer::GetStats() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return stats_;
}

// ---------------------------------------------------------------------
// FrameSinkVideoConsumer overrides.
// ---------------------------------------------------------------------

void CloudBrowserFrameSinkCapturer::OnFrameCaptured(
    media::mojom::VideoBufferHandlePtr data,
    media::mojom::VideoFrameInfoPtr info,
    const gfx::Rect& content_rect,
    mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
        callbacks) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ++stats_.frames_received;

  // A frame arrived — the captured renderer is NOT idle right now (this frame
  // is either a natural paint or a re-delivery of the last surface that our
  // own idle deadline requested). Either way, push the idle deadline out one
  // full period. This is the property that makes the fix INERT on the working
  // PRODUCING path: an animating page delivers frames faster than the period,
  // so the deadline is always re-armed before it can fire and we never issue a
  // single RequestRefreshFrame. Only when natural production stops for a whole
  // period does OnIdleRefreshDeadline get to run. Re-arm BEFORE the wrap so
  // even a wrap-failure frame (a real arrival viz produced) counts as activity
  // — a refresh can't fix a wrap failure, so issuing one on top would be
  // pointless churn. No-op unless idle-refresh was enabled.
  ArmIdleRefreshDeadline();

  if (!info) {
    LOG(WARNING) << "OnFrameCaptured: null VideoFrameInfo";
    ++stats_.frames_failed_to_wrap;
    // Still must Done() — anchor a one-shot scope locally so the
    // refcount goes to zero on return.
    auto scope = base::MakeRefCounted<BufferHandleScope>(
        std::move(callbacks),
        base::BindRepeating(
            [](FrameSinkCapturerStats* s) { ++s->buffers_done; }, &stats_));
    return;
  }

  // TODO(T20): media::VideoFrameMetadata::frame_count_dropped was
  // removed in current chromium. Producer-side dropped-frame
  // accounting can be re-added once we identify the replacement
  // metric (likely a different field name in VideoFrameMetadata or a
  // counter on the FrameSinkVideoCapturer host).

  // Build the RAII scope NOW so any early return path still acks the
  // buffer. The scope's lifetime is tied to the wrapped
  // media::VideoFrame; if WrapAsMediaFrame fails, scope drops here.
  auto scope = base::MakeRefCounted<BufferHandleScope>(
      std::move(callbacks),
      base::BindRepeating([](FrameSinkCapturerStats* s) { ++s->buffers_done; },
                          &stats_));

  scoped_refptr<media::VideoFrame> frame =
      WrapAsMediaFrame(std::move(data), info, content_rect, scope);
  if (!frame) {
    ++stats_.frames_failed_to_wrap;
    return;  // scope destruction → Done().
  }

  // Pin the scope to the frame's release path so Done() fires when
  // libwebrtc / encoder / anyone holding a ref drops it.
  // AddDestructionObserver is fired on the same sequence frame
  // destruction runs on; our stats counter is sequence-checked, so
  // we update buffers_done in the BufferHandleScope's dtor (above)
  // rather than here.
  frame->AddDestructionObserver(base::BindOnce(
      [](scoped_refptr<BufferHandleScope> s) { /* drop */ }, scope));

  ++stats_.frames_delivered;
  on_frame_.Run(std::move(frame));
}

void CloudBrowserFrameSinkCapturer::OnNewCaptureVersion(
    const ::media::CaptureVersion& /*capture_version*/) {
  // We don't use sub-capture targets in v1; ignore.
}

void CloudBrowserFrameSinkCapturer::OnFrameWithEmptyRegionCapture() {
  // The capturer signalled an empty-region frame. Phase 1 ignores;
  // Phase 3 may emit a "fully damaged" hint to the encoder so it
  // forces an IDR / cyclic-refresh pass. (re-validate)
}

void CloudBrowserFrameSinkCapturer::OnLog(const std::string& message) {
  VLOG(1) << "FrameSinkVideoCapturer log: " << message;
}

void CloudBrowserFrameSinkCapturer::OnStopped() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  started_ = false;
  // Producer has fully drained and stopped: no more frames will arrive, so a
  // pending idle deadline (if any) must be cancelled — its RequestRefreshFrame
  // would be sent to a stopped producer.
  idle_refresh_timer_.Stop();
}

// ---------------------------------------------------------------------
// Idle-refresh deadline — see the IDLE REFRESH class doc in capturer.h.
// ---------------------------------------------------------------------

void CloudBrowserFrameSinkCapturer::ArmIdleRefreshDeadline() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Feature off, or capture not running → nothing to arm. (When off this is
  // the early-out that makes every OnFrameCaptured re-arm call free.)
  if (!idle_refresh_period_.is_positive() || !started_) {
    return;
  }
  // base::OneShotTimer::Start cancels any pending fire and re-schedules — so
  // calling this on every delivered frame simply pushes the deadline out,
  // exactly the re-arm idiom CbBeginFrameDriver uses. base::Unretained(this) is
  // safe: idle_refresh_timer_ is a member, cannot outlive *this, and Stop()/the
  // dtor stop it before *this dies.
  idle_refresh_timer_.Start(
      FROM_HERE, idle_refresh_period_,
      base::BindOnce(&CloudBrowserFrameSinkCapturer::OnIdleRefreshDeadline,
                     base::Unretained(this)));
}

void CloudBrowserFrameSinkCapturer::OnIdleRefreshDeadline() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // We only get here if a whole idle_refresh_period_ elapsed with no
  // OnFrameCaptured re-arming the deadline: the captured renderer is idle and
  // has produced nothing. Ask the producer to RE-DELIVER the last composited
  // surface. Per the viz mojom contract this delivers a frame of CURRENT
  // content even though the source is not generating new compositor frames —
  // it does NOT force the renderer to repaint (cheap, correct for static
  // content) and it arrives as a normal OnFrameCaptured, which:
  //   (a) advances frames_received (the ground-truth counter the BeginFrame
  //       driver scrapes — so the wire shows real, non-zero capture fps), and
  //   (b) re-arms THIS deadline from inside OnFrameCaptured, settling into a
  //       steady period-cadence hold-and-repeat until the page paints again.
  //
  // LOAD-BEARING viz CONTRACT this rests on: a refresh-delivered frame carries
  // a FRESH capture timestamp (VideoCaptureOracle stamps it from current
  // capture time, not the stale content time), so each re-delivery has a
  // distinct, monotonically-increasing info->timestamp. video_frame_conversion
  // rebases that into the webrtc frame's timestamp_us (see
  // video_frame_conversion.cc RebaseMediaTimestampToWebrtcMicros), so the
  // encoder sees advancing timestamps and does NOT dedupe the repeated content
  // — it emits real (heavily-compressed, since pixels are identical) frames at
  // our cadence. This is the SAME mechanism viz's own idle refresh_frame_retry_
  // timer_ uses at its 1s fallback rate (the ~0.5fps baseline the BeginFrame
  // driver header cites); we simply drive it explicitly at a faster, reliable
  // cadence instead of relying on viz's internal heuristic timer to fire.
  // Guard on started_ in case a Stop()/OnStopped raced the timer fire.
  if (!started_ || !idle_refresh_period_.is_positive()) {
    return;
  }
  ++stats_.idle_refreshes_requested;
  producer_->RequestRefreshFrame();

  // Re-arm a fallback deadline NOW rather than relying solely on the refreshed
  // frame coming back to re-arm us. RequestRefreshFrame is best-effort: viz can
  // legitimately drop it (e.g. no aggregated surface yet at boot, or a frame
  // already in flight). Without this fallback, a dropped refresh would leave NO
  // pending deadline and the hold-and-repeat cadence would die after one miss.
  // If the refresh DOES come back, OnFrameCaptured's re-arm simply supersedes
  // this one (Start() cancels-and-reschedules) — so we never double-fire.
  ArmIdleRefreshDeadline();
}

// ---------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------

scoped_refptr<media::VideoFrame>
CloudBrowserFrameSinkCapturer::WrapAsMediaFrame(
    media::mojom::VideoBufferHandlePtr data,
    const media::mojom::VideoFrameInfoPtr& info,
    const gfx::Rect& content_rect,
    scoped_refptr<BufferHandleScope> scope) {
  if (!data)
    return nullptr;

  // The producer uses two buffer-handle variants:
  //   * read_only_shmem_region — CPU-side shared memory (I420 / ARGB
  //     and the fallback path for NV12).
  //   * gpu_memory_buffer_handle — GMB-backed (NV12 zero-copy path).
  scoped_refptr<media::VideoFrame> frame;

  if (data->is_read_only_shmem_region()) {
    auto mapping = data->get_read_only_shmem_region().Map();
    if (!mapping.IsValid()) {
      return nullptr;
    }
    frame = media::VideoFrame::WrapExternalData(
        info->pixel_format, info->coded_size, info->visible_rect,
        info->visible_rect.size(),
        base::span<const uint8_t>(static_cast<const uint8_t*>(mapping.memory()),
                                  mapping.size()),
        info->timestamp);
    if (frame) {
      // Keep the mapping alive for as long as the frame exists.
      frame->BackWithOwnedSharedMemory(
          std::move(data->get_read_only_shmem_region()), std::move(mapping));
    }
#if 0
  // TODO(T17): port GMB→VideoFrame path to MappableSharedImage when
  // chromium-side renderer is wired. WrapExternalGpuMemoryBuffer was
  // removed; the replacement WrapMappableSharedImage takes a
  // gpu::ClientSharedImage which isn't available at this call site
  // (see media/base/video_frame.h:233). For first-light we ride the
  // shmem path only; GMB is a perf optimization for hardware decode
  // and isn't needed for the encoder factory injection MVP.
  } else if (data->is_gpu_memory_buffer_handle()) {
    frame = media::VideoFrame::WrapExternalGpuMemoryBuffer(
        info->visible_rect,
        info->visible_rect.size(),
        std::move(data->get_gpu_memory_buffer_handle()),
        info->pixel_format,
        info->timestamp);
#endif
  } else {
    LOG(WARNING) << "OnFrameCaptured: unknown VideoBufferHandle variant";
    return nullptr;
  }

  if (!frame)
    return nullptr;

  // Attach the timing + color metadata libwebrtc cares about. The
  // capturer's CAPTURE_END_TIME is the right reference for our
  // glass-to-glass measurement (T47 §3).
  frame->set_metadata(info->metadata);
  frame->set_color_space(info->color_space);

  // TODO(T20): media::VideoFrame::set_visible_rect was removed.
  // Previously we tightened visible_rect to content_rect when the
  // producer letterboxed. For first-light we accept info->visible_rect
  // (which is already passed to WrapExternalData / WrapMappableSI).
  // Re-introduce the tighter visible_rect once we identify the
  // replacement (likely passing content_rect as the visible_rect arg
  // to the wrap call instead of info->visible_rect).

  // Pin the BufferHandleScope to the frame's release. This is what
  // makes Done() fire automatically when the encoder is finished
  // with the buffer.
  frame->AddDestructionObserver(base::BindOnce(
      [](scoped_refptr<BufferHandleScope> s) { /* refcount drop */ },
      std::move(scope)));

  return frame;
}

}  // namespace cloud_browser

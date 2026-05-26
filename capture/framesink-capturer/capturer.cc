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

void CloudBrowserFrameSinkCapturer::Start(viz::VideoCaptureTarget target) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (started_) {
    return;
  }
  started_ = true;

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
  producer_->Start(consumer_.BindNewPipeAndPassRemote(), buffer_pref);
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

  if (!info) {
    LOG(WARNING) << "OnFrameCaptured: null VideoFrameInfo";
    ++stats_.frames_failed_to_wrap;
    // Still must Done() — anchor a one-shot scope locally so the
    // refcount goes to zero on return.
    auto scope = base::MakeRefCounted<BufferHandleScope>(
        std::move(callbacks),
        base::BindRepeating(
            [](FrameSinkCapturerStats* s) { ++s->buffers_done; },
            &stats_));
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
      base::BindRepeating(
          [](FrameSinkCapturerStats* s) { ++s->buffers_done; },
          &stats_));

  scoped_refptr<media::VideoFrame> frame = WrapAsMediaFrame(
      std::move(data), info, content_rect, scope);
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
  if (!data) return nullptr;

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
        info->pixel_format,
        info->coded_size,
        info->visible_rect,
        info->visible_rect.size(),
        base::span<const uint8_t>(
            static_cast<const uint8_t*>(mapping.memory()),
            mapping.size()),
        info->timestamp);
    if (frame) {
      // Keep the mapping alive for as long as the frame exists.
      frame->BackWithOwnedSharedMemory(
          std::move(data->get_read_only_shmem_region()),
          std::move(mapping));
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

  if (!frame) return nullptr;

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

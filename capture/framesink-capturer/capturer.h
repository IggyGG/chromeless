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
};

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

  bool started_ = false;
  FrameSinkCapturerStats stats_;

  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace cloud_browser

#endif  // CAPTURE_FRAMESINK_CAPTURER_CAPTURER_H_

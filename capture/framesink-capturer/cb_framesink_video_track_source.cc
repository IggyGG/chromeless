// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// See cb_framesink_video_track_source.h for the design.

#include "capture/framesink-capturer/cb_framesink_video_track_source.h"

#include <optional>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "capture/framesink-capturer/video_frame_conversion.h"

// webrtc public-API helpers — visible via the M2 R1 passthrough.
#include "api/video/video_frame.h"
#include "rtc_base/logging.h"

namespace cloud_browser {

CloudBrowserFrameSinkVideoTrackSource::CloudBrowserFrameSinkVideoTrackSource(
    std::unique_ptr<CloudBrowserFrameSinkCapturer> capturer)
    : webrtc::VideoTrackSource(/*remote=*/false),
      capturer_(std::move(capturer)) {
  // Detach the sequence checker — OnCapturerFrame binds it on its
  // first invocation (the capturer's chromium sequence, set by the
  // caller's Start() site, which may not be the construction thread).
  DETACH_FROM_SEQUENCE(capturer_sequence_checker_);

  // Rebind the capturer's OnFrameCallback to route into our ingress.
  // base::Unretained(this) is safe because:
  //   * capturer_ is owned by *this (unique_ptr member).
  //   * Member destruction order (reverse of declaration) destroys
  //     capturer_ FIRST when *this dies; the capturer's destructor
  //     drops its mojo::Receiver binding, after which no further
  //     OnFrameCallback invocations are possible.
  //   * So the callback can never fire after *this is gone.
  if (capturer_) {
    capturer_->SetOnFrameCallback(base::BindRepeating(
        &CloudBrowserFrameSinkVideoTrackSource::OnCapturerFrame,
        base::Unretained(this)));
  }
}

CloudBrowserFrameSinkVideoTrackSource::
    ~CloudBrowserFrameSinkVideoTrackSource() {
  // Implicit member-destruction order (reverse of declaration):
  //   1. capturer_       — unique_ptr destructor → CloudBrowserFrame
  //                        SinkCapturer destructor → mojo::Receiver
  //                        binding drops → no more OnCapturerFrame
  //                        callbacks can fire.
  //   2. conversion_stats_ + stats_ — trivial.
  //   3. broadcaster_   — webrtc::VideoBroadcaster destructor; any
  //                        sinks still attached at this point are a
  //                        caller bug (the source outlived its
  //                        consumers' attachment lifetime), but the
  //                        destructor is well-defined regardless
  //                        (broadcaster_ holds raw pointers to sinks;
  //                        no callback fires from the destructor).
  //
  // Nothing to do explicitly here; this body exists only to make the
  // destructor symmetric with the declaration in the header (out-of-
  // line so the inline header doesn't need to know the destructor
  // bodies of the member types).
}

bool CloudBrowserFrameSinkVideoTrackSource::is_screencast() const {
  // See header comment for rationale + M3 TODO. Tab-content capture
  // from a chromium FrameSink is conceptually screencast; the encoder
  // factory uses this to bias settings toward content with frequent
  // localized changes (text scrolling, UI animation) rather than
  // camera video.
  return true;
}

std::optional<bool> CloudBrowserFrameSinkVideoTrackSource::needs_denoising()
    const {
  // nullopt = libwebrtc default behaviour. M3's peer-track contract
  // may flip this to false explicitly. See header.
  return std::nullopt;
}

CloudBrowserFrameSinkVideoTrackSourceStats
CloudBrowserFrameSinkVideoTrackSource::GetStats() const {
  // Plain copy — torn-read caveat documented in header. Caller may
  // observe a one-frame-stale count for a single instant; harmless
  // for monotonic monitoring.
  return stats_;
}

VideoFrameConversionStats
CloudBrowserFrameSinkVideoTrackSource::GetConversionStats() const {
  return conversion_stats_;
}

webrtc::VideoSourceInterface<webrtc::VideoFrame>*
CloudBrowserFrameSinkVideoTrackSource::source() {
  // webrtc::VideoTrackSource (base) forwards AddOrUpdateSink /
  // RemoveSink to whatever this returns. webrtc::VideoBroadcaster
  // implements rtc::VideoSourceInterface<webrtc::VideoFrame> exactly
  // for this purpose (it's how upstream's VideoCapturerTrackSource
  // and AdaptedVideoTrackSource both wire their fanout).
  return &broadcaster_;
}

void CloudBrowserFrameSinkVideoTrackSource::StartCapture(
    viz::VideoCaptureTarget target) {
  // M2 R4 entry point — see header doc. Long-term this method is
  // replaced by M2 R5's sink-attachment-driven policy, after R5 is
  // re-architected as a direct cloud-browser source edit (the patch
  // form was rejected; see CV2-40).
  //
  // For the current R4-only landing: delegate straight to the
  // underlying capturer's Start. Start is a no-op only for a repeat of
  // the SAME target; a different FrameSinkId re-targets the running
  // capturer (capturer.cc:138-164 ChangeTarget), so multiple
  // devtools-driven invocations — including capture re-arm after a
  // navigation's RenderWidgetHost swap — are safe and effective.
  if (!capturer_) {
    // Header-only / test-bypass case. No-op.
    return;
  }
  capturer_->Start(target);
}

void CloudBrowserFrameSinkVideoTrackSource::SetOnContentChangedCallback(
    base::RepeatingClosure on_content_changed) {
  if (!capturer_) {
    return;  // Header-only / test-bypass case.
  }
  capturer_->SetOnContentChangedCallback(std::move(on_content_changed));
}

void CloudBrowserFrameSinkVideoTrackSource::SetCaptureResolution(
    const gfx::Size& resolution) {
  if (!capturer_) {
    // Header-only / test-bypass case. No-op.
    return;
  }
  capturer_->SetCaptureResolution(resolution);
}

void CloudBrowserFrameSinkVideoTrackSource::OnCapturerFrame(
    scoped_refptr<media::VideoFrame> media_frame) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(capturer_sequence_checker_);
  ++stats_.frames_received_from_capturer;

  if (!media_frame) {
    // Capturer should never deliver a null frame (see capturer.h
    // contract — wrap-failure paths bump the capturer's own
    // frames_failed_to_wrap counter and skip the callback). But
    // defensive: don't increment frames_dropped_conversion_failed
    // here, since the conversion step never ran. Just no-op.
    return;
  }

  // R2 conversion. Hands us a webrtc::VideoFrame that wraps a
  // lifetime-pinning buffer holding the media::VideoFrame ref. The
  // |conversion_stats_| out-param accumulates per-format wrap counts
  // + drop reasons (see video_frame_conversion.h).
  std::optional<webrtc::VideoFrame> wf =
      WrapMediaVideoFrameAsWebrtcFrame(media_frame, &conversion_stats_);
  if (!wf) {
    ++stats_.frames_dropped_conversion_failed;
    // Drop |media_frame| here (it goes out of scope on return) — the
    // last ref to it dies, the capturer's BufferHandleScope dtor
    // fires, Mojo Done() acks the buffer back to Viz. No pool leak
    // from the unsupported-format / empty-rect path.
    return;
  }

  // Synchronous fanout. Under broadcaster_'s mutex, every attached
  // sink sees OnFrame(wf). When this returns:
  //   * Sinks that copied pixels (e.g. analytics sinks): wf-buffer
  //     ref drops on stack-unwind below; capturer's Done() acks.
  //   * Sinks that extended-refcount the buffer (e.g. the encoder
  //     RTP path): wf-buffer ref count stays > 0; Done() ack waits
  //     for the sink-side release. Viz pool slot stays held. This
  //     IS the backpressure mechanism — see header.
  broadcaster_.OnFrame(*wf);

  ++stats_.frames_published_to_sinks;

  // |wf| goes out of scope on return — drops its buffer ref; sink-
  // side refs (if any) keep the buffer alive past this stack frame.
}

}  // namespace cloud_browser

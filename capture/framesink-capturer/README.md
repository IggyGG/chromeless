# `capture/framesink-capturer/`

`capturer.{h,cc}` is the **consumer end** of Viz's video-capture
Mojo. Pair it with `media_factory` injection from T19 (added by
[`patches/0001-expose-encoder-factory-injection.patch`](../../patches/0001-expose-encoder-factory-injection.patch))
to feed our encoder factory.

```
[Viz process]                        [our cloud-browser worker]

FrameSinkVideoCapturerImpl  --Mojo->  CloudBrowserFrameSinkCapturer
   (producer)                          (consumer; this directory)
                                          |
                                          | OnFrameCallback
                                          v
                                      track source
                                          |
                                          | webrtc::VideoFrame
                                          v
                                      libwebrtc PeerConnection
                                          |
                                          | VideoEncoderFactory injected
                                          | via patch 0001
                                          v
                                      CloudBrowserVideoEncoderFactory
                                      (T19 / T35 / T36)
                                          |
                                          v
                                      Vp9Encoder / H264Encoder
```

## Files

| File | What it is |
|------|-----------|
| `capturer.h`        | Class declaration. Inherits `viz::mojom::FrameSinkVideoConsumer`. |
| `capturer.cc`       | Implementation, including the `BufferHandleScope` RAII guard that calls `Done()` when the wrapped `media::VideoFrame` is finally released. |
| `capturer_test.cc`  | gtests with a fake producer; assert frame delivery, no-Done-leak, dropped-count surfacing, Stop wiring. |

## The Done() lifecycle

Every `OnFrameCaptured()` call carries a
`FrameSinkVideoConsumerFrameCallbacks` Mojo remote. The producer's
buffer pool will not reclaim the frame until we call `Done()` on it.
Forgetting `Done()` was called out as the single most common bug in
this consumer in the T47 design (`docs/capture/framesink-design.md`
§3) — so the implementation tethers the ack to the wrapped
`media::VideoFrame`'s destruction:

1. On every `OnFrameCaptured`, build a refcounted `BufferHandleScope`
   that owns the unbound `FrameCallbacks` pending remote and captures
   the current sequenced task runner.
2. Wrap the buffer as a `media::VideoFrame` and pin the scope to its
   `AddDestructionObserver`.
3. When libwebrtc / encoder / track source finally releases the
   frame, the scope refcount drops to zero. The destructor binds and
   calls `Done()` on the original capture sequence, posting back there
   if the frame was released on a libwebrtc worker thread.

The error paths (null `VideoFrameInfo`, wrap failure) drop the scope
locally — the destructor still fires, the buffer still gets acked,
no pool starvation.

## Build

Compiled into `source_set("framesink_capture")` in
[`capture/build-integration/BUILD.gn`](../build-integration/BUILD.gn)
(T49). Pulls in `//components/viz/host`, `//components/viz/service`,
`//services/viz/privileged/mojom`, and
`//third_party/webrtc/api/video:video_frame`.

## Status

Code is **authored to spec** against documented Viz mojom +
libwebrtc + Chromium media headers. Actual compile + link is gated
on T17's build environment — every file carries the
`TODO(T17-build-env)` marker per the team convention.

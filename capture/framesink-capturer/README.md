# `capture/framesink-capturer/`

`capturer.{h,cc}` is the **consumer end** of Viz's video-capture
Mojo. Pair it with the browser-process `PeerConnectionFactory` seam in
[`cloud_browser_pcf.cc`](../build-integration/cloud_browser_pcf.cc) to feed our
encoder factory. The former renderer-side patch 0001 was retired; the current
patch-series status is recorded in [`patches/README.md`](../../patches/README.md).

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
                                          | via browser-process PCF seam
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

## Idle refresh (constant-frame-rate hold-and-repeat)

`FrameSinkVideoCapturer` is a **pull** consumer: `OnFrameCaptured`
fires only when the captured renderer commits a *new, damaging*
`CompositorFrame`. An **idle / static page** (e.g. `animejs.com`
sitting between animations) commits nothing, and the external
`CbBeginFrameDriver` ticks do **not** reach an idle renderer's
`cc::Scheduler` (see `../build-integration/cb_begin_frame_driver.h`).
So without intervention an idle page streams **zero** frames — the
byte-proven staging defect (`VERDICT=RENDERER-STARVED`,
`frames_received +0`), while continuously-damaging content (scrolling,
the portal UI) streamed fine at ~29fps.

The capturer closes this with an **idle-refresh deadline**, opt-in via
`SetIdleRefreshPeriod(period)` (wired on in
`../build-integration/cloud_browser_browser_main_parts.cc` step 5b at
10fps):

- Every delivered frame — natural **or** refresh-driven — re-arms a
  one-shot deadline `period` into the future.
- An **animating** page delivers frames faster than `period`, so the
  deadline never fires → **zero** `RequestRefreshFrame` calls → the
  producing path is bit-for-bit unchanged.
- An **idle** page lets the deadline fire; we call the producer's
  `RequestRefreshFrame()`, which re-delivers the *last composited
  surface* (no renderer repaint) as a normal `OnFrameCaptured`. That
  advances `frames_received` and re-arms the deadline, settling into a
  steady `period`-cadence hold-and-repeat until the page paints again.

`FrameSinkCapturerStats.idle_refreshes_requested` counts the issued
refreshes so a log/metric scrape can attribute `frames_received`
deltas to refresh re-delivery vs. natural paint. See the **IDLE
REFRESH** comment block in `capturer.h` for the full rationale.

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

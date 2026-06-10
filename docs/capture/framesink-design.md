# FrameSinkVideoCapturer integration design (Phase 2)

**Status:** Phase 2 design (per PROJECT_BRIEF.md and the T29 capture
spike that ruled out `HeadlessExperimental.beginFrame`).
**Owner:** chromium-dev.
**Cross-references:**
`docs/capture/path-of-least-resistance.md` (T15 — the Phase 1 path
this replaces),
`docs/build/chromium-from-source.md` (T17 — build env),
`capture/encoder/encoder_factory.h` (T19 — factory contract),
`capture/spike-beginframe/findings.md` (T29 — why we are here).

This is the design for **how Phase 2 captures pixels.** The Phase 1
`getDisplayMedia` path (T15) is intentionally non-trivial to remove
because it works; this document does not propose ripping it out, it
proposes building the next path behind a feature flag, measuring
both, and switching when the numbers warrant.

---

## 1. Where `FrameSinkVideoCapturer` sits in Chromium

Chromium's compositor lives in the **Viz** process (formerly the GPU
process). Each compositor surface — a tab, a popup, a webview —
publishes its rendered output as a **frame sink**. A frame sink has a
unique `FrameSinkId` and produces compositor frames; consumers
subscribe to it.

`FrameSinkVideoCapturer` is the consumer that turns a frame sink into
a stream of `media::VideoFrame`s. It runs **inside Viz**; the
client-side handle is a Mojo remote.

```
   Browser process                    Viz / GPU process
  +-------------------+              +----------------------------+
  | content::         |              | viz::                      |
  | RenderWidgetHost  |              |  CompositorFrameSinkSupport|
  |   Impl            |  IPC frames  |  (one per tab / surface)   |
  |  (owns FrameSinkId)|------------>|                            |
  +-------+-----------+              |                            |
          |                          |    receives compositor     |
          | reads / writes via       |    frames + damage         |
          v                          |                            |
  +-------------------+   Mojo       |    +---------------------+ |
  | viz::             |<==========>  |    | viz::               | |
  | HostFrameSink     |              |    | FrameSinkManagerImpl| |
  | Manager           |              |    +-----------+---------+ |
  | (browser-side     |              |                |           |
  |  wrapper)         |              |                v           |
  +---+---------------+              |    +---------------------+ |
      | CreateVideoCapturer          |    | viz::               | |
      v                              |    | FrameSinkVideo      | |
  +---------------------+   Mojo     |    | CapturerImpl        | |
  | viz::ClientFrameSink|<==========>|    | (impls Mojo iface   | |
  | VideoCapturer       |            |    |  in service/frame_  | |
  | (resilient wrapper) |            |    |  sinks/video_       | |
  +---------------------+            |    |  capture/...)       | |
                                      |    +-----------+---------+ |
                                      |                |           |
                              consumed |                | OnFrame  |
                              frames   |                | Captured |
                              (Mojo)   |                v           |
                                      |     +-----------------+    |
                                      |     | OUR             |    |
                                      |     | FrameSinkVideo  |    |
                                      |     | Consumer (impl) |    |
                                      |     +-----------------+    |
                                      +----------------------------+
```

Reference files in the upstream tree:

- [`components/viz/host/host_frame_sink_manager.h`](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/components/viz/host/host_frame_sink_manager.h)
  — browser-side wrapper around the Mojo `FrameSinkManager` in Viz.
  Owns the registry, exposes `RegisterFrameSinkId()`,
  `RegisterFrameSinkHierarchy()`, and the all-important
  `CreateVideoCapturer()` factory.
- [`components/viz/service/frame_sinks/video_capture/frame_sink_video_capturer_impl.h`](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/components/viz/service/frame_sinks/video_capture/frame_sink_video_capturer_impl.h)
  — the Viz-side implementation. Implements
  `mojom::FrameSinkVideoCapturer`. Frames flow out via a
  `mojom::FrameSinkVideoConsumer` we provide.
- [`services/viz/privileged/mojom/compositing/frame_sink_video_capture.mojom`](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/services/viz/privileged/mojom/compositing/frame_sink_video_capture.mojom)
  — the wire IDL: `FrameSinkVideoCapturer.{Start,Stop,ChangeTarget,
  RequestRefreshFrame,SetFormat,SetMinCapturePeriod,SetResolution
  Constraints,CreateOverlay}` and
  `FrameSinkVideoConsumer.{OnFrameCaptured,OnNewSubCaptureTargetVersion,
  OnFrameWithEmptyRegionCapture,OnLog,OnStopped}`.

How a host attaches a capturer to a tab (sketch):

```cpp
// browser-side:
// 1. We already have the WebContents → RenderWidgetHostView, which
//    knows its FrameSinkId.
auto fsid = render_widget_host_view->GetFrameSinkId();

// 2. Through HostFrameSinkManager (one per browser process),
//    create a video capturer.
auto capturer = host_frame_sink_manager_->CreateVideoCapturer();

// 3. Configure it. GPU-less pods use I420/shared memory; NV12 is the
//    opt-in hardware path when a GBM/shared-context backing is present.
capturer->SetFormat(media::PIXEL_FORMAT_I420);
capturer->SetMinCapturePeriod(base::Hertz(60));
capturer->SetResolutionConstraints(target_size, target_size,
                                   /*use_fixed_aspect_ratio=*/true);
capturer->ChangeTarget(viz::VideoCaptureTarget(fsid),
                       /*sub_capture_target_version=*/0);

// 4. Hand it our consumer and start.
capturer->Start(our_consumer_.BindNewPipeAndPassRemote(),
                viz::mojom::BufferFormatPreference::kDefault);
```

The "resilient wrapper" (`ClientFrameSinkVideoCapturer`) is
important: if the Viz process crashes, the wrapper transparently
re-establishes the Mojo channel and re-issues the configuration
calls. We get this for free.

## 2. Two integration approaches

### A. Embedder approach — we are the browser process

We run Chromium **as our browser process** (akin to how
`HeadlessShell`, `content_shell`, or Electron embed Chromium) and
host `HostFrameSinkManager` ourselves. Our binary owns the frame
sink registry, attaches a capturer to whichever surface we want, and
consumes frames in-process.

Pros:
- **No upstream patches required** beyond what we already need for
  encoder factory injection (T19). The Mojo interfaces are already
  public.
- **Single process for capture + encode + send.** The captured
  `media::VideoFrame` becomes the input to our
  `webrtc::VideoFrame` -> `webrtc::VideoEncoder` chain with one
  CPU↔CPU copy at most (and zero copies on the GPU memory buffer
  path).
- **Tracks Chromium upstream the way headless does** — i.e., we
  mostly inherit upstream maintenance because the browser-side API
  is what `chrome` itself uses.

Cons:
- We have to implement a chunk of `BrowserMainParts` /
  `ContentBrowserClient` to bootstrap the browser process. That is
  not zero — Headless ships ~3 KLOC just for the browser-process
  glue. Most of it is rote and tolerant of upstream churn, but it
  is more code than "we ship a patch."
- We become responsible for things browser processes own:
  preferences, startup ordering, GPU process spawn, sandbox
  configuration. Our story is closer to "a custom browser binary"
  than "a headless server."

### B. Patch-on-top approach — small patch to stock Chromium

We build stock Chromium with a small patch series (per T17's
patches/ pattern) that exposes a capture API to an external process —
either by extending the DevTools Protocol (`Capture.startStream` ?) or
by exposing a Mojo endpoint over a named pipe. Our capture process
sits **outside** Chromium and pulls frames via that channel.

Pros:
- Less code we own. The bridge is small; everything else stays
  upstream.
- Capture process can crash and restart without taking Chromium
  down.

Cons:
- **Every patch is forever.** This is exactly what
  PROJECT_BRIEF.md's "Don't fork Chromium until you have to"
  warns against, and the patch lives squarely in Viz / content
  layer where reorganization happens regularly.
- Mojo IPC across a process boundary adds at least one copy of the
  frame plus the channel latency. Defeats half the point of moving
  off `getDisplayMedia` in the first place.
- DevTools-extension flavour means we re-enter the
  DevTools-protocol surface that T29 found brittle.

### Verdict: Approach A (embedder)

Chosen path. Reasons:

- **Latency.** The whole motivation for moving off
  `getDisplayMedia` is to remove the extra compositor pass and the
  `MediaStreamTrack` round trip (T15 §3). Approach B re-introduces a
  process boundary; approach A keeps the encoded output one
  in-process callback away from the captured frame.
- **Maintainability across version bumps.** Public browser-process
  APIs (`HostFrameSinkManager`, `mojom::FrameSinkVideoCapturer`)
  change much more slowly than Viz internals. Headless Chromium
  rides the same APIs and survives every roll. A patch that hooks
  *inside* Viz would not.
- **Consistency with T19 / T35 / T36.** The encoder factory work
  already assumes we are in-process with libwebrtc. Approach A
  keeps the whole pipeline in one binary; approach B would require
  shipping the capture frames *to* the libwebrtc process anyway.

Approach B is documented here so the next person revisiting the
choice can see the reasoning rather than have to re-derive it.

## 3. Frame consumption protocol

Wire format (per `frame_sink_video_capture.mojom`):

```
FrameSinkVideoConsumer.OnFrameCaptured(
    VideoBufferHandle      data,            // shared memory or GMB
    media.mojom.VideoFrameInfo info,        // format, coded_size,
                                             // visible_rect, color_space,
                                             // metadata
    gfx.mojom.Rect          content_rect,    // active region; rest is
                                             // letterbox
    pending_remote<FrameSinkVideoConsumerFrameCallbacks> callbacks);
```

Key fields:

- **Format.** We `SetFormat(NV12)` on capable hardware (zero-copy
  GPU memory buffer path), `SetFormat(I420)` otherwise. Both are
  consumed cleanly by libwebrtc's `webrtc::VideoFrame`.
- **Timestamps.** `info.metadata` carries `CAPTURE_BEGIN_TIME` and
  `CAPTURE_END_TIME`. We pin our `webrtc::VideoFrame::timestamp_us`
  to `CAPTURE_END_TIME` — that is the moment Viz finished compositing
  this frame, which is the right reference for end-to-end latency.
- **Damage / update rectangle.** `info.metadata.CAPTURE_UPDATE_RECT`
  is the dirty rectangle within the frame. v2 ignores this and
  encodes full frames; Phase 3 uses it for damage-rect-aware partial
  encoding (intra-refresh slice sizing for VP9 / H.264, encoder
  hint passthrough for AV1).
- **Lifecycle.** When we are done with the buffer, we **must** call
  `callbacks->Done()`; otherwise the capturer's pool stalls. This is
  the single most common bug in implementations of this consumer.

Becoming a `webrtc::VideoFrameSource` is mechanical:

```cpp
void OnFrameCaptured(VideoBufferHandle data, VideoFrameInfo info,
                     gfx::Rect content_rect,
                     mojo::PendingRemote<FrameCallbacks> cb) {
  // 1. Wrap the shared memory / GMB as a media::VideoFrame.
  scoped_refptr<media::VideoFrame> frame =
      WrapAsMediaVideoFrame(std::move(data), info, content_rect);

  // 2. Convert media::VideoFrame -> webrtc::VideoFrame. libwebrtc
  //    has helpers for both I420 and NV12 buffers; on GMB paths the
  //    wrap is zero-copy.
  webrtc::VideoFrame wf = ToWebRtcVideoFrame(frame);

  // 3. Push to the libwebrtc track source (a custom
  //    rtc::AdaptedVideoTrackSource subclass).
  track_source_->OnFrame(wf);

  // 4. Release the underlying buffer once libwebrtc is done.
  //    The webrtc::VideoFrame holds a refcount on the
  //    media::VideoFrame, which holds the FrameCallbacks remote;
  //    Done() fires when that goes out of scope.
}
```

## 4. Performance expectations vs `getDisplayMedia`

What `getDisplayMedia` (T15) costs and `FrameSinkVideoCapturer` saves:

| Cost                                | getDisplayMedia | FrameSinkVideoCapturer |
|-------------------------------------|------------------|------------------------|
| Extra compositor pass (Viz → MediaStreamTrack consumer → encoder) | yes  | **no** — frames come straight from Viz to our consumer. |
| Frame-pacing control                 | none — capturer-side, opaque | full — `SetMinCapturePeriod` and we control consumption. |
| Damage-rect routing                  | unavailable      | available via `CAPTURE_UPDATE_RECT`. |
| Zero-copy GPU path                   | no — through MediaStream layer | yes — `kPreferGpuMemoryBuffer` + NV12. |
| Per-tab capture                      | awkward (tab-capture flag) | natural — `ChangeTarget(FrameSinkId)`. |

**Quantified expectations** (re-validate against T10/T11 once both
paths run on the harness):

- Per-frame copy cost: T29's screencast spike spent ~600 KB/frame
  serializing to PNG-over-CDP at 1280×720. The `FrameSinkVideoCapturer`
  zero-copy GMB path eliminates that whole step.
- Frame-pacing jitter: T29 measured stdev 9.85 ms at a 33 ms target
  (screencast). `SetMinCapturePeriod(60Hz)` plus our pull rate gives
  us deterministic pacing — the budget here is "stdev under 3 ms"
  on the chosen instance type. (re-validate)
- End-to-end glass-to-glass: PROJECT_BRIEF.md targets are LAN <100 ms,
  regional <200 ms. The Phase 2 commitment criterion is "harness
  shows ≥30 ms p95 reduction over Phase 1 on the same instance"
  (per `path-of-least-resistance.md` §4).

## 5. Encoder factory hookup

T19's `CloudBrowserVideoEncoderFactory` and the VP9 / H.264
encoders that plug into it (T35 / T36) take `webrtc::VideoFrame`
input. The capture path does not change the encoder factory — the
consumer described in §3 hands the encoder factory the same shape of
input as `getDisplayMedia` does.

What changes for the encoder factory in Phase 2:

- **Resolution change handling.** `getDisplayMedia` re-negotiates an
  entirely new track on size change; `FrameSinkVideoCapturer`
  silently changes its capture frame size when the underlying surface
  resizes. The encoder factory's existing re-init-on-size-change
  logic (see `vp9_encoder.cc::Encode`) handles this, but we should
  bench resize storms (DPR changes, devtools open/close) under the
  new capture path. (re-validate)
- **Scalability mode advertisement.** Once we own pacing, real-time
  SVC becomes practical. Revisit `QueryCodecSupport(format,
  scalability_mode)` at the same time as the AV1 work (T43).
- **Damage-rect hints (Phase 3+).** Optional. The encoder wrappers
  already apply intra-refresh; piping the damage rect through as a
  VP9 cyclic-refresh seed / x264 slice region is a Phase 3 follow-up
  — measure first.

## 6. Phased rollout

```
[Phase 1 today]  getDisplayMedia (T15 / T23) — production path.
                 Latency harness (T10/T11) measures it continuously.
                                  ↓
[Phase 2.0]      FrameSinkVideoCapturer behind a feature flag,
                 disabled by default. Internal builds only.
                 Both paths run; harness reports both numbers.
                                  ↓
[Phase 2.1]      Feature flag default-on for internal pool of testers
                 (~team-wide), getDisplayMedia still selectable.
                 Soak for ≥168 h (one week) — track stability and
                 latency regressions.
                                  ↓
[Phase 2.2]      Default-on for all users.
                                  ↓
[Phase 2.3]      getDisplayMedia path removed (deletion is a
                 separate, reversible commit so a roll-back stays
                 cheap until 2.3 lands).
```

**Cutover criteria, all required to advance:**

1. **Latency.** `FrameSinkVideoCapturer` shows ≥30 ms p95 reduction
   on glass-to-glass over `getDisplayMedia` measured by T10/T11 on
   the production target instance type, AND the absolute number is
   under the brief's regional budget (200 ms). Same threshold T15
   §4 set for the Phase-2 swap.
2. **Frame-pacing jitter.** Stdev under 3 ms at 30 fps target on
   the same instance type.
3. **Stability soak.** ≥168 h continuous internal use without a
   capturer-side crash or visible artifact regression. We track
   this via the metrics endpoint already added in T38.
4. **No encoder regression.** VP9 / H.264 bitrate target tracking
   stays within the ±10% envelope T35 / T36 set as the operational
   target.

If any criterion fails, we stay on `getDisplayMedia` and the failure
becomes the next Phase-2 task.

## 7. Cross-references

- `docs/capture/path-of-least-resistance.md` — what we are
  replacing; cutover criteria mirror its §4.
- `docs/build/chromium-from-source.md` — where the Chromium tree
  this code links against comes from.
- `capture/encoder/encoder_factory.h` /
  `capture/encoder/vp9_encoder.cc` /
  `capture/encoder/h264_encoder.cc` — the encoder factory the
  captured frames feed.
- `capture/spike-beginframe/findings.md` — why the
  `HeadlessExperimental.beginFrame` alternative is off the table
  and `FrameSinkVideoCapturer` got promoted.
- `docs/research/av1-encoders.md` — when AV1 lands, the same
  capture path feeds it; SVC becomes a real option once we own
  pacing.

## 8. Open questions

- **Multi-tenant frame sink isolation.** Phase 3 puts one Chromium
  per tenant; a single embedder process serving multiple Chromium
  child processes is *not* the path — but the reverse (one
  embedder per Chromium) is. Pin this in the Phase 3 sandbox doc
  (T44) before any code lands.
- **GPU memory buffer availability on chosen cloud instance type.**
  The `kPreferGpuMemoryBuffer` zero-copy path needs working GBM /
  DMA-buf in the container. Headless GPU on EC2 is sometimes a
  software-rasterized illusion. Verify on the chosen instance type
  before the Phase 2.0 prototype is benched.
- **WebGL / video-element compositing edge cases.** Embedded `<video>`
  elements with hardware overlay paths can land in separate frame
  sinks. We may need `RegisterFrameSinkHierarchy` to capture them
  alongside the page — measure on real workload first.

---

**Bottom line:** Approach A (embedder) is the path. The wire format
and lifecycle are well-understood; the work is the browser-process
glue, the consumer-side conversion to `webrtc::VideoFrame`, and the
flag-then-soak rollout. Numbers must come from the harness on the
production instance type — the spike on a Mac laptop (T29) is enough
to rule things *out*, not in.

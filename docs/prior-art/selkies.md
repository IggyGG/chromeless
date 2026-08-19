# Prior Art: Selkies-GStreamer

**Project:** Selkies (formerly Selkies-GStreamer)
**Repo:** https://github.com/selkies-project/selkies
**Docs:** https://selkies-project.github.io/selkies/
**License:** MPL-2.0

Selkies is the closest direct comparable to what we are building: an
open-source, Linux-native, low-latency WebRTC remote desktop platform aimed at
containers, Kubernetes, HPC, and cloud gaming. The architecture is
GStreamer-centric — the streaming pipeline is constructed from GStreamer
elements rather than libwebrtc — and the host-side glue is Python. Reading
their code first is the cheapest way to learn which Linux capture / encode
pitfalls are real and which are folklore.

---

## 1. Architecture

```
+----------------------+   +------------------+   +------------------+
| X11 desktop / Xvfb   |   | PulseAudio /     |   | Browser client   |
|  (or Wayland*)       |   | PipeWire monitor |   | (gst-web)        |
+-----------+----------+   +---------+--------+   +---------+--------+
            | ximagesrc /            | pulsesrc           ^
            | pixelflux capture      |                    | WebRTC SRTP +
            v                        v                    | data channel
   +---------------------------------------+   +----------+----------+
   | GStreamer pipeline (Python-built)     |   | Browser RTCPeerConn |
   |                                       |   +----------+----------+
   | video: nvh264enc | vah264enc |        |              ^
   |        x264enc | nvh265enc |          |              |
   |        vp8enc | vp9enc | nvav1enc     |              |
   | audio: opusenc                        |              |
   |                                       |              |
   | sink: webrtcbin                       +--- SDP/ICE ---+
   +-------------------+-------------------+              |
                       |                                  |
                       +---- Python signaling server -----+
                             (WebSocket; SDP/ICE relay,
                             coTURN / TURN-REST optional)
                       ^
                       | input events (data channel + WS)
                       | mouse/keyboard: uinput | XTest | pynput
                       | gamepad: virtual JS/EVDEV unix sockets
                       | clipboard: chunked WS messages
                       +---- input_handler.py
```

\* Wayland is a stated future direction; current production path is X11.
[design page](https://selkies-project.github.io/selkies/design/)

Source-of-truth files in their tree:

- `src/selkies/media_pipeline.py` — capture + encoder orchestration.
  ([file](https://github.com/selkies-project/selkies/blob/main/src/selkies/media_pipeline.py))
- `src/selkies/webrtc_signaling.py`, `signaling_server.py`, `rtc.py` — SDP /
  ICE, signaling server, peer connection management.
  ([dir](https://github.com/selkies-project/selkies/tree/main/src/selkies))
- `src/selkies/input_handler.py` — input fan-out to uinput / XTest / virtual
  gamepad sockets.
  ([file](https://github.com/selkies-project/selkies/blob/main/src/selkies/input_handler.py))
- `addons/gst-web/` — browser client (HTML5 + JS).
  ([historical dir](https://github.com/selkies-project/selkies/tree/382a462ca5b89983b84ad0e0947393efdc4fc774/addons/gst-web))

## 2. Strengths

- **GStreamer encoder coverage is excellent.** Out of the box they support
  `nvh264enc`, `vah264enc`, `x264enc`, `nvh265enc`, `vp8enc`, `vp9enc`,
  `nvav1enc`, all selectable via the `SELKIES_ENCODER` env var or
  `--encoder=`.
  ([component docs](https://selkies-project.github.io/selkies/component/))
  This is exactly the set we will eventually want to expose behind our own
  `webrtc::VideoEncoderFactory`, and the runtime selection model is worth
  copying.
- **Single-port deployment, container-first.** They prioritize working under
  Docker / K8s without privileged access and with one externally-exposed port,
  via webrtcbin + a single signaling WebSocket and optional TURN-REST.
  ([design](https://selkies-project.github.io/selkies/design/),
  [component](https://selkies-project.github.io/selkies/component/)). Our
  Phase 3 multi-tenancy story benefits from copying this constraint from
  day 1.
- **Runtime tuning of encode parameters.** `media_pipeline.py` exposes
  framerate, bitrate, CRF, CBR/CRF rate-control switching, and on-demand IDR
  insertion via async tasks rather than tearing down the pipeline.
  ([media_pipeline.py](https://github.com/selkies-project/selkies/blob/main/src/selkies/media_pipeline.py))
  This is the pattern we want for ABR and viewport-driven quality changes.
- **Audio is treated as a peer of video, not an afterthought.** Opus at 48
  kHz with 10 ms latency / 20 ms frame size is in the same pipeline and rides
  the same `RTCPeerConnection`. ([media_pipeline.py](https://github.com/selkies-project/selkies/blob/main/src/selkies/media_pipeline.py))
- **TURN-REST.** Time-limited credential issuance is a small, separable
  service; we should adopt their split (coTURN container + TURN-REST
  container) rather than reinvent it.
  ([component](https://selkies-project.github.io/selkies/component/))
- **Gamepad story.** Four virtual Xbox-360-shaped pads exposed via Unix
  domain sockets — interesting prior art for our Phase 2 input fidelity.
  ([input_handler.py](https://github.com/selkies-project/selkies/blob/main/src/selkies/input_handler.py))

## 3. Cut corners / known issues

- **GStreamer `webrtcbin` is not libwebrtc.** It is a GStreamer-native ICE +
  SRTP + RTP element; it lacks the years of congestion-control, FEC, RED,
  pacer, and bandwidth-estimator tuning that libwebrtc has accreted. The
  Selkies design doc explicitly calls webrtcbin out as the chosen transport.
  ([design](https://selkies-project.github.io/selkies/design/)) Our latency
  budget likely cannot tolerate this gap once we add ABR.
- **X11-only, with Wayland as a future plan.** Capture path is `ximagesrc`
  (or external `pixelflux` shim) tied to an X11 display server — fine for
  Xvfb, awkward for a Chromium-internal capture surface.
  ([component](https://selkies-project.github.io/selkies/component/))
- **Capture is desktop-shaped, not browser-shaped.** They scrape the
  framebuffer via `ximagesrc` / `pixelflux` rather than tapping any compositor
  output, so they cannot do per-tab capture, sub-window crops, or trust the
  browser's damage rectangles. This is a fundamental ceiling on efficiency
  for a single-tab cloud browser.
- **Input goes through several Linux abstractions.** uinput, X11 XTest, and
  pynput are layered; clipboard is chunked over WebSocket up to ~750 KB
  pieces. Works, but a lot of surface area for IME, modifier, and paste
  bugs. ([input_handler.py](https://github.com/selkies-project/selkies/blob/main/src/selkies/input_handler.py))
- **Python in the hot path.** `selkies.py` / `media_pipeline.py` keep the
  encode loop in Python with `asyncio.Lock` around pipeline restarts. Fine
  for 1080p60 H.264, but a dubious foundation for future
  multi-tenant-per-host density.
  ([media_pipeline.py](https://github.com/selkies-project/selkies/blob/main/src/selkies/media_pipeline.py))
- **Vendor-style dual capture libs.** `pixelflux` (video) and `pcmflux`
  (audio) are project-maintained shims wrapped by `MediaPipelinePixel`; they
  are not standard GStreamer plugins and add a maintenance burden.
  ([media_pipeline.py](https://github.com/selkies-project/selkies/blob/main/src/selkies/media_pipeline.py))

## 4. What we'd reuse

- **Encoder selection model.** A single `SELKIES_ENCODER`-style environment
  variable / flag that picks among `nvenc`, `vaapi`, `x264`, `vpx`, `av1` is
  a clean shape for our `VideoEncoderFactory` config.
  ([component](https://selkies-project.github.io/selkies/component/))
- **TURN-REST split.** Two containers, time-limited credentials, no shared
  secret in clients — adopt as-is for our infra layer.
  ([component](https://selkies-project.github.io/selkies/component/))
- **Runtime ABR knobs.** The `update_rate_control_mode()` /
  `dynamic_idr_frame()` shape from `media_pipeline.py` translates directly
  to libwebrtc encoder controls.
  ([media_pipeline.py](https://github.com/selkies-project/selkies/blob/main/src/selkies/media_pipeline.py))
- **Browser client outline.** `addons/gst-web` gives us a working reference
  for keymap, cursor, and clipboard wire formats even if our client is
  rewritten in TypeScript.
  ([historical gst-web](https://github.com/selkies-project/selkies/tree/382a462ca5b89983b84ad0e0947393efdc4fc774/addons/gst-web))
- **Audio configuration defaults.** Opus 48 kHz, 10 ms latency, 20 ms frame
  size is a safe default for our Phase 1 PulseAudio loopback.
  ([media_pipeline.py](https://github.com/selkies-project/selkies/blob/main/src/selkies/media_pipeline.py))

## 5. What we'd diverge on and why

- **Capture path: Chromium-internal, not X11 desktop.** Phase 2 hooks Viz's
  `FrameSinkVideoCapturer`, which gives us per-tab frames, damage rects, and
  exact frame timestamps — none of which `ximagesrc` can provide. Phase 1
  uses `getDisplayMedia` for the same reason: the bits are already inside
  Chromium. (See PROJECT_BRIEF.md Phase 1/2 and forthcoming
  `docs/capture/path-of-least-resistance.md`.)
- **Transport: libwebrtc, not webrtcbin.** We need GCC/BWE, FEC, RED, the
  pacer, and the option to inject our own encoder factory. Selkies'
  webrtcbin path forecloses most of that.
  ([design](https://selkies-project.github.io/selkies/design/))
- **Host-side process language.** Our hot path lives in Chromium / libwebrtc
  C++. Python is fine for signaling (Selkies puts it there too), but not
  for the encode loop.
- **One Chromium per tenant, not one desktop per tenant.** Selkies streams
  a whole X session; we stream one browser instance. This collapses the
  attack surface (no display server, no window manager, no apps other than
  Chromium) and lets us put gVisor / Firecracker around it.
  (PROJECT_BRIEF.md Risks table.)
- **Input via DevTools `Input.dispatch*`, not uinput / XTest.** For Phase 1
  we route input through Chromium DevTools Protocol; this skips an entire
  class of focus / IME / synthetic-event problems Selkies has to handle in
  `input_handler.py`. (PROJECT_BRIEF.md Phase 1.)
- **Latency harness from week 1.** Selkies advertises "60fps at Full HD" as
  the headline metric ([README](https://github.com/selkies-project/selkies/blob/main/README.md));
  we are committing to glass-to-glass latency budgets (LAN <100 ms, regional
  <200 ms) measured continuously. Different north star, different
  trade-offs.

---

**Bottom line:** Selkies is the right reference for encoder coverage, TURN
deployment shape, audio defaults, and runtime ABR ergonomics. It is the wrong
reference for capture path and transport — its X11 + webrtcbin foundation is
exactly what we are choosing not to inherit, and it justifies our Phase 2
investment in Chromium-native capture and a libwebrtc-based pipeline.

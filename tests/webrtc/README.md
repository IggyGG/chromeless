# tests/webrtc — WebRTC harness for chromeless

Standalone Node.js harness that drives the chromeless binary directly
via Chrome DevTools Protocol, plays the role of WebRTC counter-peer in
Node.js (using `@roamhq/wrtc`), records the inbound video stream to a
`.webm`, and (via the parallel-authored `encoder-assertions.mjs` module)
asserts that `outboundRtp.encoderImplementation` matches our embedded
encoders rather than chromium's stock fallbacks.

This is the **Test 2 / Test 3** layer from the validation pyramid in
`fizzy-beaming-shamir.md`. Test 1 (CDP smoke) lives in `../cdp/`.

## Why direct CDP and not platform deploy?

End-to-end via the platform takes ~20 minutes per iteration. Direct CDP
takes ~5 seconds. We caught the BrowserContext crash that broke
`Target.createBrowserContext` in five seconds with a manual CDP probe;
this harness codifies that loop so every chromium build can iterate the
same way.

## Recording is always produced

Every successful run writes a `.webm` and prints a `TEST_ARTIFACT` line
on stderr that CI runners can grep for:

```
TEST_ARTIFACT path=/abs/path/webrtc-vp9-20260503T142035.webm bytes=24382 frames=148 codec=vp9-libvpx wire=vp9
```

Default output path:

- If `TEST_ARTIFACTS_DIR` is set, the artifact lands in
  `${TEST_ARTIFACTS_DIR}/webrtc-<codec>-<UTC-timestamp>.webm`.
- Otherwise, `./artifacts/webrtc-<codec>-<UTC-timestamp>.webm`
  (the directory is auto-created).

Override with `--out=/path/to/file.webm` if you want to pin a name.

## What gets recorded

The streamer page (`streamer-page.html`) attaches the canvas demo
(`fixtures/demo.js`) to a 1280×720 canvas and captures a MediaStream
from it via `canvas.captureStream(30)`. This means:

1. **The recording shows real, varied content** — a multi-panel
   dashboard with a rotating spinner, animated bar chart, drifting
   particle field, and a scrolling event log.
2. **Recording works headless** — no Xvfb, no display picker, no
   `--auto-select-desktop-capture-source` flag needed. The canvas
   captureStream path is independent of the chromium GUI.
3. **The harness can steer the demo** — CDP messages
   (`set-scene`, `log-line`, `bar-set`, `flash`, `badge`) flow through
   `window.__cbtest.handle(...)` to the demo's `send()`, and the
   recording shows the response within one frame.

The encoder-identity assertion is unaffected — canvas-source MediaStreams
go through libwebrtc's encoder pipeline identically to camera or
display sources, so `encoderImplementation` still reports our embedded
factory strings (`cloud-browser-vp9-libvpx`, etc.).

To preview what the recording will look like, open
`fixtures/demo.html` in any local browser. Buttons trigger the same
messages the harness sends.

## Usage

```bash
# 1. Install deps (Node 20+ required; @roamhq/wrtc has prebuilds for
#    linux-x64-glibc and darwin-x64/arm64).
npm install

# 2. Default: hit the deployed cluster service. Artifact lands in
#    ./artifacts/ unless TEST_ARTIFACTS_DIR is set.
npm test

# 3. Override the target (e.g. local chromeless on your laptop).
node drive-recorder.mjs \
  --chromeless-url=http://127.0.0.1:9222 \
  --duration=10

# 4. Pin the wire codec under test.
node drive-recorder.mjs --codec=vp9   # default order; matches stock chromium
node drive-recorder.mjs --codec=h264
node drive-recorder.mjs --codec=av1

# 5. Drive a custom scene script (any subset of:
#    set-scene, log-line, bar-set, flash, badge).
node drive-recorder.mjs --steer-script=fixtures/default-steer-script.json

# 6. Pin a specific output file.
node drive-recorder.mjs --out=/tmp/my-recording.webm
```

The cluster Job (`infra/k8s/tests/chromeless-webrtc-validation.yaml`) passes
`--steer-script=fixtures/default-steer-script.json` so the artifact
walks through `boot → idle → streaming` with log lines, bar updates,
and a couple of flashes — the recording is visibly interesting even
without per-test customization.

## What the run produces

- A `.webm` file at `--out=` (or the default path) — open in QuickTime,
  mpv, or any browser. You should see the full demo dashboard
  animating: spinner, chart bars, particle field, log feed, badge
  changing per scene, plus any scripted log lines and flashes.
- Stderr log of:
  - CDP attach + target creation
  - SDP offer/answer exchange (sizes only; SDP isn't dumped)
  - First-frame timing
  - `outbound-rtp` summary at the end (with `encoderImplementation`)
  - `inbound-rtp` summary on the receiver side
  - **`TEST_ARTIFACT path=… bytes=… frames=… codec=…`** line
  - PASS / FAIL verdict

## Verification checklist

- `npm test` exits 0 (or `node drive-recorder.mjs` against a chosen
  `--chromeless-url`).
- The `TEST_ARTIFACT` line points at an existing `.webm` ≥ 10 KB.
- `ffprobe <path>` reports duration ≥ 4s, codec=vp9 (storage codec).
- Open `<path>` in mpv / QuickTime → demo panels are visible and
  animating, the badge transitions through `BOOT → IDLE → STREAMING`,
  log lines appear and scroll, no obvious frame corruption.
- The harness's stderr line `sender getStats(): outboundRtp.video summary`
  reports `encoderImplementation` matching one of:
  `cloud-browser-vp9-libvpx`, `cloud-browser-vp9-libvpx-lowlatency`,
  `cloud-browser-h264-x264`, `cloud-browser-h264-x264-lowlatency`,
  `cloud-browser-av1-svtav1`, `cloud-browser-av1-svtav1-lowlatency`.
  (Canonical table: `encoder-assertions.mjs`, sourced from
  `capture/encoder/{vp9,h264,svtav1}_encoder.cc`.)

## File map

| File | Purpose |
|---|---|
| `package.json` | Node deps. `@roamhq/wrtc` (native libwebrtc bindings + `RTCVideoSink`), `chrome-remote-interface`, `ws`. |
| `drive-recorder.mjs` | The harness. CDP attach → page navigate → console-bridge ↔ Runtime.evaluate signaling → `@roamhq/wrtc` peer → ffmpeg → structured `TEST_ARTIFACT` line. |
| `streamer-page.html` | The page loaded into chromeless. Attaches `fixtures/demo.js` to a visible canvas, captures the canvas via `canvas.captureStream(30)`, sends it through an `RTCPeerConnection`. Falls back to `getDisplayMedia` then `getUserMedia` if canvas capture isn't supported. |
| `fixtures/demo.js` | ES module exporting `attachDemo(canvas, opts)` — renders the multi-panel demo content (spinner, chart, particles, log, footer). The harness imports it; the standalone preview also imports it. |
| `fixtures/demo.html` | Standalone preview of the canvas demo. Open in any browser to see what the recording will look like; buttons drive the same `window.__cbtest.handle()` messages the harness uses. |
| `fixtures/default-steer-script.json` | Default scene script the cluster Job uses. Walks the demo through scene transitions + log lines + bar updates + flashes during the recording window. |
| `fixtures/spinner.html` | Original simple-spinner fixture. Kept for back-compat with anyone iterating against an older streamer page; not used by the current harness. |
| `encoder-assertions.mjs` | Module that parses `outboundRtp.encoderImplementation` from sender-side getStats() and asserts against the canonical table from `capture/encoder/encoder_factory_stub.cc`. The harness imports it dynamically — when the file is absent the harness runs in recording-only mode and prints a soft warning. |

## Test environment notes

- **Local mac dev:** `@roamhq/wrtc` ships prebuilds for darwin-arm64 and
  darwin-x64. `npm install` should not require building from source.
  ffmpeg via Homebrew (`brew install ffmpeg`).
- **Cluster Job (k8s):** runs on `node:20-bookworm-slim` with
  ffmpeg installed via apt. Manifests live in
  `infra/k8s/tests/chromeless-webrtc-validation.yaml`. The Job emits the
  artifact as base64 in Pod logs (between `===WEBM_BASE64_BEGIN===`
  and `===WEBM_BASE64_END===`) so an operator can decode and play
  it locally:

  ```bash
  kubectl logs -n chromeless-tests job/chromeless-webrtc-validation -c test-driver \
    | awk '/===WEBM_BASE64_BEGIN===/{f=1; next} /===WEBM_BASE64_END===/{f=0} f' \
    | base64 -d > out.webm
  ```

- **Alpine / musl:** unsupported; `@roamhq/wrtc` has no musl prebuild
  and building from source pulls libwebrtc which is multi-GB. Stick
  with Debian-based images.

## Steer script format

A JSON array; each entry is `{at: <ms>, send: {type, ...}}`. The `at`
field is the offset from first-frame in milliseconds. The `send`
payload is pushed via the same `window.__cbtest.handle(...)` bridge
the harness uses for SDP/ICE; the streamer page routes it to the
demo's `send()`.

```json
[
  {"at":     0, "send": {"type": "set-scene", "value": "streaming"}},
  {"at":   500, "send": {"type": "log-line",  "text": "first frame"}},
  {"at":  1000, "send": {"type": "bar-set",   "index": 0, "value": 0.92}},
  {"at":  1500, "send": {"type": "flash",     "color": "#5fb4ff"}},
  {"at":  2000, "send": {"type": "badge",     "text": "ENCODING"}}
]
```

## Architecture

```
┌─────────────────────────┐                    ┌─────────────────────┐
│  drive-recorder.mjs     │  CDP (websocket)   │   chromeless       │
│  (Node.js)              │ <──────────────────│   /json/version     │
│                         │                    │   browser-level ws  │
│   ┌─────────────────┐   │                    │                     │
│   │  http server    │   │ GET streamer-page  │                     │
│   │  127.0.0.1:N    │ <───────────────────── │ Page.navigate       │
│   │  (also serves   │   │                    │                     │
│   │   demo.js)      │   │                    │                     │
│   └─────────────────┘   │                    │                     │
│                         │  Runtime.evaluate  │                     │
│   ┌─────────────────┐   │ ──────────────────>│   streamer-page.html│
│   │  @roamhq/wrtc   │   │  (sdp, ice, demo   │   ┌─────────────┐   │
│   │  Peer + RTCVS   │ <─┐  steer messages)   │   │ <canvas>    │   │
│   └─────────────────┘   │                    │   │ + demo.js   │   │
│            │            │  consoleAPICalled  │   │ +captureStream  │
│            ▼            │ <──────────────────│   └─────────────┘   │
│   ┌─────────────────┐   │  (sdp offer, ice)  │   RTCPeerConnection │
│   │  ffmpeg stdin   │   │                    │                     │
│   │   I420 frames   │   │                    │                     │
│   └─────────────────┘   │                    │                     │
│            │            │                    │                     │
│            ▼            │                    │                     │
│   ${ARTIFACT}.webm      │                    │                     │
│   + TEST_ARTIFACT line  │                    │                     │
└─────────────────────────┘                    └─────────────────────┘
```

The signaling shortcut (page log lines + Runtime.evaluate) keeps the
test self-contained — there's no signaling server to deploy, no
WebSocket exposure surface, just CDP. `pollStatsUntilEncoded` and
`assertEncoderIdentity` (in `encoder-assertions.mjs`) close the loop on
encoder validation by calling `pc.getStats()` ON THE SENDER SIDE via
the same CDP session, which is where `encoderImplementation` is
populated by the embedded `EncoderInfo` from
`capture/encoder/{vp9,h264,svtav1}_encoder.cc`.

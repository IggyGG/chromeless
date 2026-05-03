# tests/webrtc — WebRTC harness for cb-chromium

Standalone Node.js harness that drives the cb-chromium binary directly
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

## Usage

```bash
# 1. Install deps (Node 20+ required; @roamhq/wrtc has prebuilds for
#    linux-x64-glibc and darwin-x64/arm64).
npm install

# 2. Default: hit the deployed cluster service.
npm test -- --duration=5 --out=/tmp/out.webm

# 3. Override the target (e.g. local cb-chromium on your laptop).
node drive-recorder.mjs \
  --cb-url=http://127.0.0.1:9222 \
  --duration=10 \
  --out=/tmp/out.webm

# 4. Pin the wire codec under test.
node drive-recorder.mjs --codec=vp9   # default order; matches stock chromium
node drive-recorder.mjs --codec=h264
node drive-recorder.mjs --codec=av1
```

## What the run produces

- `out.webm` (or whatever `--out=` you passed) — a real video file you
  can open in QuickTime, mpv, or anywhere else. You should see the
  spinner from `fixtures/spinner.html` rotating with a ticking
  timestamp.
- Stderr log of:
  - CDP attach + target creation
  - SDP offer/answer exchange (sizes only; SDP isn't dumped)
  - First-frame timing
  - `outbound-rtp` summary at the end (with `encoderImplementation`)
  - `inbound-rtp` summary on the receiver side
  - PASS / FAIL verdict

## Verification checklist

- `node drive-recorder.mjs --duration=5 --out=/tmp/out.webm` exits 0
- `ls -la /tmp/out.webm` shows ≥10 KB
- `ffprobe /tmp/out.webm` reports duration ≥4s, codec=vp9 (storage codec)
- Open `/tmp/out.webm` in mpv / QuickTime → the spinner is visible and
  rotating, timestamp counts up, no obvious frame corruption.
- The harness's stderr line `sender getStats(): outboundRtp.video summary`
  reports `encoderImplementation` matching one of:
  `cloud-browser-vp9-libvpx`, `cloud-browser-vp9-libvpx-lowlatency`,
  `cloud-browser-h264-x264`, `cloud-browser-h264-x264-lowlatency`,
  `cloud-browser-svtav1`, `cloud-browser-svtav1-lowlatency`. (See the
  `encoder-assertions.mjs` module for the canonical table — sourced
  from `capture/encoder/{vp9,h264,svtav1}_encoder.cc`.)

## File map

| File | Purpose |
|---|---|
| `package.json` | Node deps. `@roamhq/wrtc` (native libwebrtc bindings + `RTCVideoSink`), `chrome-remote-interface`, `ws`. |
| `drive-recorder.mjs` | The harness. CDP attach → page navigate → console-bridge ↔ Runtime.evaluate signaling → `@roamhq/wrtc` peer → ffmpeg. |
| `streamer-page.html` | The page loaded into cb-chromium. Forked from `scrapebrowse:capture/streamer-page/streamer.js`'s getDisplayMedia + RTCPeerConnection + `window.pc`-hook pattern; signaling layer swapped from WebSocket to `console.log("CBTEST:…")` ↔ `window.__cbtest.handle(…)`. |
| `fixtures/spinner.html` | Animated CSS spinner + timestamp. Loaded as the page captured by `getDisplayMedia` so the recording has real motion to encode. Self-contained; no external fetches. |
| `encoder-assertions.mjs` | **Authored in parallel by `encoder-assertion-author`.** Module that parses `outboundRtp.encoderImplementation` from sender-side getStats() and asserts against the canonical table from `capture/encoder/encoder_factory_stub.cc`. The harness imports it dynamically — when the file is absent the harness runs in recording-only mode and prints a soft warning. |

## Test environment notes

- **Local mac dev:** `@roamhq/wrtc` ships prebuilds for darwin-arm64 and
  darwin-x64. `npm install` should not require building from source.
  ffmpeg via Homebrew (`brew install ffmpeg`).
- **Cluster Job (k8s):** runs on `node:20-bookworm-slim` with
  ffmpeg installed via apt. Manifests live in
  `infra/k8s/tests/cb-webrtc-validation.yaml` (authored by
  `k8s-manifest-author`).
- **Alpine / musl:** unsupported; `@roamhq/wrtc` has no musl prebuild
  and building from source pulls libwebrtc which is multi-GB. Stick
  with Debian-based images.

## Architecture

```
┌─────────────────────────┐                    ┌─────────────────────┐
│  drive-recorder.mjs     │  CDP (websocket)   │   cb-chromium       │
│  (Node.js)              │ <──────────────────│   /json/version     │
│                         │                    │   browser-level ws  │
│   ┌─────────────────┐   │                    │                     │
│   │  http server    │   │  GET streamer-page │                     │
│   │  127.0.0.1:N    │ <───────────────────── │ Page.navigate       │
│   └─────────────────┘   │                    │                     │
│                         │  Runtime.evaluate  │                     │
│   ┌─────────────────┐   │ ──────────────────>│   streamer-page.html│
│   │  @roamhq/wrtc   │   │  (sdp, ice push)   │   getDisplayMedia + │
│   │  Peer + RTCVS   │ <─┐                    │   RTCPeerConnection │
│   └─────────────────┘   │  consoleAPICalled  │                     │
│            │            │ <──────────────────│   "CBTEST:..." log  │
│            ▼            │  (sdp offer, ice)  │                     │
│   ┌─────────────────┐   │                    │                     │
│   │  ffmpeg stdin   │   │                    │                     │
│   │   I420 frames   │   │                    │                     │
│   └─────────────────┘   │                    │                     │
│            │            │                    │                     │
│            ▼            │                    │                     │
│       out.webm          │                    │                     │
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

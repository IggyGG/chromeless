# Capture path of least resistance: `getDisplayMedia` for Phase 1

**Status:** Phase 1 design (per PROJECT_BRIEF.md).
**Owner:** chromium-dev.
**Cross-references:** `docs/prior-art/selkies.md` (T3),
`docs/build/chromium-from-source.md` (T17), `PROJECT_BRIEF.md` Phase 1.

This document is the design for **how Phase 1 captures pixels**. It commits
us to a deliberately boring approach — `navigator.mediaDevices.getDisplayMedia()`
inside the headless Chromium, fed straight into an `RTCPeerConnection` to
the user's browser — and lays out the cost we are paying for that simplicity.
This doc is meant to be questioned.

---

## 1. The plan

Inside the headless Chromium instance running in our container we open a
small **privileged page** ("the streamer") served by a tiny HTTP server
inside the same container. That page does three things:

1. Calls `navigator.mediaDevices.getDisplayMedia({ video: true, audio: true })`,
   which Chromium auto-grants as "the entire screen of the headless display"
   (see §2 for the flags).
2. Adds the resulting `MediaStream` tracks to a `RTCPeerConnection`.
3. Negotiates SDP / ICE with the user's browser via our WebSocket signaling
   server (see T13).

The user's browser is the remote peer of that `RTCPeerConnection`. Once
SDP/ICE complete, Chromium's own libwebrtc encodes the captured stream and
ships SRTP packets directly to the user. **No C++ from us, no Chromium
fork, no custom build.**

```
+-------------------------------------------------------+
| Container                                             |
|                                                       |
|  Xvfb display ──┬── (target)Chromium tabs (1..N)      |
|                 │      shows the cloud page           |
|                 │                                     |
|                 └── streamer page (in same Chromium): |
|                       getDisplayMedia()               |
|                          │                            |
|                          ▼                            |
|                       RTCPeerConnection ─── SRTP ───►  user's browser
|                          ▲                            |
|                          │ SDP/ICE                    |
|                          ▼                            |
|                  WebSocket signaling server (T13) ◄── user's browser
+-------------------------------------------------------+
```

Why this is path-of-least-resistance:

- **No C++.** All glue is HTML + JS + Go. We avoid `gn gen`,
  `autoninja`, depot_tools, the 2-hour build, and the patch series
  altogether (per `docs/build/chromium-from-source.md` — that work is
  Phase 2).
- **All capture / encode happens inside libwebrtc** that is already
  shipped in stock Chromium and proven on billions of users. Selkies got
  this wrong (they use webrtcbin instead, see T3) and pay for it in
  congestion-control gaps.
- **Validates the architecture end-to-end.** Signaling, ICE, audio, input
  round-trip, and the latency harness all get exercised on a real WebRTC
  pipeline. If any of those are broken, we want to know in week 1, not
  after we've finished a Chromium fork.
- **Reversible.** When we replace `getDisplayMedia` with a Viz hook in
  Phase 2, the SDP/ICE/peer-connection wiring and the entire user side
  stay identical. The capture surface is a single `MediaStreamTrack`; it
  doesn't matter to anything downstream how we got it.

## 2. How it gets wired up

Three pieces, one container:

### 2.1 The streamer page

A small static bundle served on `http://localhost:9000/streamer.html`
inside the container. It is:

- A page with no UI; opened with `chromium --app=http://...` so it
  doesn't show chrome.
- Sized to match the Xvfb display.
- Loaded with the WebSocket URL of our signaling server in a query
  parameter.

Pseudo-code:

```js
const ws = new WebSocket(params.get('signal'));
const stream = await navigator.mediaDevices.getDisplayMedia({
  video: { frameRate: 60 },
  audio: true,
});
const pc = new RTCPeerConnection({ iceServers: ICE_SERVERS });
stream.getTracks().forEach(t => pc.addTrack(t, stream));
pc.onicecandidate = e => e.candidate && ws.send(...);
ws.onmessage = msg => { /* offer/answer/candidate routing */ };
const offer = await pc.createOffer();
await pc.setLocalDescription(offer);
ws.send({type:'offer', sdp: offer.sdp});
```

We will keep this under 200 lines of JS in `client/streamer/`. It is not
the user-facing client (that lives in `client/`, T14) — it is a
server-side worker that happens to be HTML.

### 2.2 Chromium launch flags

The privileged page must auto-accept `getDisplayMedia` without a picker.
The relevant flags (verified against the Chromium command-line switch
reference at https://peter.sh/experiments/chromium-command-line-switches/):

```bash
chromium \
  --no-first-run \
  --no-default-browser-check \
  --disable-features=TranslateUI,MediaRouter \
  \
  # Display + audio
  --window-size=1920,1080 \
  --window-position=0,0 \
  --autoplay-policy=no-user-gesture-required \
  \
  # Auto-grant getDisplayMedia / getUserMedia for the streamer origin
  --use-fake-ui-for-media-stream \
  --auto-select-desktop-capture-source="Entire screen" \
  --auto-accept-this-tab-capture \
  --enable-usermedia-screen-capturing \
  \
  # Ensure http://localhost:9000 is treated as a secure context
  --unsafely-treat-insecure-origin-as-secure=http://localhost:9000 \
  \
  # Open the streamer as an app (no chrome chrome)
  --app=http://localhost:9000/streamer.html?signal=ws://localhost:8080/ws
```

Notes:

- `--auto-select-desktop-capture-source="Entire screen"` is documented as
  "should only be used for tests" — that wording is fine for our
  purposes; the container *is* a controlled test environment and there
  is no human to display a picker to.
- `--use-fake-ui-for-media-stream` is the belt to that suspenders: it
  bypasses the permission dialog entirely so even tab/window capture
  paths don't block.
- The exact spelling of `--auto-select-desktop-capture-source` has
  changed across Chromium versions; verify on the pinned branch (T17 §6)
  before merging the Dockerfile (T7).

### 2.3 Signaling and exit

- **Signaling.** Streamer connects to the same WebSocket server (T13) the
  user connects to, but identifies itself as `role=streamer`. The server
  pairs one streamer with one user per session.
- **Exit.** When the WebSocket closes (user disconnect, server shutdown,
  signal from supervisord), the streamer page calls `pc.close()` and
  `window.close()`. Supervisord brings the whole Chromium down on session
  end; we do not try to recycle Chromium across sessions in Phase 1
  (PROJECT_BRIEF.md Phase 3 Risks: "Never share Chromium across users").

## 3. Limitations we accept

`getDisplayMedia` is not free. The losses, roughly in order of
"how much do we care":

1. **Extra compositor pass.** Frames already drawn by Viz are read back
   into a `MediaStreamTrack` and re-fed into Chromium's encoder pipeline.
   That round trip costs a frame or two of latency and adds GPU/CPU
   work. A direct `FrameSinkVideoCapturer` hook (Phase 2) skips the
   extra copy.
2. **No damage-rect routing.** Viz knows which rectangles changed each
   frame; `getDisplayMedia` does not surface that to JS. We will encode
   full frames every IDR and let the encoder rediscover stationarity.
3. **No zero-copy.** GPU-side textures get copied to a CPU-readable
   `MediaStreamTrack` before encode. On NVENC / VAAPI hardware paths
   (Phase 4) this is the difference between encode latency in
   single-digit ms vs tens of ms.
4. **Frame pacing is the WebContents capturer's, not ours.** It targets
   a max framerate; under load it will silently drop frames in ways we
   cannot inspect. (Selkies sidesteps this by reading X11 directly; see
   `docs/prior-art/selkies.md`.) For Phase 1 we accept this and measure
   it via T10/T11.
5. **Per-tab capture is cumbersome.** `getDisplayMedia` captures the
   whole display by default; tab capture is a separate API
   (`chrome.tabCapture` / `getDisplayMedia({preferCurrentTab:true})`)
   with different permission flow. Phase 1 captures the whole display;
   Phase 2 capture-per-tab is exactly what `FrameSinkVideoCapturer`
   gives us cleanly.

**Numbers we don't have yet.** Selkies' README advertises "60fps Full HD"
without latency numbers; their docs site doesn't quantify the
`ximagesrc` → webrtcbin cost (see T3 — section "Strengths" and
"Cut corners"). Neko's writeup is similarly qualitative. Our own
harness (T10/T11) is the only credible source for the latency delta
between `getDisplayMedia` and a Viz hook on our hardware. Don't accept
folklore; measure.

## 3a. T86 status update — Phase 1 dev stack uses synthetic media

When this doc was written we assumed real `getDisplayMedia` against
Xvfb under stock `chromium 147` would work after the T78 launch-flag
fix (X11 ozone + SwiftShader + Vulkan disabled). It does not — the screen
capturer still throws `NotReadableError: Could not start video
source` even with the X11 ozone backend confirmed in the UA.
Investigation under T86 ruled out Xvfb, ozone selection, picker
flags, and the four GPU flags T78 added. The remaining hypothesis
is a Chromium-147-side regression in the X11 desktop capturer that
we are not in a position to fix from launch-flag space alone.

**The dev compose stack therefore defaults
`CHROMELESS_USE_FAKE_MEDIA=1`**, which routes `getDisplayMedia`
through `--use-fake-device-for-media-stream` to Chromium's
synthetic test pattern + tone. The streamer page sees a normal
`MediaStream`; the rest of the WebRTC pipeline (encoder, signaling,
RTP, client receive) runs against real encoded video and audio.

This is a deliberate stop-gap, not a fix. **Phase 2's
`FrameSinkVideoCapturer` (T47, T55) does not go through
`getDisplayMedia` at all**, so the durable fix lives there. Until
Phase 2 capture is wired into the running stack, the dev compose
runs against synthetic media; demos that need real-content video
either accept the bug (and the synthetic source) or land Phase 2.

The §3 limitations below still describe what `getDisplayMedia`
*would* cost relative to the Phase 2 hook if it worked; nothing
in §4's exit criteria changes — Phase 2 wins on its own merits,
not because of T86. The exit criteria stand even if someone fixes
Chromium 147's X11 capturer tomorrow.

## 4. Exit criteria for switching to custom capture

We will replace `getDisplayMedia` with a `FrameSinkVideoCapturer` hook
when **all** of the following are true:

1. **Measured latency exceeds budget on target hardware.** Glass-to-glass
   on the Phase 1 path is above the LAN <100ms or regional <200ms target
   (PROJECT_BRIEF.md), as measured by the harness on our chosen instance
   type (T10/T11).
2. **A Phase-2 capture spike (separate prototype branch, per
   PROJECT_BRIEF.md Phase 2) shows clear improvement.** Same hardware,
   same harness, same content: at least a 30 ms reduction or a
   meaningful drop in p99 frame-drop rate. Speculation does not count.
3. **The maintenance cost is justified.** A `FrameSinkVideoCapturer`
   hook is a forever-patch in our patch series (T17 §6). We only take
   that on if (1) and (2) clear.

If any of those fails, we stay on `getDisplayMedia` and keep the
optimization budget for things that actually move the latency number
(encoder tuning, GOP structure, JBP, pacing).

## 5. Cross-link

For comparison with how a desktop-shaped streamer handles the same
problem differently, see `docs/prior-art/selkies.md`. Selkies skips
this whole question by living outside Chromium and capturing X11 with
`ximagesrc` — which is also why they cannot do per-tab capture or
damage-rect routing, and why they sit on `webrtcbin` instead of
libwebrtc. Our Phase 1 makes the opposite trade: stay inside Chromium,
inherit libwebrtc, accept the extra compositor pass, and revisit only
when measurements demand it.

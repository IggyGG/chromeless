# Audio routing inside the cloud-browser-webrtc container

This document describes how audio flows from a tab playing inside our
headless Chromium to a `RTCPeerConnection` outbound-rtp track on the
streamer page. It is the source of truth for the pulse and Chromium
audio configuration; if you change `infra/pulse-default.pa`,
`infra/supervisord.conf`, or any audio-related Chromium flag, update
this doc in the same commit.

## Goals

1. **No host audio.** Nothing Chromium plays may reach the host's real
   audio devices. We accomplish this by replacing
   `/etc/pulse/default.pa` with a script that loads only null sinks — no
   ALSA, no PipeWire bridge, no UNIX-to-host audio modules.
2. **Capturable playback.** Whatever Chromium plays must be readable by
   `getUserMedia({audio:true})` / `getDisplayMedia({audio:true})` from
   inside the same Chromium process, so the streamer page (T23) can
   forward it as an RTC audio track.
3. **No self-feedback.** Capturing audio must not loop captured samples
   back into the playback sink, even if a misconfigured module accepts
   the default routing.

## Topology

```
                               +-----------------------------+
   audio in tab ----------->   |  cb_audio    (null-sink)    |
   (HTMLMediaElement,          |  default playback sink      |
    WebAudio, MSE, …)          +--------------+--------------+
                                              |
                                  cb_audio.monitor (source)
                                              |
                       module-loopback (latency_msec=20)
                                              |
                                              v
                               +-----------------------------+
                               |  cb_capture  (null-sink)    |
                               |  (audio-only intermediary)  |
                               +--------------+--------------+
                                              |
                                cb_capture.monitor (source)
                                              |
                                              v
                       getUserMedia({audio:true}) on the streamer page
                       getDisplayMedia({audio:true}) on Linux/PulseAudio
                                              |
                                              v
                          RTCPeerConnection outbound-rtp (audio)
```

A separate, isolated `virtual_mic` null-source exists for tests that
specifically want silence as their input device.

## Why two null sinks instead of one?

The naive layout — Chromium plays into `cb_audio`, default source set to
`cb_audio.monitor`, captures audio directly off the same sink — is one
errant `module-loopback source=cb_audio.monitor` away from a feedback
loop, because pulse loopback's default sink is the system default sink.
Routing the capture through a *separate* `cb_capture` sink means even if
something (a future add-on, a debugging session, an experimental module)
loads a loopback against the default source, the worst case is silence,
not a runaway echo.

## Chromium flags that matter for audio

| Flag                                          | Where set                | Why                                           |
| --------------------------------------------- | ------------------------ | --------------------------------------------- |
| `--autoplay-policy=no-user-gesture-required`  | `infra/launch-chromium.sh` | Allows the streamer page to start audio playback without a synthetic click. |
| `--use-fake-ui-for-media-stream` (set in T28) | `infra/launch-chromium.sh` | Auto-grants the getUserMedia/getDisplayMedia permission picker. **Does NOT synthesize media** — that's `--use-fake-device-for-media-stream`, which we deliberately omit so real PulseAudio routes through `cb_audio.monitor` end up in the captured track. |
| `--use-fake-device-for-media-stream` (NOT set)| `infra/launch-chromium.sh` | Deliberately absent — would feed Chromium's synthetic media (sine-wave audio, green-circle video) into capture, bypassing our PulseAudio routing. |
| `PULSE_SERVER=unix:/run/user/1000/pulse/native` | `infra/Dockerfile` ENV  | Pins Chromium to the in-container PulseAudio socket so it cannot fall through to host audio if the env is inherited. |

## Manual smoke test

Until the container fully boots end-to-end with T23's streamer page,
this manual procedure verifies the audio path:

1. Start the container:
   ```
   docker run --rm -p 9222:9222 cloud-browser-webrtc:dev
   ```
2. Exec into it as `cbuser` and confirm the sinks are loaded:
   ```
   docker exec -u cbuser <id> pactl list short sinks
   docker exec -u cbuser <id> pactl list short sources
   docker exec -u cbuser <id> pactl info | grep "Default Sink\|Default Source"
   ```
   Expected: a sink named `cb_audio`, a sink named `cb_capture`, default
   sink = `cb_audio`, default source = `cb_capture.monitor`.
3. Open Chrome DevTools at `http://localhost:9222` against the running
   Chromium and navigate to a YouTube video.
4. From an inspector console attached to that tab, run:
   ```javascript
   const stream = await navigator.mediaDevices.getUserMedia({audio:true});
   const ac = new AudioContext();
   const src = ac.createMediaStreamSource(stream);
   const an = ac.createAnalyser();
   src.connect(an);
   const buf = new Float32Array(an.fftSize);
   setInterval(() => { an.getFloatTimeDomainData(buf); console.log(Math.max(...buf.map(Math.abs))); }, 250);
   ```
5. While the video is playing, the printed peak amplitude should be
   non-zero. While the video is paused, it should drop to ~0 within
   one second.

## Automated smoke

`tests/smoke/audio-presence.sh` queries Chromium's DevTools protocol on
the streamer page (T23 contract: `window.pc` is the active
`RTCPeerConnection`) and asserts at least one outbound-rtp audio stream
with `bytesSent > 0`. It will fail until T23 lands a streamer page —
that's expected.

## Open questions / TODOs

- `getDisplayMedia({audio:true})` on Linux historically captures only
  the source associated with the display media's tab, not the system
  default. T23 should verify that the captured audio matches what
  Chromium plays into `cb_audio`; if not, we may need a per-tab routing
  module rather than the default-source approach.
- Echo cancellation, AGC, and noise suppression are off by default for
  the loopback path. For Phase 2 (real users with mics in the room
  watching the cloud browser), revisit whether we want EC enabled
  per-stream.
- A future task will add `pactl` health checks to supervisord so that
  if `cb_audio` or `cb_capture` is somehow unloaded at runtime, the
  pulse program is restarted instead of silently entering a degraded
  state.

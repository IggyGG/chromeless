# Webcam + microphone passthrough (Phase 4)

> **Status:** Phase 4 (T81). v1 stop-gap — the host-DaemonSet
> deployment is the path that scales; the privileged-container path
> documented here is for compose / single-node development only.
>
> **Owner:** `platform-dev` (this doc + client + streamer ontrack).
> Native v4l2-writer helper is implemented — see
> [`capture/v4l2-writer/`](../../capture/v4l2-writer/) (T92).

This doc describes how the local user's camera and microphone reach
the cloud Chromium tab so a page like Google Meet inside the cloud
browser sees a real `getUserMedia()` device.

---

## The two paths we considered

### Path A — extra tracks on the existing peer connection (chosen)

```
   client browser                                   cloud Chromium pod
   ┌────────────────────────┐                      ┌────────────────────────────┐
   │ client/main.ts         │                      │ streamer.js (privileged    │
   │   passthrough.ts       │                      │   page launched by         │
   │                        │                      │   --auto-select-desktop-…) │
   │ navigator.media        │                      │                            │
   │ Devices.getUserMedia() │                      │ pc.ontrack = (e) => ...    │
   │   ↓ MediaStreamTrack   │                      │   ↓ track                  │
   │ pc.addTrack(t, stream) │                      │   InsertableStreams        │
   │   ↓                    │                      │   per-frame transform      │
   │ negotiationneeded      │ ── SDP renegotiate ──▶│   ↓ Y'CbCr 4:2:0 frames    │
   │   ↓                    │ ──── ICE / SRTP  ────▶│ unix:/run/cb-passthrough/  │
   │ track is now a sender  │                      │   video.sock + audio.sock  │
   └────────────────────────┘                      │   ↓                        │
                                                   │ v4l2-writer helper (Go)    │
                                                   │   writes to /dev/video10   │
                                                   │   pulse-loopback for audio │
                                                   │   ↓                        │
                                                   │ Chromium's getUserMedia    │
                                                   │ device list now contains   │
                                                   │ "Cloud Browser Camera"     │
                                                   └────────────────────────────┘
```

The selling points of Path A:

1. **Reuses the existing peer connection.** No second negotiation,
   no parallel SCTP/SRTP context, no extra signaling envelopes
   beyond the existing `request_renegotiate` (T37 protocol).
2. **Real codecs, real bandwidth estimation.** The libwebrtc
   bandwidth-estimator (T58) sees the upstream track natively and
   can throttle the user's outbound camera bitrate the same way it
   throttles the downstream remote-tab bitrate.
3. **Encrypted in flight.** Camera/mic content travels over SRTP,
   not over an additional data channel that we'd have to
   re-encrypt or trust the SCTP layer for.

### Path B — encoded media over a data channel

The page receives encoded chunks over an RTC data channel,
decodes them in JS, and pushes raw frames to the v4l2 sink. Worse
on every dimension: extra encode → decode round-trip, no native
BWE coupling, more JS-side plumbing. **Rejected.**

---

## v1 streamer-side plumbing

Inside the streamer page we use **WebRTC Insertable Streams** —
the encoded-frame transform API
(`RTCRtpReceiver.createEncodedStreams()`) — running in a worker.
The worker:

1. Decodes the incoming `EncodedVideoFrame` / `EncodedAudioFrame`
   stream into raw frames via a transform.
2. Writes each frame to a Unix domain socket
   (`/run/cb-passthrough/video.sock` and `…/audio.sock`).

A native helper ([`capture/v4l2-writer/`](../../capture/v4l2-writer/),
T92) listens on those sockets and writes:
- Video frames into a `v4l2loopback` device (`/dev/video10` by
  default) using `VIDIOC_S_FMT` + `write(2)`.
- Audio frames into a PulseAudio null-sink that's exposed as a
  source via `module-virtual-source` — same pattern as the
  Chromium audio capture path from T24.

The streamer JS does NOT touch the v4l2 device directly. v4l2 ioctl
isn't reachable from a sandboxed Chromium renderer; the helper is
the privileged piece, not the page.

### Frame wire format (Unix socket → v4l2-writer)

Length-prefixed framing — 8 bytes header, body follows:

```
+────────────────+────────────────+──────────────────────────+
│  4B BE size    │  4B BE pts_ms  │  size bytes of frame     │
+────────────────+────────────────+──────────────────────────+
```

Video frame body is **I420** (`YUV420p`) at the resolution
negotiated by the upstream codec. The first frame on the socket is
preceded by a 16-byte init header `MAGIC(4) | width(4) | height(4) | fps(4)`
so the writer can call `VIDIOC_S_FMT` once.

Audio frame body is **interleaved S16LE** at 48 kHz mono /
2-channel; the init header is `MAGIC(4) | sample_rate(4) | channels(4) | reserved(4)`.

Both magic constants are also documented in
[`capture/v4l2-writer/README.md`](../../capture/v4l2-writer/README.md)
(T92).

---

## v1 deployment shapes

### Shape 1 — privileged container (compose dev, single-node)

```yaml
# infra/compose.yaml — chromium service
chromium:
  image: cloud-browser-webrtc:dev
  privileged: true              # required for `modprobe v4l2loopback`
  volumes:
    - cb-passthrough:/run/cb-passthrough
```

The container's entrypoint runs:

```sh
# infra/scripts/init-passthrough.sh
modprobe v4l2loopback \
  devices=1 \
  video_nr=10 \
  card_label="Cloud Browser Camera" \
  exclusive_caps=1
```

This is **explicitly a stop-gap**. Privileged containers blow open
most of the per-tenant isolation we built in T57. Use only on a
trusted single-node host.

### Shape 2 — host DaemonSet (production-shaped)

A Kubernetes DaemonSet runs once per node and `modprobe`s
`v4l2loopback` with N pre-allocated devices (`devices=64
video_nr=10,11,12,...`). Sessions then bind one device per Pod via
a device plugin or a host-path volume:

```yaml
# infra/k8s/cb-passthrough-loader.yaml — DaemonSet (TODO follow-up)
# loads v4l2loopback once per node; exposes /dev/videoN devices as
# part of a kubelet device-plugin claim:
spec:
  containers:
    - name: cb-passthrough-loader
      image: ghcr.io/.../cb-passthrough-loader:dev
      securityContext:
        privileged: true   # only the DaemonSet is privileged
```

Individual session Pods then claim a device via a hostPath mount or
a device plugin allocation — the **session Pod itself remains
non-privileged**. This is the path that scales.

The session manifest (T50/T71) gets a `cb.passthrough/device-claim`
annotation that the controller fills in at scheduling time.

---

## Renegotiation

Adding a track to an established `RTCPeerConnection` triggers a
`negotiationneeded` event. The client (offerer) must:

1. Create a new offer via `pc.createOffer()` — recvonly + sendrecv
   m= sections appear in the new SDP.
2. `setLocalDescription(offer)`.
3. Send the offer to the streamer over signaling using the existing
   envelope shape.
4. Wait for the streamer's `answer`, then `setRemoteDescription`.

The signaling layer doesn't need a new envelope type — the existing
`offer` / `answer` envelopes already carry post-renegotiation SDPs.
However, we use the T37 `request_renegotiate` protocol to ask the
streamer to *initiate* the renegotiation in cases where the client
isn't the offerer (per the T34 answerer-flip work). In v1 the
client adds tracks and triggers the renegotiation directly.

---

## Permissions and consent

### Client-side

`navigator.mediaDevices.getUserMedia({video, audio})` triggers the
**user-agent's** permission prompt — we do not bypass this, ever.
The user explicitly grants the browser access to camera/mic.

### Cloud-Chromium-side

The streamer page sees the inbound track as a regular
`MediaStreamTrack`. To make the v4l2/pulse loopback show up to a
user-loaded page (e.g., Google Meet) inside the same Chromium, that
page calls its OWN `getUserMedia()` against the loopback device.

For local-dev compose:
- Pass `--use-fake-ui-for-media-stream` to Chromium so the picker
  auto-selects the loopback without a user prompt. This is a
  development-only flag — in production we configure
  `permissions.devices.video.allowed_devices` via Chromium policy
  to scope the camera grant to the loopback device only, and
  surface a *single* prompt to the user via the streamer page when
  the inner page first calls `getUserMedia`.

The privileged streamer page is the only context that talks to
v4l2; user-loaded pages inside the cloud Chromium tab see only the
loopback's name (`"Cloud Browser Camera"`). They cannot enumerate
the host's real devices.

---

## Opt-out by default

Per the threat model in
[`docs/security/passthrough-threat-model.md`](../security/passthrough-threat-model.md),
camera/mic passthrough is **off by default**. It activates only
when:

1. The user has clicked the explicit "Share camera/mic" button in
   the client UI (no auto-enable from URL params or storage).
2. The user has granted browser-level permission via the UA prompt.
3. The streamer page was launched with `?passthrough=true` (config
   from the orchestrator, not user-controllable).
4. The session pod was scheduled with the
   `cb.passthrough/device-claim` annotation (or, in compose dev,
   `passthrough: true` in the env).

All four gates must be true for any media to leave the client.

---

## Cross-references

- T34 — answerer-flip; relevant for who triggers renegotiation.
- T37 — `request_renegotiate` envelope; reused here.
- T58 — libwebrtc BWE; sees upstream tracks natively under Path A.
- T24 — PulseAudio plumbing; the audio loopback follows the same shape.
- [`docs/security/passthrough-threat-model.md`](../security/passthrough-threat-model.md)
  — attack surface, opt-out story, per-tenant configuration.

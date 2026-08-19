# chromeless

[![CI](https://forgejo.triform.dev/triform/chromeless/actions/workflows/ci.yml/badge.svg?branch=main)](https://forgejo.triform.dev/triform/chromeless/actions)
[![E2E](https://forgejo.triform.dev/triform/chromeless/actions/workflows/e2e.yml/badge.svg?branch=main)](https://forgejo.triform.dev/triform/chromeless/actions)
[![Harness](https://forgejo.triform.dev/triform/chromeless/actions/workflows/harness-loopback.yml/badge.svg?branch=main)](https://forgejo.triform.dev/triform/chromeless/actions)
[![CodeQL](https://forgejo.triform.dev/triform/chromeless/actions/workflows/codeql.yml/badge.svg?branch=main)](https://forgejo.triform.dev/triform/chromeless/actions)
[![Release](https://forgejo.triform.dev/triform/chromeless/actions/workflows/release.yml/badge.svg?branch=main)](https://forgejo.triform.dev/triform/chromeless/actions)

**Open-source cloud browser with low-latency WebRTC streaming.**

> **Status:** Phase 0 — foundations. Container, signaling, and harness are
> still being bootstrapped. See [PROJECT_BRIEF.md](./PROJECT_BRIEF.md) for the
> full roadmap and exit criteria for each phase.

---

## What is this?

`chromeless` is a remote-browser platform: a real Chromium runs on a
Linux server, its tab is streamed to your local browser over WebRTC, and your
mouse / keyboard / clipboard / audio round-trip back through the same peer
connection. The user-facing experience is "I open a webpage and use a browser
inside it" — useful for browser isolation, scraping, agent tooling, embedded
co-browsing, and anywhere you need a real Chromium that you don't want to (or
can't) run locally.

It is built around three opinions:

1. **Latency is the product.** Everything from capture to encoder tuning to
   input dispatch is evaluated against glass-to-glass latency. The targets are
   **<100 ms on LAN** and **<200 ms regional**, measured by a flashing-color
   harness from week one.
2. **Don't fork Chromium until you have to.** The shortest path to a working
   demo is stock Chromium + DevTools Protocol + `getDisplayMedia` piped into
   `RTCPeerConnection`. Custom `FrameSinkVideoCapturer` capture, encoder
   factory injection, and HW encode are scheduled improvements, not v1
   prerequisites.
3. **Ship the boring path first.** Software encode (libvpx VP9, x264 zero-
   latency), a Go WebSocket signaler, and a single-tenant Docker container is
   enough to demo. Multi-tenancy, NVENC/VAAPI, and AV1 come later.

### Why not Selkies, Neko, or Kasm?

Each of those projects solves a slightly different problem and we owe them
significant prior art (see [`docs/prior-art/`](./docs/prior-art/) for our
surveys):

- **Selkies-GStreamer** focuses on GStreamer-based desktop streaming with
  hardware encode; it's not browser-native and not WebRTC-first by default.
- **Neko** is a multi-user "watch together" room around a shared browser; it
  is excellent for that shape and not optimized for one-user-per-Chromium
  isolation or programmatic access.
- **Kasm Workspaces** is a polished commercial product with VNC/KasmVNC at
  its core; it is not OSS-first and not WebRTC-first.

We are aiming for: **WebRTC-first, one-Chromium-per-user, latency-obsessed,
Apache-2.0, with a clear extension path for hardware encoders and custom
capture.** If that overlaps with what you need, contributions welcome.

---

## Architecture sketch

```
              ┌────────────────────── User's machine ──────────────────────┐
              │                                                            │
              │   ┌────────────────┐                                       │
              │   │ Browser client │   client/index.html + main.ts         │
              │   │  (Chrome/FF)   │   - RTCPeerConnection (recv video/    │
              │   └───────┬────────┘     audio; send input via DataChannel)│
              │           │                                                │
              └───────────┼────────────────────────────────────────────────┘
                          │ WebSocket (SDP / ICE)        WebRTC media + DC
                          │                              (UDP, SRTP, DTLS)
                          ▼                                       ▲
              ┌──────────────────────┐                            │
              │   signaling/  (Go)   │                            │
              │   ws://…/session/:id │                            │
              └──────────┬───────────┘                            │
                         │ session bootstrap                      │
                         ▼                                        │
┌──────────────────── Headless Chromium container ─────────────── │ ────────┐
│                                                                 │         │
│   ┌───────────────────────────┐    ┌──────────────────────────┐ │         │
│   │   Chromium (headless)     │    │   capture/ (sidecar)     │ │         │
│   │   - Xvfb / headless+gpu   │◄──►│   - getDisplayMedia path │ │         │
│   │   - DevTools Input.*      │    │   - (later) FrameSink    │ │         │
│   │   - PulseAudio sink       │    │     VideoCapturer        │ │         │
│   └────────────┬──────────────┘    └────────────┬─────────────┘ │         │
│                │ frames + audio + cursor meta   │               │         │
│                ▼                                ▼               │         │
│   ┌────────────────────────────────────────────────────────┐    │         │
│   │   Encoder + libwebrtc                                  │────┼─────────┘
│   │   - SW VP9 / x264, zero-latency tuning (v1)            │
│   │   - HW NVENC/VAAPI behind webrtc::VideoEncoderFactory  │
│   │   - Adaptive bitrate via libwebrtc BWE (Phase 2)       │
│   └────────────────────────────────────────────────────────┘
│
│   infra/  Dockerfile + compose.yaml + (later) K8s manifests
│
└─────────────────────────────────────────────────────────────────────────┘

         harness/  flashing color block + webcam reconciliation
                   → measures glass-to-glass latency end-to-end
```

A more detailed phase-by-phase architecture lives in
[PROJECT_BRIEF.md](./PROJECT_BRIEF.md); design docs and ADRs land in
[`docs/`](./docs/).

---

## Latency budget

Latency is the product. The v1 contract is:

| Path                        | Target            |
| --------------------------- | ----------------- |
| Glass-to-glass, LAN         | **< 100 ms**      |
| Glass-to-glass, regional    | **< 200 ms**      |
| Input round-trip (separate) | budgeted in v1 doc|

The full, measurable acceptance criteria — resolution, framerate, concurrent
sessions per host, supported input types — live in
[`docs/v1-success-criteria.md`](./docs/v1-success-criteria.md). Every
architectural decision is evaluated against that document.

Latency is measured, not asserted. The
[`harness/`](./harness/) directory contains a flashing-color-block page and
reconciliation script: a webcam points at the client screen, the page emits
timestamped color transitions, and we recover glass-to-glass latency from the
captured video. See [`harness/latency/README.md`](./harness/latency/README.md)
for the methodology.

---

## Quickstart

> ⚠️ **Coming Phase 0 exit.** The container, signaling server, and client
> stub all land before `docker compose up` works end-to-end. Track progress
> via the task list in this repo.

### Local dev (docker compose)

```bash
git clone https://github.com/<org>/chromeless.git
cd chromeless
docker compose -f infra/compose.yaml up

# Then open http://localhost:3000
```

Add `--profile observability` to also bring up Prometheus + Grafana
with the cb dashboards (T66) auto-loaded; see
[`infra/observability/dashboards/README.md`](./infra/observability/dashboards/README.md).

### Kubernetes (Helm)

The supported install path for clusters is the chart at
[`infra/helm/chromeless/`](./infra/helm/chromeless):

```bash
helm install chromeless \
    oci://ghcr.io/<org>/chromeless/helm/chromeless \
    --version 0.1.0 \
    --namespace chromeless \
    --create-namespace \
    -f my-values.yaml
```

Required `my-values.yaml` knobs are documented inline in
[`values.yaml`](./infra/helm/chromeless/values.yaml); the
short list is the Ed25519 auth pubkey, the TURN shared secret, and
the TURN URLs. See
[`docs/operations/phase1-deployment-checklist.md`](./docs/operations/phase1-deployment-checklist.md)
before going to production, and
[`docs/operations/runbook.md`](./docs/operations/runbook.md) for
day-2 operations.

For development against individual components before the full stack lands,
see each subdirectory's README.

---

## Known limitations (v1)

A handful of things don't fully work in v1 by design — captured here
so users hit them with eyes open. Each has a planned-fix milestone.

- **WebGL is software-rendered.** v1's launch flag set pins Chromium
  to ANGLE + SwiftShader (T78 / T91 — Vulkan is disabled because
  Xvfb has no Vulkan driver). Simple WebGL pages render correctly;
  heavy WebGL (Three.js stress scenes, full-frame post-processing)
  will drop below ~5 fps and feel choppy. Phase 4's GPU passthrough
  unblocks hardware WebGL. See [`docs/research/rendering-matrix.md`](./docs/research/rendering-matrix.md).
- **WebGPU is not supported.** Dawn requires Vulkan on Linux; v1
  disables Vulkan. `navigator.gpu` is present, but `requestAdapter()`
  returns null. Same Phase 4 GPU-passthrough fix unblocks it.
- **Dev compose ships synthetic media.** `infra/compose.yaml`
  defaults `CHROMELESS_USE_FAKE_MEDIA=1`, which routes `getDisplayMedia`
  through Chromium 147's synthetic test pattern + tone instead of
  real screen capture. Real `getDisplayMedia` fails on Chromium 147
  + Xvfb regardless of launch-flag tuning (see
  [`docs/capture/path-of-least-resistance.md`](./docs/capture/path-of-least-resistance.md)
  §3a). Phase 2's
  `FrameSinkVideoCapturer` (T47, T55) replaces the
  `getDisplayMedia` path entirely and is the durable fix.
- **Single tab per session, single session per container.** v1
  intentionally ships one Chromium per user; multi-tenant
  orchestration is Phase 3.

---

## Repo layout

| Path           | Contents                                                       |
| -------------- | -------------------------------------------------------------- |
| [`signaling/`](./signaling/) | WebSocket signaling server (Go). One session per browser for v1. |
| [`capture/`](./capture/)     | Headless Chromium capture sidecar + spike branches for `FrameSinkVideoCapturer`. |
| [`client/`](./client/)       | Browser-side client (`index.html`, `main.ts`) — RTCPeerConnection + input DataChannel. |
| [`infra/`](./infra/)         | Dockerfile, `compose.yaml`, K8s manifests, TURN/STUN config.   |
| [`harness/`](./harness/)     | Glass-to-glass latency harness (flashing color block + webcam reconciliation). |
| [`docs/`](./docs/)           | v1 success criteria, prior-art surveys, ADRs, internal notes.  |
| [`tests/`](./tests/)         | Container smoke tests, harness validation, E2E regression.     |
| [`PROJECT_BRIEF.md`](./PROJECT_BRIEF.md) | Phased roadmap, team shape, risks, definition of v1 done. |

---

## Contributing

This project is in **Phase 0**; the public contribution flow will solidify
once the Phase 0 exit gate (container + harness + signaling stub) is green.
In the meantime:

- **Issues** describing latency regressions, broken inputs, or container
  build problems are welcome.
- **PRs** are easiest to land if they (a) come with a measurement from the
  harness for anything in the hot path, and (b) reference a section of
  `PROJECT_BRIEF.md` or `docs/v1-success-criteria.md`.
- **Design discussions** that would change architecture should land as an
  ADR under [`docs/`](./docs/) before code.

A formal `CONTRIBUTING.md` and code-of-conduct will be added before the
project is announced publicly.

---

## License

Apache-2.0 — see [`LICENSE`](./LICENSE).

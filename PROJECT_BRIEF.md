# Cloud Browser over WebRTC — Project Brief

> **Status: HISTORICAL — the architecture described here no longer exists.**
>
> This brief plans around *stock Chromium + `getDisplayMedia`*, driven from
> outside the browser. That is not what this project is. The M7 native-peer
> migration deleted `capture/streamer-page/` and moved the WebRTC peer
> **inside the browser process** — see `capture/build-integration/` and
> `capture/signaling/`. Sections below that read as present tense ("where we
> are now", "Phase 0") describe early 2026, not today.
>
> Kept because the *reasoning* is still worth reading — particularly
> principle 1 ("don't fork Chromium until you have to") and the prior-art
> survey, which is exactly the wall this project eventually hit and had to
> descend past. For what the system actually is now, read `README.md`; for
> the wire contracts, `docs/protocols/` and
> `capture/signaling/cb_wire_envelope.h`.

**Scope:** Open-source cloud browser with low-latency WebRTC streaming
**Team:** 4 developers + 1 dedicated tester, WebRTC-strong
**Target:** Linux server-side, containerized; software encode for v1; both single- and multi-tenant eventually

---

## Guiding principles

1. **Don't fork Chromium until you have to.** Every patch is forever. Start with what stock Chromium + DevTools Protocol can give you, and only descend into the source tree when you hit a wall you can prove with measurements.
2. **Lean on prior art.** Selkies-GStreamer, Neko, and Hyperbeam's public writeups have already mapped most of the failure modes. Read them before writing code.
3. **Latency is the product.** Every architectural decision should be evaluated against glass-to-glass latency. Set a budget early (LAN <100ms, regional <200ms) and measure it from week 1.
4. **Ship the boring path first.** Software encode + `getDisplayMedia` + libwebrtc gets you a working demo in weeks. Hardware encode and custom capture are optimizations.

---

## Phase 0 — Foundations (where we are now)

**Goal:** Working dev environment, baseline measurements, team alignment.

- Linux container (Debian/Ubuntu base) with Chromium, Xvfb or headless-with-gpu, PulseAudio, input injection. Document the Dockerfile.
- Glass-to-glass latency measurement harness: a flashing color block driven by JS, captured by a webcam pointed at the client screen, timestamps reconciled. Non-negotiable infrastructure.
- Survey OSS prior art: Selkies-GStreamer, Neko, Kasm. Note their architecture, what they do well, what they cut corners on. Decide which (if any) to fork vs. build from.
- Define v1 success criteria: resolution, framerate, latency target, concurrent sessions per host, supported input types.

**Phase 0 exit:** Container boots, page renders, you can capture a single frame and measure latency end-to-end on a stub pipeline.

## Phase 1 — Streaming MVP (after Phase 0)

User opens a webpage in their browser, sees a remote Chromium tab, and can interact with it. Software encode. No optimizations.

- **Stream-out:** privileged page in headless instance using `getDisplayMedia` → `RTCPeerConnection` to user. Path of least resistance, no C++. Spike alternative: native sidecar with DevTools `HeadlessExperimental.beginFrame` + screencast piped to libwebrtc.
- **Input round-trip:** WebRTC data channel client → server. Server translates to DevTools `Input.dispatchMouseEvent` / `Input.dispatchKeyEvent`. Measure input latency separately from video latency.
- **Signaling:** Minimal WebSocket signaling (Go). One session per browser. No auth/multi-tenancy yet.
- **Audio:** PulseAudio loopback into the same `RTCPeerConnection`. Opus default settings.

**Phase 1 exit:** A user on a different network can browse YouTube on the cloud Chromium with audio, type into search boxes, recognizable as "a browser" even with poor latency.

## Phase 2 — Quality and capture pipeline

- **Custom capture path:** replace `getDisplayMedia` with a direct hook into Viz's `FrameSinkVideoCapturer`. Requires building Chromium from source. Spike as a separate prototype branch first.
  - **Update (post-T29 spike):** the previously-listed alternative — a sidecar using `HeadlessExperimental.beginFrame` + `Page.startScreencast` — is **ruled out for stock Chromium** because the `HeadlessExperimental` CDP domain only exists in the `chrome-headless-shell` binary (per M132+). `Page.startScreencast` works on stock Chrome but exhibits jittery frame pacing (p50 17.9 ms, p99 32.8 ms, σ 9.85 ms) — not viable at our latency budget. Phase 2 should therefore commit directly to the `FrameSinkVideoCapturer` route. See `capture/spike-beginframe/findings.md`.
- **Encoder factory injection:** `webrtc::VideoEncoderFactory` override. Keep software encoders (libvpx VP9, x264) for v1 but tune for low latency: zero-latency tuning, no B-frames, intra-refresh, small GOPs.
- **Cursor handling:** render cursor client-side based on shape/position metadata over data channel.
- **Input fidelity:** IME, modifier keys, paste, drag-and-drop, scroll inertia, touch events, gamepad, clipboard sync.
- **Adaptive bitrate:** hook libwebrtc's bandwidth estimator, scale resolution and framerate on congestion.

**Phase 2 exit:** Internal team uses the cloud browser daily for real work; complaints are about specific apps, not the platform.

## Phase 3 — Multi-tenancy and operations

- Container-per-session orchestration (Docker first, K8s later). One Chromium per container. Never share Chromium across users.
- Resource isolation: CPU shares, memory limits, GPU sharing strategy.
- Session lifecycle: cold start, warm pool, idle timeout, snapshot/restore (stretch).
- Observability: per-session metrics for latency, dropped frames, encoder load, network.
- Auth, signaling at scale, TURN servers, proper STUN, reconnect.

**Phase 3 exit:** A 16-core box runs N concurrent sessions at target quality, where N is published.

## Phase 4 — Hardware encode and polish

NVENC / VAAPI behind the encoder factory. AV1, file upload/download, webcam/mic passthrough, WebGPU/WebGL, HDR.

---

## Team shape

| Name | Role | Primary ownership |
|------|------|-------------------|
| `chromium-dev` | Capture/encoder specialist | Custom capture path, encoder factory, Chromium build |
| `webrtc-dev` | WebRTC pipeline | Signaling, RTCPeerConnection, client SDK, data channels |
| `infra-dev` | Containers/orchestration | Dockerfile, K8s, TURN/STUN, deploy story |
| `platform-dev` | Input + app layer | Input fidelity, cursor, clipboard, audio routing, measurement harness |
| `qa-tester` | Test / measurement | Latency harness validation, E2E tests, regression suite, observability |

---

## Risks

| Risk | Mitigation |
|------|------------|
| Chromium internals shift between versions | Pin to a release branch; track stable on a schedule. Maintain a small, well-documented patch series. |
| Software encode misses latency target on commodity cloud CPUs | Prove the budget on target instance type before committing software-only. Have hardware-encode fallback ready. |
| `FrameSinkVideoCapturer` API churns | Spike custom capture as a throwaway prototype. Stay on `getDisplayMedia` longer if painful. |
| Input latency dominates regardless of video latency | Measure input separately from frame 1. Optimize hot path independently from correctness paths. |
| Untrusted pages on shared infra | gVisor or Firecracker per container. Don't share Chromium across tenants. |
| OSS traction never materializes | Pick one strong differentiator vs Selkies/Neko. Working demo URL in README. |

---

## v1 "done"

A user clones the repo, runs `docker compose up`, opens `http://localhost:8080`, and gets a working cloud Chromium with acceptable latency for typical browsing. Codebase has a clear extension point for hardware encoders. Docs another engineer can follow to add a new input type or encoder backend.

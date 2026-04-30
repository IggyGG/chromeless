# Phase 1 stack end-to-end audit (T100)

**Date:** 2026-04-30 · **Author:** `qa-tester` · **Repo HEAD:** `f6528a2` ·
**Scope:** every Phase 1 component reviewed against
[`docs/v1-success-criteria.md`](../v1-success-criteria.md).

This doc is the v1 ship/no-ship baseline. Per-component findings, then a
mapping to the success-criteria checklist, a TODO-marker inventory, a
recommended sign-off path, and a list of risks that don't have a
follow-up task yet.

---

## Executive summary

Phase 1 is **ship-ready in code**, with the v1-defining number still
gated on the operator measurement run (T65, blocked on T86's real fix
or its dev fake-media workaround being adopted in production). The
shipping container boots, signaling round-trips end-to-end, the audio
path is wired, and #96's offer-replay fix landed (commit `f640478`)
unblocking the full negotiation pipeline. The remaining risk surface
is concentrated in two places: (1) every encoder body lives behind a
`TODO(T17-build-env)` compile gate, so v1 effectively ships with a
designed-but-not-linked encoder stack relying on Chromium's bundled
libwebrtc encoders for actual byte production; and (2) the
glass-to-glass and concurrent-sessions checklist items (L1–L4, C1–C4)
have no real numbers yet — the harness scaffolding is ready
([T65](../measurements/phase1-loopback-2026-04-30.md)) but a hardware-
in-loop run hasn't happened. **Recommendation: PROCEED to v1 sign-off
contingent on the §4 must-pass list.**

---

## 1. Per-component findings

### 1.1 Capture (T23 streamer page, T28 wiring, T34 answerer flip, T29 spike)

`capture/streamer-page/streamer.js` (00f80df → 411d06a) is production-
authored: full signaling protocol, `getDisplayMedia` (with the T86
fake-media gate), RTCPeerConnection with answerer-role flow per T34,
heartbeat, and the input-bridge relay (T41). Launched via
`infra/launch-chromium.sh` with the X11 + ANGLE/SwiftShader pin. The
T29 HeadlessExperimental spike concluded such a sidecar is not viable
in stock Chromium — Phase 2 must use the in-process FrameSink path.
**Risk:** real `getDisplayMedia` under Chromium 147 + Xvfb returns
`NotReadableError` (T86). Production today ships with the
`CBWRTC_USE_FAKE_MEDIA=1` env flag (synthetic green frames + 440 Hz
tone). Acceptable for a contract-layer demo; not acceptable for the
v1 latency number (which would measure synthetic-encoder time, not
real screen capture).

### 1.2 Encoder factory + encoders (T19, T35, T36, T54, T58, T63, T70, T75, T83, T85, T97)

Factory contract + BWE adapter (T19, T58) wired in
`encoder_factory_stub.cc`. SW encoders (VP9 T35, x264 T36, SVT-AV1
T75) are class-complete with 6+7+(SVT) unit tests. HW skeletons
(NVENC T63, VAAPI T70) are shape-only behind SDK ifdefs. Simulcast
(T83) wraps any encoder per-layer, 11 mocked-encoder tests pass.
Damage-rect (T85) is a Phase-2 design doc only.
[T97 catalog](../capture/encoder-factory-catalog.md) is the canonical
status table. **Risk:** every body carries `TODO(T17-build-env)` —
nothing is linked. v1 production today is encoded by Chromium's own
in-tree libwebrtc encoders; our factory injection is dead code until
T17's build environment runs. T101 has a 2-week unlock plan from
that point.

### 1.3 FrameSinkVideoCapturer scaffold (T55, T47 design)

`capture/framesink-capturer/` (a8c500c) — Viz mojom consumer with
`BufferHandleScope` RAII for the Done() footgun T47 called out, 11
gtests, wired through the encoder factory injection patch in
`capture/build-integration/`. **Phase 2 durable fix for the T86
getDisplayMedia issue.** Compile-gated on T17 + Mojo headers; no v1
impact.

### 1.4 Build integration (T49, T17, T29) and launch flags

`capture/build-integration/` (736aab6) ships `args.gn` + a single
patch (`0001-expose-encoder-factory-injection.patch`). T17 research
landed; the actual build host hasn't been provisioned. Launch script
(`infra/launch-chromium.sh`) is production: ozone-x11, use-gl=angle,
use-angle=swiftshader-webgl, disable-gpu-vsync, with the optional
`--use-fake-device-for-media-stream` gate (T86). Plus the T9-followup
socat sidecar (a3cd5ad) bridging eth0:9222 → loopback to work around
Chromium 147's bind-only-loopback DevTools default.

### 1.5 Signaling server (T13, T27, T51, T67, T89, T93, T96)

`signaling/server.go` is production-tested: T27 integration green
(SDP/ICE/duplicate-role/bye/isolation), T51 stabilized the ICE-trickle
race with generous deadlines. **T96 (f640478) shipped the offer-replay
buffer** — the protocol gap I filed during T64/T65 is now closed; the
streamer's offer is buffered and replayed when the client joins.
`offer_replay_test.go` covers four scenarios (only-most-recent, no-effect-
on-normal, ICE-not-replayed, survives-peer-reconnect) — all green
locally. T67 tenant namespacing (composite `(tenant, session)`), T89
revocation (denylist + admin endpoint), T93 region-scoping
(`aud` claim). **Risk:** none for v1 — this is the most-tested layer in
the stack.

### 1.6 ICE / TURN (T25, T76)

T25's `/turn-credentials` endpoint returns an `RTCConfiguration` (STUN-
only by default; falls back gracefully if endpoint fails). T76 TURN-REST
issuer (RFC 7635, HMAC-SHA1) signs short-lived credentials gated on a
T48 token, supports rotation via `_PREV` overlap. Per-region issuers
hold per-region secrets. v1 ships **STUN-only**; self-hosted coturn is
explicitly Phase 3.

### 1.7 Auth (T48, T89)

Ed25519 JWTs, single-key verifier in signaling, dev issuer for local
flow. **Disabled by default** when `CBWRTC_AUTH_PUBKEY` is unset —
signaling logs a clear warning at boot, all sessions collapse to the
`_anonymous` tenant. T89 layers short-TTL refresh + Redis denylist.
Key rotation is single-key-only for v1; Phase 3 adds JWKS for rolling.

### 1.8 Multi-region (T87 design, T93 region claim, T94 federated obs)

T93 enforces `aud` against `CBWRTC_REGION`, rejects `region_not_allowed`,
tags every metric with `region`. T94 federates Prometheus per-region
into a global view; Helm `region:` propagates everywhere. **T87's
GeoDNS / cross-region session pools / TURN topology are design-only**
— Phase 3.

### 1.9 Browser client (T14, T34, T37, T42, T48-client, T54)

Five data-channel branches wired (`input`, `cursor`, `clipboard`, `stats`,
`files` for Phase 4). T37 reconnect with exponential backoff
(unit-tested), T42 stats sample emission with T82 tenant/session
labels, T54 codec-fallback inspection post-SDP, T48-client fetches
TURN config + signed token with public-STUN fallback. Vitest suite
green. **Risk: minimal.** Latency-budget contributions live in the
capture/encode layers, not here.

### 1.10 Input pipeline (T20, T22, T41, T46, T56, T62, T88)

14 event types in the v1 protocol: mouse, keyboard, composition,
clipboard, drag-drop, touch. T41 wires the data-channel relay to T22's
input-bridge (a Go sidecar that translates v1 envelopes into
DevTools `Input.dispatch*`). T46 drag, T56 touch, T62 scroll inertia
(phase / delta-mode / momentum), T88 IME (composition + caret +
candidate list). T20's protocol covers I1–I3 directly; I4 (clipboard)
is in T32 below. **Risk:** the bridge has **no rate-limiting** today
(noted in `input-bridge/README.md`); a misbehaving client could DoS
DevTools. Phase 2 hardening.

### 1.11 Cursor metadata (T26)

`capture/cursor-watcher/` injects a JS probe on every page load,
emits cursor envelopes (x, y, visible, shape) over the data channel
on change. Client renders an overlay above the `<video>`. **Gap:**
on data-channel reconnect the watcher should re-emit a baseline
state; not yet wired. Cosmetic for v1.

### 1.12 Clipboard sync (T32)

`capture/clipboard-bridge/`, text-only, 1 MiB cap. Outbound via JS
`copy` event + `Runtime.addBinding`; inbound via
`navigator.clipboard.writeText` + synthetic Ctrl+V (or
`Input.insertText` fallback). Echo suppression by last-write
tracking. Image/HTML clipboard is explicitly out of scope for v1.

### 1.13 Audio (T24)

PulseAudio two-sink cascade in `infra/pulse-default.pa`:
`cb_audio` → `module-loopback` (20 ms) → `cb_capture` → consumed by
`getUserMedia({audio:true})` on the streamer page. Opus 48 kHz
stereo, libwebrtc defaults. Chromium flags grant permissions, pin the
PulseAudio socket. **Audio E2E spec 05** is wired; gated on T78 same
as the video path.

### 1.14 File upload bridge (T74) — Phase 4

`capture/file-bridge/` chunks + SHA-256 + MIME sniff + CDP
`DOM.setFileInputFiles`. Virus-scan stub. **Out of v1 scope**; the
fact that it shipped early is a bonus.

### 1.15 Webcam / mic passthrough (T81 + T92)

Client adds tracks → renegotiation (T37) → streamer page consumes
inbound tracks → Unix-socket relay → `v4l2-writer` writes I420 to
`/dev/video10` (v4l2loopback) and audio to PulseAudio via `pacat`.
**Out of v1 scope**; T92 v4l2-writer integration is in flight as a
T81 follow-up.

### 1.16 Container + lifecycle (T7, T28, T31, T57, T68, T90)

Debian bookworm-slim base; supervisord with xvfb / pulseaudio /
streamer-static / chromium / devtools-proxy / cb-metrics-sidecar /
idle-watchdog. T57 hardening: non-root, read-only fs, dropped caps,
seccomp profile. T31 cold-start wipes user-data-dir, resolves
SESSION_ID, persists `/run/cb-session/env`. T9 smoke green
(commit `2907370` — verified end-to-end with a real PNG of
example.com). T90 ScrubAndReturn for warm-pool recycling. T68 CRIU
scaffold only. **Risk:** `--no-sandbox` ships as the v1 expedient and
is the single largest deferred risk against multi-tenant production —
explicitly Phase 3 territory (T44 sandbox primer).

### 1.17 Orchestration (T50 design, T71 BrowserSession controller)

Go controller (`infra/controllers/browser-session-controller/`)
reconciles `BrowserSession` + `BrowserSessionPool` CRDs to Pods.
Warm-pool replenishment, session assignment via label, status
exposure, idle eviction (timestamps measure "since assigned" not
"since user activity" — a Phase 3 task lands signaling→controller
heartbeats). T90 ScrubAndReturn is implemented but the happy path
still goes through full pod recreation; scrub is opt-in. Admission
webhook scaffold + RBAC complete.

### 1.18 Observability (T38, T66, T82, T94)

cb-metrics-sidecar polls Chromium DevTools every 10 s + `/proc`,
exposes Prometheus on `:9100`. Two Grafana dashboards: cluster-overview
+ session-detail. T82 caps metric cardinality at 100 tenants × 100
sessions before falling to `_other`. Signaling exposes its own
`/metrics`. **Gaps:** no alert rules in-image (Phase 3 — belong in the
deploy repo); 10 s polling means sub-second bursts are invisible
unless the harness is running; client-side stats forwarding (T72) is
green. T99 OpenTelemetry tracing is in flight separately.

### 1.19 Operations (T80 runbook, T84 CI, T21 baseline workflow)

`docs/operations/runbook.md` covers build/push, seccomp install,
overlays, rollback, scaling, draining, snapshot ops. CI: T21 baseline
(lint + build-image + smoke + docs link-check, < 10 min wall) plus
T84's added e2e + harness-loopback workflows. PR gate is unit +
integration + smoke; harness is nightly + label-opt-in. **Gap:** the
phase1-deployment-checklist hasn't been walked end-to-end on a real
cluster yet; first real run is part of v1 sign-off itself.

### 1.20 Harness (T10, T11, T12, T39, T59-T61)

Source page + reconciler + validation methodology + input-latency
variant. All three T12 follow-ups landed (G1 bars-fallback demoted to
human-diagnostic, G2 cam_fps Nyquist warning, G3 sink_lag in summary).
`tests/harness/loopback-baseline.sh` hermetic regression check passes
8/8 against the bundled fixture. **Risk:** `phase1-baseline.sh` has
never run end-to-end on real hardware; `tests/harness/baselines/` is
empty. T65 framework is ready to fill on first operator session.

---

## 2. Pass/fail vs v1 success criteria (T6)

| ID    | Criterion                                                  | Status | Notes |
|-------|------------------------------------------------------------|--------|-------|
| R1    | 1080p30 in capture + SDP                                   | **conditional** | Capture is fake-media (T86); negotiated SDP carries 1080p30 video track. Real-screen R1 awaits T17 build env or a non-Xvfb Chromium fix. |
| R2    | ≥ 25 fps under load                                        | **gated** | No real-pipeline measurement run yet. Operator session pending. |
| R3    | No tearing / decoder error overlay                         | **gated** | Same. |
| L1–L4 | Glass-to-glass LAN/regional p50/p95                        | **gated** | T65 scaffold ready; numbers blocked on operator run. |
| L5    | Input round-trip < 80 ms LAN p50                           | **gated** | Input-latency harness (T39) ready; same gate. |
| L6    | Per-stage encoder latency vs budget within ±25%            | **gated** | Requires linked encoders (T17 unlock) for real per-stage data. |
| C1–C4 | 4 (stretch 8) concurrent sessions on 16 vCPU               | **gated** | Controller (T71) supports it; never measured. |
| I1    | Mouse — move/click/drag/scroll                             | **conditional** | Protocol + bridge complete (T20+T22+T41+T62). E2E spec scaffolded (`tests/e2e/`); needs operator validation. |
| I2    | Keyboard — printables, modifiers, function keys            | **conditional** | Same as I1. |
| I3    | IME — Pinyin + Hiragana                                    | **conditional** | T88 polish in protocol; e2e spec authored. |
| I4    | Clipboard text                                             | **conditional** | T32 clipboard-bridge complete; e2e spec scaffolded. |
| I5    | Input round-trip latency                                   | **gated** | Same as L5. |
| A1–A4 | Audio plays / sync drift / opus 48 k / no crackle          | **conditional** | Routing + spec 05 wired; A1–A3 met at code level; A4 needs the 10-min live-playback session. |
| B1–B5 | Browser support matrix                                     | **gated** | Playwright matrix authored for chromium; firefox + safari + edge + mobile not exercised. |
| §7    | Out of scope                                               | n/a    | All §7 items respected; T74/T81 are early-shipped Phase 4. |

"**conditional**" = code path is in place + unit/integration-tested,
needs operator validation against the real pipeline.
"**gated**" = depends on T86 real-fix and/or the first operator
measurement session.

---

## 3. TODO-marker inventory

Categorized by ship-blocking vs Phase 2+ stretch.

### `TODO(T17-build-env)` — every encoder + framesink + build script
Files: `capture/encoder/{vp9,h264,svtav1,nvenc,vaapi,bwe_adapter,
simulcast_factory}_encoder{,_test}.cc`,
`capture/framesink-capturer/capturer{,_test}.cc`,
`capture/build-integration/build.sh`, plus README references.
**Gate:** all flip from "code authored" to "code compiled/linked/
tested" the day someone provisions the T17 build host. **Phase 2
unlock**, not Phase 1 ship-blocking — v1 production runs Chromium's
in-tree libwebrtc encoders unaltered.

### `TODO(NVIDIA-Video-Codec-SDK)` — `nvenc_encoder{,_test}.cc`
Needs `<nvEncodeAPI.h>` + `libnvidia-encode` + Chromium GPU bindings.
**Phase 4 stretch**; HW encode is explicitly deferred per
`v1-success-criteria.md` §7.

### `TODO(VAAPI-libva)` — `vaapi_encoder{,_test}.cc`
Needs `<va/va.h>` + `<va/va_drm.h>` + libva. **Phase 4 stretch**, same.

### `TODO(SVT-AV1-lib)` — `svtav1_encoder{,_test}.cc`
Needs `<EbSvtAv1Enc.h>` + libSvtAv1Enc. **Phase 4 stretch**.

No other TODO/FIXME/XXX markers in the v1 ship path.

---

## 4. Recommended Phase 1 sign-off path

### 4.1 Must-pass (gates v1 ship)

1. **T86 real-fix lands** OR `CBWRTC_USE_FAKE_MEDIA=1` is documented
   as the production v1 default with explicit trade-off (no real screen
   capture; synthetic media). Today's compose.yaml already takes the
   second path.
2. **First operator measurement session** runs the full
   `tests/harness/validation.md` §4 procedure, fills in
   `docs/measurements/phase1-loopback-2026-04-30.md`, and produces a
   committed `tests/harness/baselines/phase1-latest.json`. L1–L4, L5,
   L6 all pass.
3. **Concurrent-sessions pilot**: 4-session C1 run on the 16 vCPU
   reference deployment, dashboards green, no OOM-kills (C1–C3).
4. **Browser matrix**: Chrome current/previous on Linux + macOS +
   Windows (B1) — all `tests/e2e/` specs pass. Firefox + Safari
   defer-or-document if not green; document in the v1 acceptance
   report.
5. **`docker compose up` quickstart**: run from scratch on a clean
   reference host per the README, end-to-end Connect → video flow.
6. **Two-tester reproducibility** of the harness numbers within
   ±10% (per the v1 sign-off rule).

### 4.2 Must-document (acceptable to defer)

- `--no-sandbox` in production: the multi-tenant trust boundary is
  not gVisor/Firecracker yet (T44 is design-only). Document the
  single-tenant-per-deployment assumption on the front page.
- TURN: STUN-only. Document that users behind symmetric NAT will
  fail; T76 issuer is built but TURN endpoints aren't deployed.
- Auth disabled by default: ship with a clear "set
  `CBWRTC_AUTH_PUBKEY` for any internet-exposed deployment" warning.
- Idle-eviction measures "time since assignment" not "time since
  activity" — Phase 3 follow-up.

### 4.3 Out of scope (per `v1-success-criteria.md` §7)

Hardware encode, AV1, custom capture, file upload, webcam/mic
passthrough, touch, gamepad, multi-user rooms, AAA, K8s multi-region,
self-hosted TURN at scale, WebGPU acceleration, snapshot/restore,
mid-call network-change reconnect — all explicitly deferred. Several
of these have shipped as design or skeleton (T74, T81, T92, T56,
T68, T87) — that's bonus scope, not creep.

---

## 5. Risks not yet captured anywhere

- **Idle-watchdog ↔ streamer-page contract is implicit.** Watchdog
  reads `window.pc.connectionState`; if T23's variable name changes,
  the watchdog silently reports null → premature container shutdown.
  Worth a regression assertion (Playwright spec 04 already covers
  the hook existence). No follow-up filed.
- **cb-metrics-sidecar same-contract dependency.** Sidecar polls
  `window.pc` via DevTools; same single-name fragility as the
  watchdog. No follow-up filed.
- **Cursor-watcher reconnect baseline.** On data-channel reconnect
  the watcher should re-emit cursor state; today's client sees a
  stale overlay until the next user mouse-move. Cosmetic.
- **Input-bridge has no rate-limit.** A misbehaving client can flood
  DevTools dispatches. v1 single-tenant acceptable; Phase 2 must
  cap.
- **Phase 1 deployment checklist hasn't been run.** First real-cluster
  walk is part of v1 sign-off itself; document any frictions found.
- **`tests/harness/baselines/` is empty.** Until the first
  `phase1-baseline.sh` run lands, the regression-budget gate from
  `tests/README.md` (≤ 5 ms drift) can't fire.

If any of the first four become observed-in-the-wild (vs theoretical),
file follow-ups.

---

## Sign-off

This audit recommends **PROCEED** to v1 sign-off subject to §4.1's six
must-pass items. The single largest residual risk is a Chromium 147
+ Xvfb capture path that ships behind a documented synthetic-media
flag; the durable fix is Phase 2's FrameSinkVideoCapturer (T55
scaffold ready; T17 build env unlocks the rest). Everything else is
either green, conditional-on-the-operator-session, or explicitly
deferred per `v1-success-criteria.md` §7.

Cross-references: T101 Phase 2 unlock plan, T80 deployment runbook,
T6 v1 success criteria, T65 first-measurement framework.

# v1 Success Criteria

> **Status:** Phase 0 contract. This document is the team's definition of
> "v1 done" — it is the criterion every architectural decision is evaluated
> against. Changes require an ADR.
>
> **Owners:** `platform-dev` (author), `qa-tester` (validation methodology).

This document defines the measurable, testable acceptance criteria for v1 of
`cloud-browser-webrtc`. v1 is reached when **every** Pass criterion below is
satisfied on a reference deployment using the methodology in the
[latency harness](../harness/) and the [container smoke + E2E tests](../tests/).

The reference deployment for v1 sign-off:

- **Server:** Linux x86_64 host, 16 vCPU / 32 GB RAM, no GPU encoder, running
  `infra/compose.yaml`.
- **Client:** Chrome stable on desktop Linux/macOS/Windows.
- **Network for LAN measurement:** wired or single-hop Wi-Fi, RTT < 5 ms,
  loss < 0.1%.
- **Network for regional measurement:** same continent, RTT 30–60 ms,
  loss < 0.5%.
- **Tester:** `qa-tester`, using the harness in [`harness/`](../harness/) and
  scripts in [`tests/`](../tests/).

If a criterion cannot be measured with the harness as it exists today, the
criterion ships with a stub procedure and a follow-up task to make it
measurable. Untestable criteria are not allowed to block v1.

---

## 1. Resolution and framerate

**Target:** 1080p (1920×1080) at 30 fps sustained, on the reference deployment,
for the LAN scenario.

**Rationale:** 1080p30 is the sweet spot for software VP9/x264 zero-latency
encoding on commodity 16-core CPUs. It matches the dominant client display
mode (laptop + external 1080p monitor), keeps the per-session bitrate budget
under 8 Mbit/s with reasonable visual quality, and leaves headroom for
multi-tenancy in Phase 3. 4K / 60 fps is deferred to Phase 4 (HW encode +
AV1).

**Pass criteria (checklist):**

- [ ] **R1.** The `getDisplayMedia` capture path is configured for
      1920×1080 @ 30 fps and the negotiated SDP includes a corresponding
      video track.
      *Procedure:* read `getStats()` `frameWidth` / `frameHeight` /
      `framesPerSecond` on the receiver `RTCRtpReceiver` for ≥ 60 s of a
      cnn.com / youtube.com idle page; medians within ±5% of target.
- [ ] **R2.** Sustained framerate under interactive load (scrolling a long
      page, playing a 1080p YouTube video, typing into a search box) does
      not drop below 25 fps for more than 1 s of any 60 s window.
      *Procedure:* `getStats()` time series over a scripted interaction in
      `tests/e2e/`; tester eyeball + automated threshold.
- [ ] **R3.** No client-side rendering artifacts (block tearing, persistent
      green frames, decoder error overlays) during the R2 procedure.
      *Procedure:* visual inspection by `qa-tester` against the reference
      script; failures captured to `tests/artifacts/`.

**Defer to Phase 4:** 4K (3840×2160), 60 fps, HDR, multi-monitor.

---

## 2. Latency budget

**Target:** glass-to-glass video latency, measured by the flashing-color-
block harness:

| Scenario     | Target     | Hard cap |
| ------------ | ---------- | -------- |
| LAN          | **< 100 ms** median | 150 ms p95 |
| Regional     | **< 200 ms** median | 300 ms p95 |

Glass-to-glass is defined as: the time between a pixel changing color in the
headless Chromium tab and the same color reaching the client display, as
recovered from a webcam pointed at the client screen by the harness.

### 2.1 Budget breakdown (LAN)

The 100 ms LAN budget is allocated across the pipeline as a planning aid.
These are not hard sub-budgets, but exceeding any one materially is a flag
for an ADR.

| Stage                              | Budget   | Notes |
| ---------------------------------- | -------- | ----- |
| Browser render → capture surface   | 8 ms     | One frame at 30 fps is 33 ms; we count the half-frame jitter. |
| Capture (`getDisplayMedia` v1)     | 12 ms    | Will drop with `FrameSinkVideoCapturer` in Phase 2. |
| Software encode (VP9/x264 zero-lat)| 18 ms    | Per-frame on 16-core, 1080p30; measure with libwebrtc encoder stats. |
| RTP packetize + pacer + send       | 4 ms    | libwebrtc default. |
| Network (one-way LAN)              | 5 ms    | RTT/2 budget; tighter on wired. |
| Receive + jitter buffer            | 25 ms    | Single-frame target; libwebrtc default playout delay. |
| Decode (VP9/H.264 in browser)      | 12 ms    | Hardware decode in client. |
| Compositor → display               | 16 ms    | One vsync at 60 Hz client display. |
| **Total (LAN)**                    | **100 ms** | |

The regional budget adds ~50 ms of one-way network and a slightly larger
jitter buffer (~25 ms more).

**Pass criteria (checklist):**

- [ ] **L1.** LAN median glass-to-glass latency < 100 ms across **at least
      300 sampled transitions**, measured by `harness/latency/reconcile.py`,
      reference deployment.
- [ ] **L2.** LAN p95 glass-to-glass latency < 150 ms, same sample.
- [ ] **L3.** Regional median glass-to-glass latency < 200 ms, same
      sample-size, same-continent peer.
- [ ] **L4.** Regional p95 < 300 ms, same sample.
- [ ] **L5.** Input round-trip latency (DataChannel keydown → first frame
      with the rendered character) < 80 ms median LAN. Measured by a
      separate harness mode where the harness drives keystrokes and looks
      for the rendered glyph; budgeted independently from video latency.
- [ ] **L6.** Per-stage encoder latency reported via `getStats()` matches
      the budget within ±25%; deviations require an ADR.

**Defer to later phases:** target tightening (Phase 4 with HW encode aims
< 60 ms LAN), congestion-aware adaptive bitrate (Phase 2), reconnect
behavior under packet loss > 2% (Phase 3).

---

## 3. Concurrent sessions per host

**Target (v1, software encode):** **4 concurrent sessions** sustained at the
R1/R2/L1 quality bar on a 16 vCPU / 32 GB host, with a stretch goal of 8.

**Rationale:** software VP9 zero-latency encoding at 1080p30 consumes
roughly 1.5–2.5 cores per session on Skylake-class CPUs (validated by
Selkies and Neko writeups; we will re-measure). At 2 cores per session
plus Chromium overhead (~1 core idle, more under load), 4 sessions is the
floor we sign off on; 8 is the stretch the encoder/CPU has to be tuned to
hit. Phase 3 re-validates this number at K8s scale and may revise.

**Pass criteria (checklist):**

- [ ] **C1.** With 4 sessions running the R2 interactive-load script in
      parallel on the reference host, every session passes R1, R2, L1, L2
      independently.
- [ ] **C2.** The host's CPU `%idle` does not drop below 5% for more than
      a 10-s window during the C1 run (i.e. we have at least some headroom).
- [ ] **C3.** No session is OOM-killed; per-container RSS stable under
      4 GB.
- [ ] **C4.** Stretch (non-blocking for v1): C1 holds for **8** sessions.

**Defer to Phase 3:** auth, warm pool, snapshot/restore, gVisor/Firecracker
isolation, NUMA pinning.

---

## 4. Supported input types

**Target:** the input set required for "this is recognizably a browser" —
mouse, keyboard with modifiers, scroll, basic IME, and clipboard sync
(text only). Touch / gamepad / webcam-passthrough are deferred.

Inputs are dispatched server-side via DevTools `Input.dispatchMouseEvent`
and `Input.dispatchKeyEvent`. Clipboard syncs via a dedicated DataChannel
message type.

**Pass criteria (checklist):**

- [ ] **I1.** Mouse move, left/middle/right click, double-click, click-and-
      drag, scroll wheel (vertical + horizontal). *Procedure:* scripted
      `tests/e2e/input_mouse.spec.ts` against a known target page; visual
      check + DOM-event assertion.
- [ ] **I2.** Keyboard: all printable ASCII, Shift / Ctrl / Alt / Cmd
      modifiers, Enter, Tab, arrow keys, Backspace, Delete, Esc, function
      keys F1–F12. *Procedure:* `tests/e2e/input_keyboard.spec.ts` types
      a known string into a textarea and asserts equality.
- [ ] **I3.** IME: composition events round-trip for a known Pinyin and
      Hiragana sequence via the DataChannel. *Procedure:*
      `tests/e2e/input_ime.spec.ts`.
- [ ] **I4.** Clipboard sync, text only: `Cmd/Ctrl+C` on the remote page
      populates the local OS clipboard within 250 ms; `Cmd/Ctrl+V` from
      local OS pastes into a focused remote text field. *Procedure:*
      manual + `tests/e2e/clipboard.spec.ts` with a clipboard-permission
      stub.
- [ ] **I5.** Input round-trip latency meets L5.

**Defer to Phase 2:** scroll inertia, drag-and-drop of files, paste of
non-text MIME types.

**Defer to Phase 4:** touch events, multi-touch gestures, gamepad, webcam
and microphone passthrough.

---

## 5. Audio

**Target:** Opus 48 kHz stereo, libwebrtc defaults, sourced from a
PulseAudio sink inside the container and routed into the same
`RTCPeerConnection` as the video.

**Rationale:** matches Chromium's native audio defaults; libwebrtc's Opus
implementation is well-tuned out of the box; deviations are deferred until
we have evidence they matter.

**Pass criteria (checklist):**

- [ ] **A1.** Audio plays in the client browser when the remote tab is on
      a YouTube video. *Procedure:* manual + `tests/e2e/audio_smoke.spec.ts`
      asserts `RTCInboundRtpAudioStream.audioLevel` > 0 for ≥ 10 s.
- [ ] **A2.** Audio/video sync drift ≤ 100 ms across a 5-minute YouTube
      playback. *Procedure:* manual A/V sync check using a "lip-sync test"
      reference video; automated check against `RTCRemoteInboundRtpStream`
      timestamps.
- [ ] **A3.** Negotiated SDP shows `opus/48000/2` and `useinbandfec=1`.
- [ ] **A4.** No persistent crackle, dropouts > 50 ms, or clock skew under
      10 minutes of continuous playback at the reference deployment.

**Defer:** mic passthrough (Phase 4), spatial audio (out of scope), echo
cancellation (out of scope — there's no local mic in the loop for v1).

---

## 6. Client browser support

**Target:** Chrome / Edge / Firefox / Safari, current stable + previous
stable on desktop. Mobile browsers are best-effort, not a v1 gate.

**Pass criteria (checklist):**

- [ ] **B1.** Chrome current + previous on Linux, macOS, Windows: passes
      R1, R2, L1, L2, I1, I2, A1.
- [ ] **B2.** Edge current + previous (Chromium-based, desktop): same as B1.
- [ ] **B3.** Firefox current + previous on Linux, macOS, Windows: passes
      R1, R2, L1, L2, I1, I2, A1. Known Firefox H.264/VP9 quirks
      documented; SDP renegotiation works.
- [ ] **B4.** Safari current + previous on macOS: passes R1, R2, L1, L2,
      I1, I2, A1. Safari may require H.264 instead of VP9; the SDP
      negotiation handles this without a code branch on the client.
- [ ] **B5.** Best-effort mobile: page loads and a frame is visible on iOS
      Safari and Android Chrome current; no perf or input gate.

**Defer:** older Safari, in-app webviews, embedded WebView2 hosts.

---

## 7. Out of scope for v1

The following are explicitly **not** v1 acceptance criteria. Scope creep
into this section requires an ADR and team-lead sign-off.

- **Hardware video encode** (NVENC, VAAPI, QuickSync) — Phase 4.
- **AV1** encode or decode path — Phase 4.
- **Custom capture path** via `FrameSinkVideoCapturer` — Phase 2 spike,
  not required for v1 sign-off.
- **File upload / download passthrough** — Phase 4.
- **Webcam / microphone passthrough** from client to remote tab — Phase 4.
- **Touch and gamepad inputs** — Phase 4.
- **Multi-user shared sessions / "watch together" rooms** — out of scope
  (different product, Neko fits that shape).
- **Authentication, accounts, billing** — Phase 3+ infrastructure concern.
- **Multi-tenant orchestration / K8s** — Phase 3.
- **TURN/STUN at scale, geo-distributed signaling** — Phase 3.
- **WebGPU acceleration of the remote tab** — out of scope for v1; see
  Phase 4 for HDR / WebGL stretch.
- **Snapshot / restore of a session** — Phase 3 stretch.
- **Reconnect across network changes** without re-establishing the peer
  connection — Phase 3.

---

## Sign-off

v1 is "done" when:

1. Every Pass-criterion checkbox above is checked, with a linked test run
   or harness output.
2. The `docker compose up` quickstart in the root README works on a clean
   reference host.
3. The harness deltas are reproducible by a second tester within ±10%.
4. The `qa-tester` files a v1-acceptance report under
   `docs/internal/v1-acceptance-<date>.md` linking each checkbox to evidence.

This document supersedes informal targets in `PROJECT_BRIEF.md` for the
purpose of v1 sign-off; the brief remains the long-term roadmap.

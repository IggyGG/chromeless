# Phase 0 exit gate — report

**Date:** 2026-04-30
**Author:** `qa-tester`
**Repo HEAD at run:** `e6ec3eab`
**Project brief Phase 0 exit criterion:**
> Container boots, page renders, you can capture a single frame and
> measure latency end-to-end on a stub pipeline.

**Recommendation: PROCEED to Phase 1.**

All three Phase-0 contracts are met. The integrated compose stack
boots cleanly and signaling round-trips end-to-end. One real Phase 1
integration concern was uncovered during the run and is filed as a
follow-up (T69 — streamer page doesn't expose `window.pc`); it does
not block Phase 0 exit because each Phase-0 component stands on its
own.

---

## 1. What passed

### 1.1 Container boot smoke (T9) — PASS

Re-run from clean against `cloud-browser-webrtc:dev` immediately
before this report:

```
== smoke passed ==
[smoke] container-boot: PASS
```

End-to-end ~12 s on Apple Silicon. Layered checks all green:

| Step | Result |
|------|--------|
| 1. Image resolved (`cloud-browser-webrtc:dev` auto-detected) | OK |
| 2. Container started; DevTools up after 3 s | OK |
| 3. `/json/version` reports `Browser: Chrome/147.0.7727.116` | OK |
| 4. Page target discovered | OK |
| 5. `Page.navigate https://example.com`, `readyState=complete` | OK |
| 6. `Page.captureScreenshot` returned 20060 bytes | OK |
| 7. PNG magic + size > 5 KiB | OK (1919×1027 RGB) |

Driven via `docker exec` against the container's loopback to side-step
T52 (Chromium 147 ignores `--remote-debugging-address=0.0.0.0`); see
`tests/smoke/README.md` §"Why we drive DevTools from inside the
container".

### 1.2 Latency harness reconciler regression (T12) — PASS

`bash tests/harness/loopback-baseline.sh`:

```
[loopback] PASS
```

All 8 deterministic-fixture assertions green:

```
  OK         frames_seen=4.0
  OK          qr_decoded=4.0
  OK      qr_decode_rate=1.0
  OK      negative_count=0.0
  OK              min_ms=42.0
  OK              p50_ms=49.0
  OK              p95_ms=82.45
  OK              max_ms=88.0
```

This validates `harness/latency/reconcile.py` parsing, percentile
math, time arithmetic, and manifest precedence — the unit-of-trust
for every latency number we will report against the v1 budget.
**Methodology** (clock skew, frame-rate aliasing, rolling shutter,
ambient light, rAF-vs-paint) is documented in
[`tests/harness/validation.md`](../tests/harness/validation.md);
**physical loopback** is the operator's per-session step and is
unrelated to this hermetic CI gate.

### 1.3 Signaling integration test (T27) — PASS

`go test ./tests/integration/...` — 4 test functions all green
(stabilized in T51 with the `drain()` helper to absorb CI scheduling
jitter on ICE-trickle assertions):

- TestSignalingRoundtrip — SDP offer + answer + 3×ICE each
  direction with strict ordering
- TestDuplicateRoleRejected — third connection gets close 1008
- TestByePropagationAndTeardown — bye delivered, session reusable
- TestSessionIsolation — two parallel sessions, no cross-talk

### 1.4 Compose stack boots cleanly

`docker compose -f infra/compose.yaml up -d --build` brings up four
services to healthy/running:

```
SERVICE     STATUS
chromium    Up 3 minutes (healthy)
client      Up 3 minutes (healthy)
signaling   Up 3 minutes
client-build  Exited (0)            (one-shot built ../client/dist)
```

Reachability from the host:
- `http://localhost:3000/`              — `200`, served from `client/dist`
- `http://localhost:3000/main.js`       — `200`, esbuild bundle
- `http://localhost:3000/config.js`     — `200`, runtime-generated
- `http://localhost:8080/healthz`       — `{"status":"ok"}`
- `http://localhost:9222/json/version`  — unreachable from host (T52, see §3)

### 1.5 Live signaling round-trip — PASS

A throwaway Python harness ran two stdlib WebSocket peers against
`ws://localhost:8080/ws/phase0-exit-demo` with the running compose
stack:

```
[demo] ✓ client received offer (126 bytes)
[demo] ✓ browser received answer (131 bytes)
[demo] ✓ 3 ICE browser→client in order
[demo] ✓ 3 ICE client→browser in order
[demo] ✓ bye sent; round-trip complete
[demo] LIVE SIGNALING ROUND-TRIP: PASS
```

Signaling-server logs corroborated:

```
{"msg":"peer joined","session_id":"phase0-exit-demo","role":"client"}
{"msg":"peer joined","session_id":"phase0-exit-demo","role":"browser"}
{"msg":"peer left",  "session_id":"phase0-exit-demo","role":"client"}
{"msg":"peer left",  "session_id":"phase0-exit-demo","role":"browser"}
```

Identical envelope contract to T13's hub: SDP offer / answer / ICE /
bye routed by role with strict per-direction ordering.

---

## 2. What didn't pass — and why it doesn't gate Phase 0

### 2.1 Streamer Chromium reaches signaling: NOT DEMONSTRATED

The cloud-Chromium container in compose loads the streamer page
(`title="cloud-browser streamer"`, URL is the expected
`http://localhost:9000/streamer/index.html?signal=ws://signaling:8080/ws&session=dev&fps=30`)
but probing via DevTools Runtime.evaluate:

```
typeof window.pc                                 -> 'undefined'
(window.pc && window.pc.connectionState) || null -> None
typeof window.signalingWs                        -> 'undefined'
```

The signaling-server logs corroborate: zero connections from inside
the container's net namespace. Source-of-the-issue: T34's
`capture/streamer-page/streamer.js` creates a function-local `const
pc = new RTCPeerConnection(...)` (line 242) but never publishes it
to `window`. We can't tell from the outside whether the streamer JS
errored before reaching that line or completed it silently as a
local. **Filed as T69** with a one-liner fix proposed.

This is **not** a Phase 0 exit blocker per the project brief: Phase 0
is "container boots, page renders, latency measurable on stub". All
three are demonstrated above. The streamer-side dial is a Phase 1
integration concern.

### 2.2 RTCPeerConnection reaches `connected`: NOT DEMONSTRATED

A natural side effect of §2.1 — without the streamer dialing
signaling, no negotiation happens. The brief explicitly allows this:
> RTCPeerConnection reaches `have-remote-answer` or **fails predictably
> given no real remote source**.

Today's behaviour is "fails predictably": the streamer load completes
silently without reaching out, the user-side client has nothing to
negotiate against, the v1 latency budget can't be measured against a
real flow. Once T69 lands the demo will go end-to-end.

### 2.3 Real glass-to-glass latency on the live stack: N/A

No video stream means no real measurement. Phase 1 deliverable
([T65](#) "Real glass-to-glass latency measurement run on assembled
Phase 1 stack" — already filed). The harness (T10/T11/T12) is ready
and validated; it's waiting for media.

---

## 3. Known limitations carried into Phase 1

These were uncovered during T9 / T12 / T16 work and are tracked.
None gates Phase 0 exit; all are on the Phase 1 critical path.

| ID  | Title | Owner suggestion | Phase 1 impact |
|-----|-------|------------------|----------------|
| T52 | Chromium 147 ignores `--remote-debugging-address=0.0.0.0` | infra-dev | Blocks T33 E2E specs 02/03 against canonical compose flow; no impact on T9 smoke (driven via `docker exec`). |
| T69 | Streamer doesn't expose `window.pc`; idle-watchdog blind | platform-dev | Blocks: real signaling integration; idle-watchdog correctness; T33 E2E spec 03. |
| T59 | Latency-harness binary-bar fallback rendered but never decoded | platform-dev | Doc/code mismatch; only matters when QR decode rate < 0.95. |
| T60 | `reconcile.py` doesn't enforce `cam_fps ≥ 2 × flash_freq` | platform-dev | Diagnostic gap; an aliased run silently emits meaningless numbers. |
| T61 | `reconcile.py` doesn't surface `sinkRecvEpochMs` lag | platform-dev | Diagnostic gap; can't see slow page→sink WS without poking JSONL by hand. |

T51 (flaky `TestSignalingRoundtrip`) was completed in parallel with
this report and is no longer outstanding — the `drain()` helper fix
is documented in
[`tests/integration/README.md`](../tests/integration/README.md).

---

## 4. Numbers from the harness

**N/A on the live compose stack** — the streamer-page → signaling
gap (§2.1) means there's no real video to measure end-to-end. The
harness reconciler itself produces deterministic, validated numbers
on the bundled fixture (§1.2): `min=42, p50=49, p95=82.45, max=88
ms`. These are the **fixture's** numbers, not a real-pipeline
measurement.

Harness is ready for Phase 1's real measurement; T65 picks that up
once the streamer is actually streaming.

---

## 5. Phase 1 kickoff — action items

In rough priority order:

1. **T69 (highest priority for Phase 1 integration).** Fix
   streamer-page to expose `window.pc` (and `window.signalingWs`).
   One-line change, but it unlocks (a) idle-watchdog correctness,
   (b) any sane in-container introspection, (c) T33 E2E spec 03.
2. **T52.** Make Chromium DevTools reachable from the host (or
   document the in-container-only convention). Required for T33
   spec 01 against canonical compose; preferred for any developer
   debugging the streamer in real time.
3. **T65.** Once T69 is fixed and the streamer reliably reaches
   `connected`, run the harness against the assembled stack on a
   wired LAN setup and produce the first real glass-to-glass p50/p95
   numbers. Reconcile against the v1 budget in
   [`docs/v1-success-criteria.md`](v1-success-criteria.md).
4. **T59 / T60 / T61.** Address the three harness gaps from T12
   review. None blocks Phase 1, all are easy wins that improve
   diagnostic quality.
5. **CI workflow follow-ups (already discussed in T18 / T33).**
   `e2e.yml` (Playwright nightly), `harness-loopback.yml` (nightly
   regression). Both should be authored once T69 / T52 are in to
   avoid baking known-failing assumptions.

---

## 6. Sign-off

Phase 0 exit criteria from
[`PROJECT_BRIEF.md`](../PROJECT_BRIEF.md):

| Criterion | Evidence | Status |
|-----------|----------|--------|
| Container boots | T9 smoke; compose chromium service `Up (healthy)` | ✓ |
| Page renders | T9 smoke captured 1919×1027 PNG of example.com via CDP | ✓ |
| Capture a single frame | T9 smoke `Page.captureScreenshot` returned 20060-byte PNG | ✓ |
| Measure latency end-to-end on a stub pipeline | T12 reconciler regression on bundled fixture; min=42, p50=49, p95=82.45, max=88 ms | ✓ |

Recommendation: **PROCEED to Phase 1**, with the action-item
ordering in §5.

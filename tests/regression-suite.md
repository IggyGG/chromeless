# Regression suite — canonical reference (T105)

> **Owner:** `qa-tester`. This document is the single source of truth
> for "what we run before we ship." Pair with
> [`tests/README.md`](./README.md) (the strategy doc) and
> [`docs/audits/phase1-stack-audit.md`](../docs/audits/phase1-stack-audit.md)
> (the v1-readiness verdict). When new tests land, they get added here
> first; when removed, here last.

---

## How to read this doc

Tests are grouped by **layer** (unit → integration → smoke → harness
→ e2e), and within each layer split into three **gates**:

| Gate          | When it must be green                | Runtime budget | Where it runs |
|---------------|--------------------------------------|----------------|----------------|
| **PR-blocking** | every PR, every push to `main`      | < 10 min total | `make test-all-ci` / GitHub `ci.yml` |
| **Nightly**    | every night + on touched-paths push  | < 30 min total | `make test-all-nightly` / `e2e.yml` + `harness-loopback.yml` |
| **Operator**   | manual, ahead of v1 sign-off         | physical rig   | run by hand per checklist |

Anything that is **not** in one of these three gates does not exist
for shipping purposes. If you write a test that doesn't fit, file a
PR adding it here in the same change.

---

## 1. Unit tests (PR-blocking)

Runtime budget: **< 60 s combined**. No Docker, no compose, no live
network. Fail fast.

### 1.1 Go modules

| Module | Test files | What it catches | What it doesn't | Runtime |
|--------|------------|-----------------|-----------------|---------|
| `signaling/` | `admin_test.go`, `auth_test.go`, `denylist_test.go`, `metrics_test.go`, `probe_test.go`, `replay_test.go`, `turn_test.go` | T48 token verify (Ed25519, exp/nbf/aud), T67 tenant routing, T89 denylist precedence + admin endpoint, T96 offer/answer replay buffer, T93 region-claim enforcement, T25 `/turn-credentials` shape, T38 metrics labels | Real WS round-trip (covered by `tests/integration/signaling_roundtrip_test.go`); cross-tenant collisions (covered by `tests/integration/cross_tenant_test.go`) | ~3 s |
| `infra/turn-issuer/` | `main_test.go` | T76 RFC 7635 HMAC-SHA1 credential format; secret rotation (`_PREV` overlap window); admin token gating | live coturn handshake | ~1 s |
| `infra/controllers/browser-session-controller/` | `pkg/reconciler/{pool,scrub,session}_test.go`, `pkg/server/admission_test.go` | T71 BrowserSession CRD reconcile; T90 scrub-and-return; admission webhook validation | live K8s API (envtest only); CRIU restore (T68 deferred) | ~5 s |
| `capture/cb-metrics-sidecar/` | `stats_handler_test.go` | T82 stats label cardinality cap; T72 stats forwarding payload shape | live Chromium DevTools poll | ~1 s |
| `capture/clipboard-bridge/` | `main_test.go` | T32 echo suppression, MIME guard, 1 MiB cap | live Chromium clipboard permissions | ~1 s |
| `capture/cursor-watcher/` | `main_test.go` | T26 envelope diff/dedupe; CSS-keyword vs custom shape parse | live cursor poll loop | ~1 s |
| `capture/file-bridge/` | `main_test.go` | T74 chunk reassembly + SHA-256 verify + MIME re-sniff; allowlist gate | live CDP `DOM.setFileInputFiles` | ~2 s |
| `capture/input-bridge/` | `main_test.go` | T22 envelope→CDP translation for all 14 input types incl. T46 drag, T56 touch, T62 scroll inertia, T88 IME composition | live `Input.dispatch*`; rate-limit (still Phase 2 gap per audit §5) | ~2 s |
| `capture/v4l2-writer/` | `cmd/v4l2-writer/main_test.go`, `internal/wire/reader_test.go` | T81/T92 magic-tagged wire format; chunk re-framing; size caps | live v4l2loopback ioctl path; pacat audio sink | ~2 s |

### 1.2 Client (TypeScript / Vitest)

| File | What it catches | Runtime |
|------|-----------------|---------|
| `client/src/auth.test.ts` | T48 client-side token fetch + retry + JWT decode | ~50 ms |
| `client/src/clipboard.test.ts` | T32 client-side copy/paste DC bridging | ~50 ms |
| `client/src/codec-negotiate.test.ts` | T54 codec preference inspection post-SDP | ~50 ms |
| `client/src/cursor.test.ts` | T26 client overlay positioning + visibility transitions | ~50 ms |
| `client/src/file-upload.test.ts` | T74 client-side chunking + lifecycle envelopes | ~100 ms |
| `client/src/input.test.ts` | T20 input encoder (coalescing, rAF flush, seq counter) | ~150 ms |
| `client/src/passthrough.test.ts` | T81 webcam/mic offer + renegotiation | ~100 ms |
| `client/src/probe.test.ts` | T102 connection-quality probe (in flight) | ~100 ms |
| `client/src/reconnect.test.ts` | T37 ReconnectingWebSocket exponential backoff | ~100 ms |
| `client/src/sdp.test.ts` | T30 SDP munging (codec preference + low-latency profile-level-id) | ~100 ms |
| `client/src/simulcast.test.ts` | T77 client-side simulcast layer config | ~100 ms |
| `client/src/stats.test.ts` | T42 stats sample emission + T82 labels | ~100 ms |
| `client/src/turn.test.ts` | T48/T76 TURN config fetch + STUN fallback | ~100 ms |

**Combined unit runtime:** ~12 s on a clean checkout (after `go test`
caching warms; first run is ~25 s for the Go side alone).

**Run via:**
- `make test-unit` — all of above (matches `test-unit-signaling`,
  `test-unit-client`, `test-unit-harness`)
- Per-module: `cd <module> && go test ./...` or `cd client && npm test`

---

## 2. Integration tests (PR-blocking)

Cross-component, real-binary, no Docker. Live in `tests/integration/`.
Builds the signaling binary in `TestMain` and exercises the wire
contract end-to-end.

| File | What it catches | What it doesn't | Runtime |
|------|-----------------|-----------------|---------|
| `signaling_roundtrip_test.go` | T13/T27/T51 — full SDP/ICE round-trip, duplicate-role rejection, bye propagation, two-session isolation | streamer/client real implementations | ~2 s (incl. Go build) |
| `offer_replay_test.go` | T96 — replay buffer keeps only most-recent offer per session; ICE explicitly NOT replayed (covered by [#104](#) followup); survives peer reconnect | T94 cross-region replay (Phase 3) | ~1 s |
| `audio_loopback_test.go::TestStreamerOffersAudio` | T64 — streamer's SDP offer carries `m=audio` + opus rtpmap | client-side audio decode (T64 spec 05) | ~22 s (live compose; opt-in via `CBWRTC_INTEGRATION_LIVE=1`) |
| `clipboard_test.go` | T32 — clipboard envelope round-trip through signaling, no echo | actual Chromium copy/paste (T64 e2e) | ~2 s |
| `cross_tenant_test.go` | T67 — `(tenant_id, session_id)` pairs route independently; collisions impossible across tenants | per-region cross-tenant (T93) | ~2 s |
| `file_upload_test.go` | T74 — file chunks reassemble across signaling under simulated re-order | actual CDP file attach | ~3 s |
| `input_loop_test.go` | T22/T41 — input envelope `client → DC → input-bridge → CDP-stub` round-trip | real Chromium `Input.dispatch*` | ~2 s |
| `stats_loop_test.go` | T42/T72/T82 — stats envelope `client → sidecar → /metrics` round-trip with cardinality cap | live Grafana scrape | ~2 s |

**PR-blocking subset:** every test EXCEPT `audio_loopback_test.go`
(opt-in because it spins up `docker compose`). The opt-in test runs
nightly via `e2e.yml` indirectly through Playwright spec 05.

**Combined integration runtime (PR-blocking):** ~15 s.

**Run via:** `make test-integration` (cd into `tests/integration/`
because it's its own Go module); set
`CBWRTC_INTEGRATION_LIVE=1` to also run audio_loopback.

---

## 3. Smoke tests (PR-blocking; Linux only)

Coarse-grained "container boots and renders" checks. `bash` scripts;
exit code is the sole signal.

| Script | What it catches | What it doesn't | Runtime | Linux-only? |
|--------|-----------------|-----------------|---------|-------------|
| `tests/smoke/container-boot.sh` | T9 — image builds, Chromium boots under Xvfb, DevTools responds, navigates to `https://example.com`, captures a real PNG > 5 KiB via CDP `Page.captureScreenshot` | streamer page itself; signaling | ~12 s (after build cached) | ✓ Docker required |
| `tests/smoke/audio-presence.sh` | T24 — PulseAudio null-sink cascade is alive; Chromium routes audio to capture sink | A/V sync; codec choice | ~10 s | ✓ |
| `tests/smoke/metrics-presence.sh` | T38 — `:9100/metrics` returns Prometheus exposition; required series present (`cb_chromium_*`, `cb_session_*`) | dashboard rendering (Grafana) | ~5 s | ✓ |
| `tests/smoke/security-posture.sh` | T57 — non-root, read-only fs, dropped caps, seccomp profile in effect | actual exploit attempt | ~5 s | ✓ |
| `tests/smoke/snapshot-restore.sh` | T68 — CRIU dump/restore primitive (Phase 3 stretch; gracefully skips when `criu` not installed) | session re-join after restore | ~5 s skip / ~30 s real | ✓ Linux + criu pkg |

**Smoke uses `docker exec` for all DevTools traffic** so it's
robust to T52 (Chromium 147 DevTools binding to loopback only) — no
host port forwarding required. See
[`tests/smoke/README.md`](./smoke/README.md).

**Run via:** `make test-smoke` (today only invokes
`container-boot.sh`; `make test-all-ci` runs all five).

---

## 4. Harness baselines (Nightly)

Reconciler-correctness + diagnostic-completeness checks. Hermetic
(deterministic fixtures, no webcam) — these test the *harness math*,
not the *physical pipeline*.

| Script | What it catches | Runtime |
|--------|-----------------|---------|
| `tests/harness/loopback-baseline.sh` | T12 — reconcile.py against `harness/latency/sample-input/` deterministic fixture; 8 tight bounds (frames_seen=4, qr_decode_rate=1.0, negative_count=0, min/p50/p95/max ms) | ~3 s |
| `tests/harness/aliased-warning-baseline.sh` | T60 — reconcile.py warns when `cam_fps < 2 × flash_freq` (Nyquist rule) | ~3 s |
| `tests/harness/sink-lag-baseline.sh` | T61 — reconcile.py surfaces `sink_lag_p{50,95,max}_ms` when `--jsonl` is supplied | ~3 s |
| `tests/harness/input-latency-loopback.sh` | T39 — input-latency reconciler against bundled fixture | ~3 s |
| `tests/harness/y4m-loopback-baseline.sh` | T109 — y4m fixture-correctness check (decodes 30 frames from `harness/latency/fixtures/harness-loop-720p30.y4m`; auto-regenerates the fixture if missing). Asserts qr_decode_rate ≥ 0.5 (proves the fixture isn't synthetic-green-square) | ~5 s (hermetic when fixture cached) |
| `tests/harness/phase1-baseline.sh` | T65 — operator wrapper against a real cam.mp4 + jsonl; budget assertion + baseline JSON write | **Operator-only** (needs cam) |

**Run via:** `make test-harness` (loopback only) or `make test-all-nightly`
(all four hermetic baselines). `phase1-baseline.sh` is invoked
directly by the operator per `docs/measurements/phase1-loopback-TEMPLATE.md`.

---

## 5. End-to-end (Nightly)

Playwright specs in `tests/e2e/`. Drive a real Chromium against the
running compose stack. Single shared session per spec; serial only
(`fullyParallel: false`, `workers: 1`).

| Spec | What it catches | Status today | Gating dependency |
|------|-----------------|--------------|-------------------|
| `01-signaling-handshake.spec.ts` | Client reaches stub-or-connected state within 10 s of clicking Connect | **ACTIVE — passes** | none |
| `02-data-channel-open.spec.ts` | `input` data channel reaches `open` state | `test.skip` pending T34 (now landed; un-skip pending re-run) | T34 + working live pipeline |
| `03-receives-video-track.spec.ts` | Client gets a video `RTCRtpReceiver` | `test.skip` pending T34 + capture path | T34 + real screen capture (T78 real fix or FrameSink) |
| `04-streamer-exposes-pc-hook.spec.ts` | Regression guard for T16-followup — `window.pc` is set on the streamer page | **ACTIVE — passes** (uses `connectOverCDP`) | T52 |
| `05-audio-receives.spec.ts` | Client `inbound-rtp` audio `bytesReceived > 0` | **FAILS today** — [#108](#) (client `state-conn` stays `"—"`); audio_loopback Go test confirms protocol layer is fine | #108 client-side fix |
| `06-camera-passthrough.spec.ts` | T81 webcam passthrough — bytes flow client→cloud | gated on T81 + a fake-cam permission flag | (Phase 4) |

**Run via:** `make test-e2e` or `( cd tests/e2e && npm run test:e2e )`.
Requires `npx playwright install --with-deps chromium` once.

---

## 6. Operator-only (Sign-off gate)

Cannot run from CI. Requires physical hardware.

| Script | What it catches | Prerequisites |
|--------|-----------------|---------------|
| `tests/harness/phase1-baseline.sh` | Real glass-to-glass + input-latency on the assembled stack vs the v1 budget | Linux x86_64 host, 16 vCPU/32 GB; webcam ≥ 60 fps with locked AE/AF; NTP-synced source + cam hosts; real-screen-capture path (T78 real fix or FrameSink); operator following [`tests/harness/validation.md`](./harness/validation.md) §4 |
| Phase 1 deployment checklist walk | T80 — every "should be ✓ before letting traffic in" item | A real K8s cluster |

**These don't gate PRs** — they gate **v1 sign-off**, per
[`docs/audits/phase1-stack-audit.md`](../docs/audits/phase1-stack-audit.md)
§4.1.

---

## 7. The orchestrator: `tests/run-regression.sh`

Drives the full suite (PR-blocking + Nightly subsets) end-to-end and
prints a colored green/yellow/red summary. Useful as a pre-PR check
on a developer machine, or to walk the suite before declaring v1
sign-off.

```sh
bash tests/run-regression.sh             # full sequence, colored summary
bash tests/run-regression.sh --pr-only   # PR-blocking subset only
bash tests/run-regression.sh --nightly-only
```

Exit codes:
- **0** — all run layers green
- **1** — at least one layer red (test failure)
- **2** — at least one layer yellow (skipped because pre-req missing,
  e.g. no Docker on macOS); no reds; consult human

The script does NOT pretend a yellow run is the same as a green run.
Skips are fine for a developer's pre-push check; for v1 sign-off
every layer must be green on a real Linux runner.

---

## 8. Workflow audit (the CI gates)

| Workflow                | Trigger                            | Gates which subset                                    | Notes |
|-------------------------|-------------------------------------|-------------------------------------------------------|-------|
| `.github/workflows/ci.yml` | every PR + push to main          | lint + build-image + smoke (`container-boot.sh`) + docs link-check | PR-blocking. Wall-time target < 10 min. |
| `.github/workflows/e2e.yml` | every PR + push to main + manual | docker compose + Playwright (all `tests/e2e/0*.spec.ts`) | Nightly-equivalent (runs every PR; treats `*.live.spec.ts` as best-effort to tolerate transient T78 issues). 30-minute timeout. |
| `.github/workflows/harness-loopback.yml` | nightly cron + push to harness-touched paths + manual | 4 hermetic baselines (loopback / aliased-warning / sink-lag / input-latency); compares against `tests/harness/baseline-thresholds.json` and files an issue if regression > 5 ms p50 / 10 ms p95 | Nightly. Auto-files an issue with `regression` + `harness` labels. |
| `.github/workflows/codeql.yml` | per its own schedule              | security scan; not a regression gate                  | informational |
| `.github/workflows/release.yml` | tag push                        | release pipeline; not a regression gate               | informational |

Only the first three are regression gates. `ci.yml` runs the suite I
call PR-blocking; `e2e.yml` + `harness-loopback.yml` together cover
my Nightly subset.

**Audit findings (2026-04-30):**
- `ci.yml` runs only `container-boot.sh` from the smoke layer. It
  doesn't run `audio-presence`, `metrics-presence`, `security-posture`,
  or `snapshot-restore`. Fix: extend the workflow's smoke step to call
  `make test-smoke-all` (added in this commit) — see follow-up below.
- `e2e.yml` runs all six specs, but spec 05 is a known fail
  ([#108](#)). The workflow currently treats spec failures as job
  failures. Either un-skip in `*.live.spec.ts` style or fix #108
  before the next nightly is meaningful.
- `harness-loopback.yml`'s `baseline-thresholds.json` doesn't exist
  yet — the workflow records but does not compare on first runs (per
  its own warning). First baseline lands when an operator runs
  `phase1-baseline.sh` on the real rig.

---

## 9. PR-blocking vs Nightly: the canonical split

**PR-blocking (must be green on every PR before merge):**

- All §1 unit tests
- All §2 integration tests EXCEPT `audio_loopback_test.go`
- §3 smoke `container-boot.sh` (and once `make test-smoke-all` extension lands, all five smoke scripts on Linux runners)

**Nightly (must be green every night; failures file issues, not
block PRs):**

- All four §4 hermetic harness baselines
- All §5 e2e specs, including the live-stack ones (with `*.live.spec.ts`
  treatment for T78-dependent specs)

**Operator-only (must be green for v1 sign-off):**

- §6 phase1-baseline.sh against the real rig
- Manual walk of the deployment checklist

---

## 10. Cross-references

- **Strategy:** [`tests/README.md`](./README.md)
- **v1 success criteria:** [`docs/v1-success-criteria.md`](../docs/v1-success-criteria.md)
- **v1 readiness audit:** [`docs/audits/phase1-stack-audit.md`](../docs/audits/phase1-stack-audit.md)
- **Harness validation methodology:** [`tests/harness/validation.md`](./harness/validation.md)
- **First operator measurement report:** [`docs/measurements/phase1-loopback-2026-04-30.md`](../docs/measurements/phase1-loopback-2026-04-30.md)

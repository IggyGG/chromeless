# Testing strategy

This document is the entry point for everything we test in
`chromeless`. It is the contract between the dev teammates
(`chromium-dev`, `webrtc-dev`, `infra-dev`, `platform-dev`) and the
`qa-tester` role, and the playbook for anyone landing a PR.

> **Status:** Phase 0. The harness, smoke, and E2E layers are all in flight
> (T9, T10–T12, T16). Where a layer is not yet implemented, this doc names
> the owning task so the reader can find it.

## 1. Test pyramid

We use a five-layer pyramid. Each layer has a clear owner, a clear directory,
and an opinion about what it costs to run.

```
           ┌──────────────────────────────────────┐
   slow    │  E2E:  user-perceived flows          │  tests/e2e/   (future)
           │  (browser-to-cloud-Chromium loop)    │
           ├──────────────────────────────────────┤
           │  Measurement harness:                │  harness/  +
           │  glass-to-glass latency, input RTT   │  tests/harness/
           ├──────────────────────────────────────┤
           │  Smoke: container & service boot     │  tests/smoke/
           ├──────────────────────────────────────┤
           │  Integration: cross-component        │  tests/integration/
           │  (signaling roundtrip, DC, capture)  │
           ├──────────────────────────────────────┤
   fast    │  Unit: per-subproject                │  signaling/, client/,
           │                                      │  capture/, harness/
           └──────────────────────────────────────┘
```

### 1a. Unit tests — per-subproject, fast, no Docker

Owned by the subproject author; live next to the code they cover.

| Subproject     | Framework             | Location                          |
| -------------- | --------------------- | --------------------------------- |
| `signaling/`   | Go `testing` + `go test` | `signaling/**/*_test.go`       |
| `client/`      | Vitest                | `client/src/**/*.test.ts`         |
| `capture/`     | Whatever the sidecar lang lands on (likely Go or C++ gtest) | `capture/**/test_*` |
| `harness/`     | Pytest                | `harness/**/test_*.py`            |

**Rule:** every PR that adds production code in a subproject adds or modifies
unit tests in the same subproject. Unit tests must run on macOS and Linux
without Docker.

### 1b. Integration tests — cross-component, no container required

Cross-component checks that need a real built artifact but no Docker — Go
toolchain (or eventually Node + Python) is enough. Each surface
(`signaling`, `capture`, the input bridge, …) gets its own subdirectory
or test file under `tests/integration/`. The subdirectory is its own Go
module so it can build sibling modules cleanly; see
[`tests/integration/README.md`](./integration/README.md) for the
mechanics and conventions.

Today:

| Surface       | Test file                                    | Covers                                                                 |
| ------------- | -------------------------------------------- | ---------------------------------------------------------------------- |
| `signaling/`  | `tests/integration/signaling_roundtrip_test.go` | SDP offer/answer, 3×ICE each direction with ordering, duplicate-role rejection (1008), `bye` propagation + clean teardown, session isolation. T27. |
| `capture/`    | _(future)_                                   | Capture-sidecar handshake + frame format negotiation.                  |
| input bridge  | _(future)_                                   | Data-channel input protocol round-trip against the dispatch service.   |

Owner: the subproject whose interface is the unit-under-test, with
`qa-tester` reviewing for cross-cutting coverage.

### 1c. Smoke tests — `tests/smoke/`

Coarse-grained "does the container boot and render a page" checks. Slow
enough that we don't run them on every keystroke, fast enough to gate every
PR. Tracked by **T9** (`tests/smoke/container-boot.sh`). Requires Docker on
the runner.

### 1d. Measurement harness — `harness/` and `tests/harness/`

The flashing-color-block latency harness that backs the LAN <100 ms /
regional <200 ms contract from the project brief. Two halves:

- `harness/` — the artifact under test (the page, reconciliation script,
  webcam capture utilities). Built by **T10** (page + design doc) and
  **T11** (`harness/latency/reconcile.py`).
- `tests/harness/` — methodology validation, threats-to-validity probes,
  loopback baseline, and assertions about the harness itself. Owned by
  `qa-tester` via **T12** (`tests/harness/validation.md` +
  `tests/harness/loopback-baseline.sh`).

The harness is **infrastructure**, not a passing/failing test. It produces
numbers; the **validation** layer is what asserts those numbers are
trustworthy. See §6 below.

### 1e. End-to-end tests — `tests/e2e/` (Playwright)

Drive a real browser client against a real container with a real
signaling server and assert user-visible properties: the client loads,
signaling completes, the input data channel opens, a video receiver
shows up, etc. Scaffolded by **T33** (Playwright + TypeScript). Today
spec 01 (signaling handshake reaches the documented stub state) is
active; specs 02–03 are written and `test.skip`'d pending **T34**
("flip T14 client to answerer role"), so they un-skip with a one-line
edit when the negotiation flow is correct. Mechanics, run procedures,
and the un-skip checklist live in
[`tests/e2e/README.md`](./e2e/README.md).

## 2. Run procedures

A top-level `Makefile` stubs the four canonical targets. Until each
underlying layer exists, the target prints what it *will* do and exits
non-zero with a clear "not implemented" message — so `make` is a single
discoverable surface and CI can wire to it from day one.

```sh
make test-unit         # all subproject unit tests
make test-integration  # tests/integration/ — Go toolchain only, no Docker
make test-smoke        # tests/smoke/ — requires Docker
make test-harness      # tests/harness/ validation pass; requires loopback rig
make test-e2e          # tests/e2e/ — Phase 1+
make test              # = test-unit + test-integration + test-smoke (the PR gate)
```

Per-subproject details:

```sh
# signaling — Go
( cd signaling && go test ./... )

# client — TypeScript
( cd client && npm ci && npm test )

# harness — Python (Pytest)
( cd harness && python -m pytest )

# smoke — bash, requires Docker
bash tests/smoke/container-boot.sh

# integration — Go (own module per surface; cd in)
( cd tests/integration && go test ./... )
```

The `make test-unit` aggregator runs whichever of the above subproject
suites currently exist; missing ones are skipped with a warning, not a
failure. (T9, T13, T14 will each turn one of those skips into a real run.)

## 3. CI plan

We will run CI on GitHub Actions. The workflow files are intentionally
deferred — that is a separate follow-up task — but the shape is:

| Workflow            | Trigger             | Runner            | What it does                                  |
| ------------------- | ------------------- | ----------------- | --------------------------------------------- |
| `unit.yml`          | every PR, every push | `ubuntu-latest` + `macos-latest` matrix | `make test-unit` |
| `smoke.yml`         | every PR             | `ubuntu-latest` (Docker available) | `make test-smoke` |
| `harness-loopback.yml` | nightly + manual  | self-hosted Linux with webcam rig | `make test-harness`; uploads numbers as artifact |
| `e2e.yml`           | merge to `main`, nightly | self-hosted Linux | `make test-e2e` (Phase 1+) |
| `lint.yml`          | every PR             | `ubuntu-latest`   | `gofmt`, `eslint`, `ruff`, `shellcheck`       |

Notes:

- Anything that needs Docker runs on Linux only. Docker on macOS GH-hosted
  runners is not supported (and emulation would invalidate latency numbers
  anyway).
- The harness loopback is **not** a PR gate — it is too physical. It runs
  nightly to catch drift; PRs that touch `harness/` or capture/encode hot
  paths can opt in via a `ci:harness` label.
- Self-hosted runners are required for `harness-loopback.yml` and
  `e2e.yml` because both need a real container host (and the harness needs
  the webcam rig).

Workflow YAML lands in a follow-up after T9 + T11 are green so each
workflow has something real to call.

## 4. Quality bars

A PR is mergeable when **all** of the following hold:

1. **Unit tests pass on every subproject the PR touches.** No skipping
   subproject suites with TODOs in PR descriptions.
2. **`make test-smoke` passes** — container boots, Chromium renders a page,
   noise from supervisord stays clean. (Once T9 lands.)
3. **No harness regression beyond budget.** If the PR touches anything in
   `capture/`, `signaling/`, encoder config, or container resource limits,
   the author runs the harness on a loopback rig and pastes the numbers in
   the PR description. The merge bar is:
   - LAN P50 ≤ 100 ms, P95 ≤ 130 ms.
   - Regional P50 ≤ 200 ms, P95 ≤ 260 ms.
   - No more than 5 ms regression versus the baseline on `main`, or the PR
     must explain the trade.
4. **Lint clean** — `gofmt -d`, `eslint`, `ruff`, `shellcheck`, `hadolint`
   on Dockerfiles.
5. **No flaky tests merged.** A test is flaky if it fails on a clean
   re-run without code change. The flakiness policy is **quarantine,
   never silence**: a flaky test is moved to a `Skip("flaky: <issue#>")`
   with a tracking issue opened the same day, and the issue is fixed
   before the next release. PRs may not introduce new `t.Skip("flaky")`.

## 5. Test data and fixtures

Where things live and what is allowed in-tree.

| What                            | Lives in                          | Size cap         |
| ------------------------------- | --------------------------------- | ---------------- |
| Tiny synthetic HTML/JSON inputs | `tests/<layer>/fixtures/`         | < 64 KB / file   |
| Generated test pages (harness)  | `harness/latency/`                | < 1 MB / file    |
| Webcam captures (harness rig)   | **NOT in git** — `tests/harness/captures/` is `.gitignore`d; uploads are CI artifacts on `harness-loopback.yml` runs | external |
| Reference encoder bitstreams    | `tests/fixtures/bitstreams/` (Git LFS once we need it) | < 5 MB / file before LFS |
| Golden number outputs from harness | `tests/harness/baselines/*.json` | < 64 KB / file   |

**Hard rules:**

- No binary > 1 MB checked in to plain Git. If you need it, propose Git LFS
  in a PR description and link it from this table.
- No `.mp4` / `.webm` / `.raw` / `.yuv` in plain Git, ever — they are
  nightly-CI artifacts.
- Fixture data must be reproducible: every fixture has a sibling generator
  script or a comment documenting how it was produced.

Test artifact retention on CI: 14 days for PR runs, 90 days for nightly
harness runs.

## 6. Threats to validity (cross-reference to T12)

The harness is the spine of every latency claim we make. It is also the
easiest piece of the project to fool ourselves with. T12
(`tests/harness/validation.md`) is the authoritative methodology document;
this README only restates the four named threats so a reviewer knows to look
for them in any PR that touches the harness or the capture/encode hot path:

1. **Frame-rate aliasing** between the flashing-color-block page and the
   webcam capture rate. Probe by detuning the page rate against fixed
   webcam rates and checking that recovered latency is invariant.
2. **Clock skew** between the page-side timestamp source and the capture
   host. Probe with a synchronized clock source (NTP discipline + monotonic
   delta logging) and a loopback baseline.
3. **Rolling shutter** distortion on consumer webcams. Probe by comparing
   recovered latency from a global-shutter reference camera vs. a rolling-
   shutter consumer cam on the same flashing source.
4. **Ambient light / exposure** bias on the color transition detector.
   Probe with controlled lighting + multiple exposure settings, and assert
   detection threshold robustness.

Loopback baseline: **`tests/harness/loopback-baseline.sh`** (T12 deliverable)
runs the harness against a same-host Chromium so the only added latency is
capture + encode + decode + display. Any nontrivial recovered latency from
that run is a harness bug, not a system latency, and blocks subsequent
production measurements.

## Pointers

- **Interactive suite: [`tests/interactive/`](./interactive/)** — real client,
  real deployed worker, every result checked against the worker's own DevTools.
  The only tests here that answer "does clicking, typing and scrolling actually
  work". Currently 24/28 by design; the four failures are confirmed embedder
  defects with named fix sites.
- Smoke tests: [`tests/smoke/`](./smoke/)
- Harness validation: [`tests/harness/`](./harness/)
- Harness artifact: [`harness/`](../harness/)
- Project brief, latency budget, definition of done: [`PROJECT_BRIEF.md`](../PROJECT_BRIEF.md)
- v1 success criteria: [`docs/v1-success-criteria.md`](../docs/v1-success-criteria.md)

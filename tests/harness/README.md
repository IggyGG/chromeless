# Harness validation

This is the **validation** half of the latency harness. It asserts
that the numbers `harness/` produces are trustworthy. The harness
artifact itself — flashing-color-block page, webcam capture,
reconciliation script — lives under [`../../harness/`](../../harness/).

## What lives here

- **`validation.md`** — full methodology document. Five named threats
  to validity (frame-rate aliasing, clock skew, rolling shutter,
  ambient light/exposure, plus monitor-refresh-vs-paint), calibration
  procedure, recommended ranges, operator run checklist, and a PR-style
  review of T10/T11 with three filed follow-ups (T59/T60/T61). T12
  deliverable.

- **`loopback-baseline.sh`** — hermetic CI-friendly regression check
  for `harness/latency/reconcile.py`. Runs the reconciler against the
  bundled deterministic [`sample-input/`](../../harness/latency/sample-input/)
  fixture and asserts eight tight bounds (frames_seen, qr_decode_rate,
  negative_count, min_ms, p50_ms, p95_ms, max_ms, qr_decoded) derived
  from the sample manifest. T12 deliverable. **Not** a physical-rig
  loopback — that's the operator step in `validation.md` §4.

- **`phase1-baseline.sh`** — operator wrapper for a real-pipeline
  measurement run (T65). Takes a webcam mp4 + the source-side JSONL
  + a recording-start epoch ms, runs `reconcile.py`, asserts numbers
  against the v1 budget for the chosen target (`loopback`/`lan`/`regional`),
  and persists a baseline JSON under `baselines/phase1-<runId>.json`
  for future regression comparisons (per the `tests/README.md` ≤5 ms
  drift rule). Updates the `baselines/phase1-latest.json` symlink on
  success. See `--help` for full flag set.

- **`baselines/`** — committed JSON snapshots from
  `phase1-baseline.sh`. Each file is the canonical record of a real
  measurement run; future PRs assert no >5 ms drift vs
  `phase1-latest.json`. Empty until the first real Phase 1 measurement
  run lands — see [`docs/measurements/phase1-loopback-2026-04-30.md`](../../docs/measurements/phase1-loopback-2026-04-30.md)
  §6 for the gating items (T86 + #96).

- `captures/` — **`.gitignore`d** (covered by repo-level `.gitignore`'s
  `harness/captures/` entry plus test-data policy in
  [`../README.md`](../README.md#5-test-data-and-fixtures)). Webcam
  clips are nightly-CI artifacts, never checked in.

## How to run

```sh
make test-harness          # from repo root
# or directly:
bash tests/harness/loopback-baseline.sh
```

Requires `python3` plus the deps in
[`harness/latency/requirements.txt`](../../harness/latency/requirements.txt)
(opencv-python, pyzbar, numpy, matplotlib). On macOS also
`brew install zbar`; the script auto-sets `DYLD_FALLBACK_LIBRARY_PATH`
when libzbar is in `/opt/homebrew/lib` or `/usr/local/lib`.

The script is **hermetic** — no webcam, no recording, no network. It
catches regressions in `reconcile.py` itself (parsing, time math,
percentile calc, manifest precedence). The physical webcam-pointed-at-
source loopback procedure for the *operator* is documented in
[`validation.md`](./validation.md) §4 and is not part of this script.

## CI hookup

Currently invoked via `make test-harness` on demand. Per
[`../README.md`](../README.md#3-ci-plan) the eventual
`harness-loopback.yml` GitHub Actions workflow will run this nightly
on a self-hosted Linux runner. Heavyweight deps (opencv, pyzbar,
matplotlib) keep it out of the default PR gate.

## Conventions

- `validation.md` is authoritative: any change to the harness that
  could affect the five named threats must update `validation.md` in
  the same PR.
- The eight assertion bounds in `loopback-baseline.sh` are derived
  from `sample-input/manifest.jsonl`. If a future PR intentionally
  extends the sample, update both in lockstep.
- New follow-ups against T10/T11 land as TaskCreate entries
  cross-referenced from `validation.md` §6.

## See also

- Overall testing strategy: [`../README.md`](../README.md)
- Harness artifact (page, sink, reconciler):
  [`../../harness/latency/`](../../harness/latency/)
- Operator-facing calibration procedure:
  [`../../harness/latency/calibrate.md`](../../harness/latency/calibrate.md)
- Project latency budget:
  [`../../PROJECT_BRIEF.md`](../../PROJECT_BRIEF.md)

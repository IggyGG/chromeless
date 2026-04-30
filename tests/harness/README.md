# Harness validation

This is the **validation** half of the latency harness. It asserts that the
numbers `harness/` produces are trustworthy. The harness artifact itself —
flashing-color-block page, webcam capture, reconciliation script — lives
under [`../../harness/`](../../harness/).

> **Status:** Phase 0. Lands with **T12**, blocked on T11 (reconciliation
> logic). Until T12 ships, this directory is a placeholder.

## What lives here

- `validation.md` — methodology document covering the four threats to
  validity (frame-rate aliasing, clock skew, rolling shutter, ambient
  light / exposure). T12 deliverable.
- `loopback-baseline.sh` — runs the harness against a same-host Chromium so
  the only added latency is capture + encode + decode + display. T12
  deliverable. Any nontrivial recovered latency from this run is a harness
  bug, not a system latency, and blocks subsequent production measurements.
- `baselines/*.json` — golden recovered-latency numbers from the loopback
  rig, checked in. Regressions vs these are tracked in CI nightly.
- `captures/` — **`.gitignore`d.** Webcam clips are CI artifacts only; see
  the test-data policy in [`../README.md`](../README.md#5-test-data-and-fixtures).

## How to run

```sh
make test-harness          # from repo root
# or directly:
bash tests/harness/loopback-baseline.sh
```

Requires the loopback rig (a webcam pointed at a same-host display) to be
attached. Not a PR gate — runs nightly via the `harness-loopback.yml`
workflow described in [`../README.md`](../README.md#3-ci-plan).

## Conventions

- The validation doc is authoritative: any change to the harness that
  could affect the four named threats must update `validation.md` in the
  same PR.
- Recovered-latency baselines are reset **only by an explicit commit** with
  the rationale in the message. Drifting them silently defeats the point.

## See also

- Overall testing strategy: [`../README.md`](../README.md)
- Harness artifact: [`../../harness/`](../../harness/)
- Project latency budget: [`../../PROJECT_BRIEF.md`](../../PROJECT_BRIEF.md)

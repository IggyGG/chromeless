# Smoke tests

Coarse-grained "does the container boot and render a page" checks. Slow
enough that we don't run them on every keystroke, fast enough to gate every
PR that touches `infra/` or anything the container ships.

> **Status:** Phase 0. The first script (`container-boot.sh`) lands with
> **T9**, blocked on T7 (Dockerfile) finishing.

## What lives here

- `container-boot.sh` — boots `infra/Dockerfile`, waits for Chromium to
  paint a known reference page, asserts on a healthcheck (HTTP 200 + a DOM
  fingerprint), tears the container down. Must catch real regressions —
  do not just check that the image builds. (T9.)

## How to run

```sh
make test-smoke          # from repo root
# or directly:
bash tests/smoke/container-boot.sh
```

Requires Docker on the host. Linux runners only on CI.

## Conventions

- Each smoke check is a single self-contained shell script: no helper libs,
  no test runner. They must be readable by anyone on the team in five
  minutes.
- Exit code is the sole signal: 0 = pass, non-zero = fail. Diagnostic
  output goes to stderr.
- No flakiness budget — if a smoke fails, we treat it like a smoke alarm.
  See the flakiness policy in [`../README.md`](../README.md#4-quality-bars).

## See also

- Overall testing strategy: [`../README.md`](../README.md)
- Container under test: [`../../infra/`](../../infra/)

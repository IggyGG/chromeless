# Integration tests

Cross-component tests that don't need a running container — just a Go
toolchain. Sit between unit tests (in each subproject) and the
container-level smoke (`tests/smoke/`) in the project pyramid; see the
[overall strategy](../README.md#1-test-pyramid) for the pyramid context.

## What's here

- **`signaling_roundtrip_test.go`** — end-to-end check of the v0
  signaling server (T13). Builds the `signaling/` binary in `TestMain`,
  launches it as a subprocess on a free port, and exchanges WebSocket
  envelopes via `gorilla/websocket` clients. Covers:
  - SDP `offer` round-trip (browser → client) and `answer` round-trip
    (client → browser).
  - 3 ICE candidates each direction with ordering assertions.
  - Duplicate-role rejection — third connection trying to claim
    `client` gets `ClosePolicyViolation` (1008).
  - `bye` propagation, server-side close of the bye-sender, and clean
    session teardown (the same `session_id` can be re-used right after).
  - Session isolation: two concurrent sessions on the same server do
    not leak frames between each other.

This is independent of T14 (the browser client) — both peer roles are
played by raw `*websocket.Conn` test clients.

## How to run

```sh
# from the repo root, via the canonical entry point:
make test-integration

# or directly (this directory is its own Go module):
( cd tests/integration && go test ./... )
( cd tests/integration && go test -race ./... )
( cd tests/integration && go test -v ./... )       # verbose
( cd tests/integration && SIGNALING_TEST_LOGS=1 go test -v ./... )   # also dump server logs on success
```

A passing run takes ~2 s including the one-time `go build` of the
signaling binary. Server stdout/stderr is captured per-server and only
printed via `t.Logf` if the test fails (or `SIGNALING_TEST_LOGS=1` is
set). With `-race` the run takes ~3 s and is clean.

### Why a subprocess and not in-process?

The `signaling/` binary is `package main`, so its symbols can't be
imported from a sibling Go module. We build it once in `TestMain` and
launch it on a free port for each test. This keeps T13's package
layout untouched and tests the actual artifact end-to-end (the same
binary the Dockerfile ships). If we later split `signaling/` into a
library + thin `main`, this scaffolding can be replaced with an
in-process `httptest.Server`; the test bodies wouldn't change.

## CI hookup

Wired into `make test-integration`, which is included in the default
`make test` target — so the eventual `unit.yml` GitHub Actions
workflow described in [`../README.md`](../README.md#3-ci-plan) picks
this up for free. Requires only the Go toolchain on the runner; no
Docker, no webcam rig, no signal-able external deps. Runs on
`ubuntu-latest` and `macos-latest` matrices.

## Conventions

- One Go module per integration-test surface where the unit-under-test
  lives in its own module. Today: just `signaling`. When `capture/` or
  the input bridge become testable, add sibling test files (or a new
  `tests/integration/<surface>/` subdir with its own module).
- No flakiness. The only timing assumption is the 5 s `/healthz` poll
  on server startup; everything else uses 2 s read deadlines on
  loopback. If a test flakes, treat it as a real bug per the
  [flakiness policy](../README.md#4-quality-bars).
- The signaling binary is built fresh on every `go test` invocation —
  there is no stale-cache hazard.

## See also

- Overall testing strategy: [`../README.md`](../README.md)
- The artifact under test: [`../../signaling/`](../../signaling/)
- Smoke tests (container-level, downstream of this layer):
  [`../smoke/`](../smoke/)

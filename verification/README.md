# verification/ — ChromelessV2 native-peer gate (M0)

The CI gate for the ChromelessV2 native-WebRTC migration. Replaces the JS
streamer with a browser-process WebRTC peer; this directory verifies that
the replacement actually landed (not just the deletion of the old code).

## Layout

```
verification/
  native-peer-gate.mjs              # CLI entrypoint (R1)
  INTEGRATION_BASE.txt              # base SHA of integration/native-peer (R9)
  README.md                         # this file
  package.json                      # devDep: yaml (R8 tests)
  lib/
    registry.mjs                    # assertion registry + Status enum (R1)
    verdict.mjs                     # mode/schedule verdict algebra (R1 + R2)
    image-fs.mjs                    # source/image probe helper (R3, reused by R4)
    harness.mjs                     # container-boot + in-netns CDP (R7)
  assertions/
    streamer-page.mjs               # #1 (R3)
    bridges.mjs                     # #2 (R4)
    codec-cap.mjs                   # #3 (R5)
    native-peer.mjs                 # #4 (R6)
  fixtures/
    synthetic-clean-streamer/       # PASS fixture for R3
    synthetic-clean-bridges/        # PASS fixture for R4
    chromeless_synthetic-*/         # synthetic image-manifest.json for R3/R4
    cdp-codecs/                     # post-m1-expected.json + current-image-libwebrtc-default.json (R5)
    native-peer-loopback/peer-stub.mjs   # in-process TCP peer-stub for R6 PASS path
  scripts/
    cb-encoder-probe-v3.mjs         # R5 capture script (re-fixture from real chromeless:ci)
    with-container.sh               # R7 shell wrapper
    check-branch-protection.sh      # R8 compensating evidence
  __tests__/                        # node:test suites for every R#
```

## Two modes

```bash
node verification/native-peer-gate.mjs --scaffold --image=chromeless:ci
node verification/native-peer-gate.mjs --strict   --image=chromeless:ci
```

| Mode      | Behavior                                                         |
|-----------|-------------------------------------------------------------------|
| scaffold  | PASS unless an assertion FAILed OR an NYI assertion is past its gate-blocking marker. Permissive — the everyday integration-branch signal. |
| strict    | PASS only when every assertion is PASS. Required for the M7 PR to chromeless/main. |

Always emits a single JSON object to stdout matching:

```jsonc
{
  "schema": "native-peer-gate",
  "schemaVersion": 1,
  "mode": "scaffold",
  "position": "M0",
  "imageTag": "chromeless:ci",
  "gate": { "verdict": "PASS", "exitCode": 0 },
  "assertions": [
    { "id": "...", "number": 1, "title": "...", "status": "NOT_YET_IMPLEMENTED",
      "evidence": { ... }, "durationMs": 12, "blocked": false }
  ]
}
```

stderr is human-readable progress only (per-assertion table + verdict summary).

## Gate-blocking schedule (ratified)

| # | id                          | enablingModule | gateBlockingAt |
|---|-----------------------------|----------------|----------------|
| 1 | streamer-page-absent        | M7             | M7             |
| 2 | bridges-absent              | M7             | M7             |
| 3 | codec-cap-matches-factory   | M1             | M1             |
| 4 | native-peer-connects        | M3             | M3             |

The streamer/sidecars exist until M7 deletes them — gate-blocking them
earlier would fail every M1–M6 scaffold run, contradicting the operator's
explicit UAT (scaffold green against the current image). Source of truth:
`verification/lib/verdict.mjs::DEFAULT_SCHEDULE`. Override at runtime with
`--schedule=/path/to/schedule.json`.

## CI wiring (R8)

`.github/workflows/native-peer-gate.yml` runs both modes:

- **scaffold** — every PR + every push to `integration/native-peer`. Always-on signal.
- **strict** — PRs targeting `main` (so M7's squash-merge is gated) + pushes to `integration/native-peer` (operator check).

Both jobs build `chromeless:ci` once (with gha layer cache reuse) and share the resulting tar via `actions/upload-artifact`.

### Branch-protection (compensating evidence)

Forgejo branch-protection rules live OUTSIDE the repo. Operators MUST add
`native-peer-gate / native-peer-gate-scaffold` to required status checks
for `integration/native-peer` (and `main` for the M7 PR).

Inspect with:

```bash
FORGEJO_TOKEN=... ./verification/scripts/check-branch-protection.sh \
  --owner triform --repo chromeless --branch integration/native-peer
```

Exits 0 if the rule is present, non-zero otherwise. Cited in M0 closeout.

## Running tests locally

```bash
node --test verification/__tests__/
```

R7 (container-boot harness) tests skip cleanly when docker daemon or
`chromeless:ci` is unavailable — exercised in CI. Everything else runs
fully on a workstation with Node 20+.

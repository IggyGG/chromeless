# Smoke tests

Coarse-grained "does the container boot and render a page" checks. Slow
enough that we don't run them on every keystroke, fast enough to gate
every PR that touches `infra/` or anything the container ships.

## What lives here

- **`container-boot.sh` (T9)** — builds the image if needed, runs a
  detached container with the lifecycle idle-watchdog disabled
  (`IDLE_TIMEOUT_S=999999`), drives Chromium via the DevTools Protocol
  to navigate a real URL, captures a screenshot, and asserts the
  screenshot is a valid PNG > 5 KiB. Designed to catch real
  regressions: a launcher that crashes, a Dockerfile that builds but
  doesn't actually start Chromium, an Xvfb misconfiguration, etc. —
  not just "does the image build".

  Layered checks (each step fails loud and identifies which step
  failed):

  1. Build the image if not already present locally.
  2. Start the container detached; poll DevTools for liveness up to 60 s.
  3. Assert `/json/version` returns HTTP 200 and a `Browser` identifier.
  4. Discover the first page target and its `webSocketDebuggerUrl`.
  5. Drive CDP: `Page.navigate` → wait for `document.readyState=complete`.
  6. `Page.captureScreenshot` (PNG) and base64-decode to disk.
  7. Assert PNG file magic and size > 5 KiB.

## How to run

```sh
make test-smoke              # from repo root, via the canonical entrypoint
# or directly:
bash tests/smoke/container-boot.sh
```

Requires only Docker + python3 on the host. No pip installs, no
`curl`-on-host requirement: all DevTools traffic runs `docker exec` into
the container and a stdlib-only WebSocket client lives inside the
script. Works on macOS (Apple Silicon + Intel) and on Linux runners.

### Why we drive DevTools from inside the container

As of Chromium 147, `--remote-debugging-address=0.0.0.0` is silently
ignored — DevTools binds 127.0.0.1 only — so a `-p 9222:9222` on
`docker run` cannot reach it. Filed as a follow-up (T52) so the launch
flag set is fixed and host-side tooling (the E2E suite, dev inspection)
works again. Until that lands, running the smoke from inside the
container's net namespace bypasses the binding bug entirely. When the
bug is fixed, this smoke continues to pass without modification —
`docker exec` works either way.

A second motivation: pip is not installed in the image (filed as
followup T53 — unrelated to T9 but visible from this work), so any
host- or container-side library install would fail. The smoke embeds a
minimal RFC 6455 WebSocket client (~80 lines, stdlib only) so it is
wholly self-contained.

### Knobs

All optional; sensible defaults baked in:

| Env                       | Default                       | Notes |
|---------------------------|-------------------------------|-------|
| `SMOKE_IMAGE_TAG`         | auto-detect (`:ci` → `:dev` → build) | Override to test a specific tag. |
| `SMOKE_FORCE_REBUILD`     | `0`                           | `1` forces `docker build` even if a tag exists. |
| `SMOKE_NAVIGATE_URL`      | `https://example.com`         | Override to an offline target if the runner has no public outbound. |
| `SMOKE_SCREENSHOT_PATH`   | `/tmp/chromeless-smoke.png`           | Where the captured screenshot lands. |
| `SMOKE_READY_TIMEOUT_S`   | `60`                          | Max time to wait for DevTools to come up. |
| `SMOKE_LOAD_TIMEOUT_S`    | `30`                          | Max time to wait for navigation to complete. |

### Auto-detected image tag

In CI, `build-image` produces `chromeless:ci`; the smoke
auto-detects this so `./tests/smoke/container-boot.sh` Just Works
without env tweaks. Locally, dev convention is
`chromeless:dev`, which the smoke also picks up. If neither
tag exists, the smoke builds `:dev` from `infra/Dockerfile` with the
repo root as the build context (matching the Dockerfile's `COPY` lines
and CI's build-image job).

### What success looks like

A passing run prints, on stderr, something like (real run output —
`example.com` rendered through Chromium 147 inside the container,
captured via DevTools, ~12 s end-to-end on Apple Silicon):

```
== 1. resolve image ==
[smoke] auto-detected image: chromeless:dev
== 2. start container ==
[smoke] container started: <id>
[smoke] polling DevTools (inside container) for up to 60s...
[smoke] DevTools is up after 3s
== 3. /json/version ==
  Browser:          Chrome/147.0.7727.116
  Protocol-Version: 1.3
== 4. discover page target ==
[smoke] page target: ws://127.0.0.1:9222/devtools/page/<...>
== 5. drive CDP (navigate + screenshot) ==
[cdp] Page.enable
[cdp] Page.navigate https://example.com
[cdp] readyState=complete
[cdp] Page.captureScreenshot
[cdp] wrote /tmp/chromeless-smoke.png (20060 bytes inside container)
== 7. validate screenshot ==
[smoke] screenshot OK: /tmp/chromeless-smoke.png (20060 bytes, valid PNG magic)
== smoke passed ==
[smoke] container-boot: PASS
```

Exit code 0 = pass. Any non-zero exit prints a `[smoke] FAIL: ...`
message identifying the failing step plus the last 80 lines of the
container's logs to make triage fast.

### What it deliberately doesn't test

This smoke is about the **container** booting Chromium and rendering.
It does NOT exercise:

- The signaling server. That's covered by
  [`tests/integration/signaling_roundtrip_test.go`](../integration/).
- The cloud-Chromium actually streaming media. That requires the full
  pipeline (signaling server reachable, the cloud peer answering); see
  [`tests/e2e/`](../e2e/) and the harness validation T12 work.
- Latency. That's the harness's job — see
  [`harness/`](../../harness/) and
  [`tests/harness/`](../harness/).

## Conventions

- Each smoke check is a single self-contained shell script: no helper
  libs, no test runner. They must be readable by anyone on the team in
  five minutes.
- Exit code is the sole signal: 0 = pass, non-zero = fail. Diagnostic
  output goes to stderr.
- No flakiness budget — if a smoke fails, we treat it like a smoke
  alarm. See the flakiness policy in
  [`../README.md`](../README.md#4-quality-bars).

## CI hookup

`tests/smoke/container-boot.sh` is wired into the `smoke` job in
`.github/workflows/ci.yml`. The job runs on `ubuntu-latest`, restores
the image cache produced by `build-image`, and runs the script
directly. Auto-detect picks up `chromeless:ci`, so no env
tweak is needed in the workflow YAML.

## See also

- Overall testing strategy: [`../README.md`](../README.md)
- Container under test: [`../../infra/`](../../infra/)
- Lifecycle scripts (idle watchdog, cold-start): [`../../infra/lifecycle/`](../../infra/lifecycle/)

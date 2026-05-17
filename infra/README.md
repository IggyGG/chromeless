# infra/

Container image and (Phase 3) orchestration for the chromeless
runtime.

## Files

| File | Purpose |
|------|---------|
| `Dockerfile` | Base image: Debian bookworm-slim + Chromium + Xvfb + PulseAudio (null sink) + python3 + supervisord. |
| `supervisord.conf` | Process supervisor; starts Xvfb → PulseAudio → Chromium → devtools-proxy → metrics-sidecar. M7: native peer (M1+M3) replaced the in-container streamer page and idle-watchdog. |
| `pulse-default.pa` | PulseAudio bootstrap script, copied to `/etc/pulse/default.pa`. Two null sinks (cb_audio playback + cb_capture intermediary) plus a loopback — see `audio-routing.md`. |
| `launch-chromeless.sh` | Wrapper invoked by supervisord; takes `SESSION_ID` / `SIGNALING_URL` / `CHROMIUM_START_URL` and execs Chromium with the native-peer flag list. |
| `audio-routing.md` | Topology + manual smoke procedure for the in-container audio path. |
| `lifecycle/` | Container session lifecycle (T31): `entrypoint.sh`, `cold-start.sh`, `idle-watchdog.sh`, `restart.sh`, `README.md`. |
| `compose.yaml` | Local dev stack: real signaling (T13) + chromium with lifecycle wiring + nginx-served client. |

## Build

The build context is the **repo root** (not `infra/`), because the
Dockerfile COPYs runtime glue from `infra/*` alongside the Chromium
binary built by `build/chromeless-build.sh`:

```
docker build -t chromeless:dev -f infra/Dockerfile .
```

## Run

```
docker run --rm \
    -p 9222:9222 \
    -p 8080:8080 \
    --shm-size=1g \
    chromeless:dev
```

Confirm Chromium is alive from the host:

```
curl http://localhost:9222/json/version
```

## Validation status (2026-04-30, T7)

- `hadolint infra/Dockerfile` — clean (DL3008 suppressed with rationale
  in the Dockerfile).
- `supervisord.conf` parsed by Python `configparser` — all sections and
  keys recognised.
- **Real boot test deferred to Linux CI**: the dev box is macOS without a
  running Docker daemon. T9 (container smoke test, owned by qa-tester)
  will run the actual `docker build` + `docker run` against this image
  and report.

## Known limitations (v1 expedients)

- `--no-sandbox` is set on Chromium so the image runs on any host kernel
  without `CAP_SYS_ADMIN`. **Do not ship to multi-tenant infra as-is.** A
  follow-up will replace this with the real Chromium sandbox or per-
  session gVisor/Firecracker isolation (see Phase 3 in `PROJECT_BRIEF.md`).
- `--remote-allow-origins=*` accepts DevTools connections from any
  origin. Fine for local dev; lock down before any public deployment.
- `apt` versions are intentionally unpinned so Debian security updates
  flow through. Phase 3 will introduce a deterministic apt snapshot.

# infra/

Container image and (Phase 3) orchestration for the cloud-browser-webrtc
runtime.

## Files

| File | Purpose |
|------|---------|
| `Dockerfile` | Base image: Debian bookworm-slim + Chromium + Xvfb + PulseAudio (null sink) + python3 + supervisord. |
| `supervisord.conf` | Process supervisor; starts Xvfb → PulseAudio → streamer-static → Chromium → idle-watchdog. |
| `pulse-default.pa` | PulseAudio bootstrap script, copied to `/etc/pulse/default.pa`. Two null sinks (cb_audio playback + cb_capture intermediary) plus a loopback — see `audio-routing.md`. |
| `launch-chromium.sh` | Wrapper invoked by supervisord; expands `SESSION_ID` / `SIGNALING_URL` / `STREAMER_FPS` into the streamer URL then execs Chromium with the full T28 flag list. |
| `audio-routing.md` | Topology + manual smoke procedure for the in-container audio path. |
| `lifecycle/` | Container session lifecycle (T31): `entrypoint.sh`, `cold-start.sh`, `idle-watchdog.sh`, `restart.sh`, `README.md`. |
| `compose.yaml` | Local dev stack: real signaling (T13) + chromium with lifecycle wiring + nginx-served client. |

## Build

The build context is the **repo root** (not `infra/`), because the
Dockerfile COPYs `capture/streamer-page/` alongside `infra/*`:

```
docker build -t cloud-browser-webrtc:dev -f infra/Dockerfile .
```

## Run

```
docker run --rm \
    -p 9222:9222 \
    -p 8080:8080 \
    --shm-size=1g \
    cloud-browser-webrtc:dev
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

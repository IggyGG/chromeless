# infra/

Container image and (Phase 3) orchestration for the cloud-browser-webrtc
runtime.

## Files

| File | Purpose |
|------|---------|
| `Dockerfile` | Base image: Debian bookworm-slim + Chromium + Xvfb + PulseAudio (null sink) + supervisord. |
| `supervisord.conf` | Process supervisor config; starts Xvfb, PulseAudio, Chromium in that order. |
| `pulse-default.pa` | PulseAudio bootstrap script, copied to `/etc/pulse/default.pa`. Null sink only — no host audio. |

## Build

```
docker build -t cloud-browser-webrtc:dev infra/
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

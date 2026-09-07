# infra/

Everything around the browser: the standalone gateway, the compose stacks, the
Helm chart and controller, and the glue that runs inside the worker image.

> This file previously documented an `infra/Dockerfile` and a
> `docker build -t chromeless:dev -f infra/Dockerfile .`. **That Dockerfile does
> not exist.** It was the legacy stock-Chromium + `getDisplayMedia` image, which
> had not been buildable since the M7 native-peer migration deleted
> `capture/streamer-page/` while the Dockerfile still `COPY`d it. The worker
> image is now [`build/Dockerfile.runtime`](../build/Dockerfile.runtime), built
> by `build/chromeless-build.sh`.

## Layout

| Path | Purpose |
| --- | --- |
| `gateway/` | **The standalone front door.** TLS, login, session tokens, navigation, and a reverse proxy to the broker. The only host-facing port in a compose deployment — see [its README](./gateway/README.md). |
| `compose.yaml` | Single-host stack: gateway + broker + worker, plus opt-in `turn` and `observability` profiles. |
| `compose.host.yaml` / `compose.worker.yaml` | The same stack split across two machines. See [`docs/operations/standalone.md`](../docs/operations/standalone.md). |
| `helm/chromeless/` | Kubernetes chart: `BrowserSession` / `BrowserSessionPool` CRDs, controller, coturn, turn-issuer. |
| `controllers/browser-session-controller/` | The Go controller behind those CRDs. |
| `turn-issuer/` | RFC 7635 TURN-REST credential issuer (HMAC, rotation-aware). What a multi-user deployment should use instead of static credentials. |
| `k8s/` | Raw manifests: an alternative to the chart, plus the build-lane Job. |
| `lifecycle/` | Runs **inside the worker image**: `entrypoint.sh` → `cold-start.sh` → supervisord, plus `idle-watchdog.sh`, `restart.sh`, `scrub-pod.sh`. |
| `launch-chromeless.sh` | Invoked by supervisord. Translates `SESSION_ID` / `SIGNALING_URL` / `CHROMELESS_ICE_SERVERS` into the `WEBRTC_*` vars the browser process reads, then execs Chromium with the native-peer flag list. |
| `supervisord.phase2.conf` | Process supervisor inside the image: Xvfb → PulseAudio → Chromium → devtools-proxy. |
| `devtools-proxy.sh` | socat bridge. Chromium 147+ ignores `--remote-debugging-address` and binds DevTools to loopback only, so a container port mapping reaches nothing without this. |
| `pulse-default.pa`, `audio-routing.md` | The in-container audio path. |
| `seccomp/`, `security-hardening.md` | Sandbox profiles and hardening notes. |
| `observability/`, `observability.md` | Prometheus config and Grafana dashboards for the `observability` profile. |
| `snapshots/` | Firecracker warm-snapshot tooling. |

## Running it

```bash
CHROMELESS_IMAGE=my-registry/chromeless:cr7727-abc1234 make standalone-up
```

`standalone-up.sh` mints the three credentials (the gateway's signing key, the
broker's verifying key, and `SIGNALING_TOKEN`, the worker's own — all three, or
the broker refuses the browser and only a WebSocket close code says so) into
`infra/.env` once, then runs `docker compose -f infra/compose.yaml up --wait`.

Then <https://localhost:8443>. The full guide, including TURN and the
split-host layout, is
[`docs/operations/standalone.md`](../docs/operations/standalone.md).

`CHROMELESS_IMAGE` is required: the browser is a from-source Chromium build
(4–8h cold) and this repo publishes no images.

## One published port

`compose.yaml` publishes the gateway and nothing else. Two things that used to
be reachable from the host no longer are:

- **9222 (DevTools)** — unauthenticated remote code execution against the
  browser. The image also passes `--remote-allow-origins=*`, so a web page
  could reach it.
- **8080 (signaling)** — with no `CHROMELESS_AUTH_PUBKEY` the broker accepts
  anyone, so a published port is a way straight past the login.

Reach either through the container when debugging:

```bash
docker compose -f infra/compose.yaml exec chromium \
  curl -s http://127.0.0.1:9222/json/version
```

## Known limitations

- **`--no-sandbox`** is set so the image runs on any host kernel without
  `CAP_SYS_ADMIN`. Do not ship to multi-tenant infrastructure as-is; use
  per-session gVisor/Firecracker isolation.
- **`--remote-allow-origins=*`** accepts DevTools connections from any origin.
  Survivable only because the port is not published — do not republish it.
- **apt versions are unpinned** so Debian security updates flow through.

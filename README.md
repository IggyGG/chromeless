# cloud-browser-webrtc

Open-source cloud browser with low-latency WebRTC streaming.

> **Status:** Phase 0 — bootstrap. This README is a stub. `platform-dev` will expand it as part of task `T2`.

## Goal

A user runs `docker compose up`, opens `http://localhost:8080`, and gets a working
remote Chromium tab with acceptable latency for typical browsing. Linux
server-side, containerized, software-encode for v1.

## Repo layout (target)

```
signaling/   WebSocket signaling server (Go)
capture/     Headless Chromium capture sidecar / spike branches
client/      Browser-side client SDK
infra/       Dockerfile, compose.yaml, K8s manifests
harness/     Glass-to-glass latency measurement harness
docs/        Design docs, prior-art surveys, ADRs
tests/       Smoke + integration + harness tests
```

## License

Apache-2.0 — see `LICENSE`.

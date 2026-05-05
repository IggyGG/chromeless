# infra/Dockerfile — chromeless base container
#
# Boots Chromium inside a virtual X server with a null-sink PulseAudio
# instance, supervised by supervisord. DevTools is exposed on :9222 so the
# capture and signaling layers can drive Chromium remotely. :8080 is
# reserved for the future signaling server (see T13).
#
# Base choice: debian:bookworm-slim
#   - Debian ships Chromium as a real apt package; Ubuntu 24.04 only ships
#     it via snap, which is a non-starter inside a container.
#   - bookworm-slim keeps the base layer small and tracks Debian stable.
#   - We can move to trixie when it goes stable without a rewrite — the
#     package set below is unchanged on trixie.
#
# NOTE on --no-sandbox: this is a v1 expedient so the image runs on any
# host kernel without CAP_SYS_ADMIN gymnastics. A future task will replace
# this with the real Chromium sandbox (or gVisor/Firecracker per session,
# see Phase 3 in PROJECT_BRIEF.md). DO NOT ship this image to production
# multi-tenant infra as-is.
#
# Build (build context = repo root so we can COPY capture/streamer-page/
# and capture/chromeless-metrics-sidecar/):
#   docker build -t chromeless:dev -f infra/Dockerfile .
#
# Run (interactive smoke test):
#   docker run --rm -p 9222:9222 -p 9100:9100 \
#       --shm-size=1g \
#       chromeless:dev
#
# Inspect from the host while it's running:
#   curl http://localhost:9222/json/version
#   curl http://localhost:9100/metrics

# ---------------------------------------------------------------------------
# Build stage: chromeless-metrics-sidecar (T38). A small Go service that polls
# Chromium DevTools + /proc and exposes Prometheus metrics on :9100.
# Built from source here so the runtime image is the only artifact
# anyone deploys.
# ---------------------------------------------------------------------------
FROM golang:1.22-alpine AS metrics-build
WORKDIR /src
COPY capture/chromeless-metrics-sidecar/go.mod  capture/chromeless-metrics-sidecar/go.sum ./
RUN go mod download
COPY capture/chromeless-metrics-sidecar/*.go ./
ENV CGO_ENABLED=0 GOOS=linux
RUN go build -trimpath -ldflags="-s -w" -o /out/chromeless-metrics-sidecar .

# ---------------------------------------------------------------------------
# Runtime image
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    DISPLAY=:99 \
    HOME=/home/cbuser \
    XDG_RUNTIME_DIR=/run/user/1000 \
    PULSE_RUNTIME_PATH=/run/user/1000/pulse \
    PULSE_SERVER=unix:/run/user/1000/pulse/native

# Runtime stack: Chromium + virtual display + audio + process supervisor
# + python3 (used by the in-container static server hosting the streamer
# page; see infra/supervisord.conf [program:streamer-static]).
# fonts-noto-* and fonts-dejavu give us Latin + CJK + emoji coverage so
# the latency harness pages don't render as tofu. procps is in here for
# `ps` during smoke debugging; drop it later if image size matters.
#
# Versions are intentionally unpinned in Phase 0: we want Debian's
# Chromium and ffmpeg-related packages to track stable security updates.
# Phase 3 will revisit this with an apt snapshot or repository pin.
# hadolint ignore=DL3008
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        chromium \
        chromium-common \
        xvfb \
        pulseaudio \
        pulseaudio-utils \
        supervisor \
        dumb-init \
        ca-certificates \
        curl \
        python3 \
        python3-websocket \
        socat \
        fonts-noto-core \
        fonts-noto-cjk \
        fonts-noto-color-emoji \
        fonts-dejavu-core \
        procps \
        # T81 — webcam/mic passthrough. v4l2loopback-utils gives us
        # `modprobe v4l2loopback` + the v4l2-ctl binary the v4l2-writer
        # follow-up will use. The kernel module itself is loaded at
        # session-pod startup either by a privileged container (compose
        # dev) or by a host-DaemonSet (production). The image carries
        # only the userland tools.
        v4l2loopback-utils \
 && rm -rf /var/lib/apt/lists/*

# Non-root user. After T57, cbuser is the *whole* runtime user — there
# is no privilege drop in supervisord; PID 1 (dumb-init), entrypoint.sh,
# cold-start.sh, supervisord, and every [program:*] all run as cbuser.
# That removes a class of bugs (root supervisord -> non-root child)
# and lets us run with --user 1000 / runAsNonRoot: true cleanly.
#
# Writable directories that MUST be cbuser-owned in the image fs (so a
# plain `docker run --read-only` works) and re-created by cold-start.sh
# at container start (so K8s emptyDir overlays don't strand us with an
# empty /run):
#   /home/cbuser              chromium profile, history, supervisor pid
#   /home/cbuser/.config/chromium     chromium user-data-dir
#   /run/user/1000/pulse              PulseAudio native socket
#   /run/supervisor                   supervisord socket + pid file
#   /run/chromeless-session                   cold-start.sh writes id + env
#   /var/log/supervisor               program logs
# Everything else under / is read-only — see infra/security-hardening.md.
RUN groupadd --gid 1000 cbuser \
 && useradd --uid 1000 --gid 1000 --create-home --shell /bin/bash cbuser \
 && mkdir -p /run/user/1000/pulse \
             /run/supervisor \
             /run/chromeless-session \
             /home/cbuser/.config/chromium \
             /var/log/supervisor \
 && chown -R cbuser:cbuser /run/user/1000 /run/supervisor /run/chromeless-session /home/cbuser /var/log/supervisor \
 && chmod 0700 /run/user/1000

# PulseAudio script: null sink only, no host audio. We *replace* the
# distro's /etc/pulse/default.pa so the daemon doesn't auto-load
# module-alsa-sink etc. on startup; -F is additive and would not stop
# the default script from running. See pulse-default.pa for module-level
# rationale.
COPY infra/pulse-default.pa /etc/pulse/default.pa
COPY infra/supervisord.conf /etc/supervisor/supervisord.conf

# Streamer page (T23 product) and its launch wrapper (T28). The static
# server in supervisord serves /opt/cloud-browser, so the URL path
# /streamer/index.html resolves to the page chromium-dev shipped.
COPY capture/streamer-page/ /opt/cloud-browser/streamer/
COPY infra/launch-chromeless.sh /usr/local/bin/launch-chromeless.sh

# T52: small socat sidecar that bridges container_eth0:9222 ->
# 127.0.0.1:9222 because Chromium 147 binds DevTools to loopback
# regardless of --remote-debugging-address. See infra/devtools-proxy.sh
# for the full rationale.
COPY infra/devtools-proxy.sh /usr/local/bin/devtools-proxy.sh

# T81: passthrough init script — modprobes v4l2loopback in the
# privileged compose-dev path; no-op on the host-DaemonSet path
# where /dev/video10 is pre-mounted. See infra/init-passthrough.sh
# and docs/protocols/webcam-mic-passthrough.md for the deployment
# shape rationale.
COPY infra/init-passthrough.sh /usr/local/bin/init-passthrough.sh

# Metrics sidecar binary (T38), built in the metrics-build stage above.
COPY --from=metrics-build /out/chromeless-metrics-sidecar /usr/local/bin/chromeless-metrics-sidecar

# Lifecycle scripts (T31): entrypoint chain, per-boot setup, idle
# watchdog, manual restart. See infra/lifecycle/README.md.
COPY infra/lifecycle/ /usr/local/lib/cloud-browser/lifecycle/
RUN ln -sf /usr/local/lib/cloud-browser/lifecycle/entrypoint.sh    /usr/local/bin/entrypoint.sh \
 && ln -sf /usr/local/lib/cloud-browser/lifecycle/cold-start.sh    /usr/local/bin/cold-start.sh \
 && ln -sf /usr/local/lib/cloud-browser/lifecycle/idle-watchdog.sh /usr/local/bin/idle-watchdog.sh \
 && ln -sf /usr/local/lib/cloud-browser/lifecycle/restart.sh       /usr/local/bin/restart.sh \
 && chmod +x /usr/local/lib/cloud-browser/lifecycle/*.sh \
 && chmod +x /usr/local/bin/launch-chromeless.sh \
 && chmod +x /usr/local/bin/devtools-proxy.sh \
 && chown -R cbuser:cbuser /opt/cloud-browser

# T57: seccomp profile. COPYing into the image lets `docker run
# --security-opt seccomp=/etc/chromeless-seccomp.json` work without an external
# file. K8s reads seccomp from the kubelet's seccomp dir
# (/var/lib/kubelet/seccomp/) so this image-baked file is informational
# for K8s; the deploy step has to install it on each node. See
# infra/security-hardening.md.
COPY infra/seccomp/chromeless.json /etc/chromeless-seccomp.json

EXPOSE 9222 8080 9100

# DevTools exposes /json/version as soon as Chromium accepts connections,
# which is our cheapest liveness signal. start-period gives Xvfb +
# PulseAudio + Chromium time to come up cleanly on a cold container.
HEALTHCHECK --interval=10s --timeout=3s --start-period=30s --retries=3 \
    CMD curl -fsS http://127.0.0.1:9222/json/version >/dev/null || exit 1

# T57: drop to cbuser as the default runtime user. RUN steps above
# needed root for apt-get and chown; nothing past this line should.
USER cbuser

# dumb-init as PID 1 so SIGTERM/SIGINT propagate to supervisord and the
# child tree, and zombies get reaped if a Chromium helper crashes. The
# lifecycle entrypoint runs cold-start.sh, sources the resolved session
# env, then execs supervisord (T31).
ENTRYPOINT ["/usr/bin/dumb-init", "--", "/usr/local/bin/entrypoint.sh"]

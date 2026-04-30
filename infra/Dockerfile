# infra/Dockerfile — cloud-browser-webrtc base container
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
# Build:
#   docker build -t cloud-browser-webrtc:dev infra/
#
# Run (interactive smoke test):
#   docker run --rm -p 9222:9222 -p 8080:8080 \
#       --shm-size=1g \
#       cloud-browser-webrtc:dev
#
# Inspect from the host while it's running:
#   curl http://localhost:9222/json/version

FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    DISPLAY=:99 \
    HOME=/home/cbuser \
    XDG_RUNTIME_DIR=/run/user/1000 \
    PULSE_RUNTIME_PATH=/run/user/1000/pulse \
    PULSE_SERVER=unix:/run/user/1000/pulse/native

# Runtime stack: Chromium + virtual display + audio + process supervisor.
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
        fonts-noto-core \
        fonts-noto-cjk \
        fonts-noto-color-emoji \
        fonts-dejavu-core \
        procps \
 && rm -rf /var/lib/apt/lists/*

# Non-root user. Chromium and PulseAudio run as cbuser; supervisord stays
# as root so it can write to /var/log/supervisor and manage the runtime
# directory, then drops privs per [program:*] block via user=.
RUN groupadd --gid 1000 cbuser \
 && useradd --uid 1000 --gid 1000 --create-home --shell /bin/bash cbuser \
 && mkdir -p /run/user/1000/pulse \
             /home/cbuser/.config/chromium \
             /var/log/supervisor \
 && chown -R cbuser:cbuser /run/user/1000 /home/cbuser \
 && chmod 0700 /run/user/1000

# PulseAudio script: null sink only, no host audio. We *replace* the
# distro's /etc/pulse/default.pa so the daemon doesn't auto-load
# module-alsa-sink etc. on startup; -F is additive and would not stop
# the default script from running. See pulse-default.pa for module-level
# rationale.
COPY pulse-default.pa /etc/pulse/default.pa
COPY supervisord.conf /etc/supervisor/supervisord.conf

EXPOSE 9222 8080

# DevTools exposes /json/version as soon as Chromium accepts connections,
# which is our cheapest liveness signal. start-period gives Xvfb +
# PulseAudio + Chromium time to come up cleanly on a cold container.
HEALTHCHECK --interval=10s --timeout=3s --start-period=30s --retries=3 \
    CMD curl -fsS http://127.0.0.1:9222/json/version >/dev/null || exit 1

# dumb-init as PID 1 so SIGTERM/SIGINT propagate to supervisord and the
# child tree, and zombies get reaped if a Chromium helper crashes.
ENTRYPOINT ["/usr/bin/dumb-init", "--"]
CMD ["/usr/bin/supervisord", "-c", "/etc/supervisor/supervisord.conf"]

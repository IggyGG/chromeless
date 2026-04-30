# syntax=docker/dockerfile:1.7
#
# v4l2-writer — native helper that consumes the streamer-page →
# Unix socket frame stream (T81) and writes:
#   --mode=video → /dev/video10 (v4l2loopback I420 frames)
#   --mode=audio → PulseAudio sink via pacat
#
# We finish in **debian-bookworm-slim**, NOT distroless, because:
#   1. Audio mode shells out to pacat (pulseaudio-utils) for the
#      PulseAudio protocol handshake.
#   2. Distroless's missing /etc/nsswitch.conf has historically
#      tripped up pacat's PA_NATIVE_PROTOCOL_LOOKUP path.
# The v4l2 syscalls themselves go through libc's ioctl wrappers,
# which work everywhere — but pacat is the constraint.

# ---------- build ----------
FROM golang:1.22-alpine AS build
WORKDIR /src

COPY go.mod go.sum* ./
RUN go mod download

COPY . .
ENV CGO_ENABLED=0 GOOS=linux
RUN go build -trimpath -ldflags="-s -w" -o /out/v4l2-writer ./cmd/v4l2-writer
RUN go test ./...

# ---------- runtime ----------
FROM debian:bookworm-slim
WORKDIR /

# pulseaudio-utils provides pacat (audio mode); v4l-utils provides
# v4l2-ctl for diagnostics (not strictly required at runtime, but
# operators tail the bridge logs and want to be able to poke the
# device manually). We don't pull in the full PulseAudio daemon —
# the writer talks to a daemon already running in the cloud
# Chromium pod.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        pulseaudio-utils \
        v4l-utils \
        ca-certificates \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /out/v4l2-writer /usr/local/bin/v4l2-writer

# Run as a non-root user. v4l2 device access requires being in the
# `video` group (gid 44 on Debian); the K8s manifest mounts the
# device with mode 0666 OR runs the container with that gid.
RUN useradd --system --uid 65532 --gid 44 cbwriter \
 && mkdir -p /run/cb-passthrough \
 && chown cbwriter:44 /run/cb-passthrough
USER 65532:44

# Default to video mode; audio mode is selected by the K8s args
# override per-sidecar.
ENTRYPOINT ["/usr/local/bin/v4l2-writer"]
CMD ["--mode=video", "--socket=/run/cb-passthrough/video.sock", "--device=/dev/video10"]

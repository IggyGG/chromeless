# syntax=docker/dockerfile:1.7
#
# cursor-watcher — emits v1 cursor envelopes from CDP into a downstream
# sink. See capture/cursor-watcher/README.md for runtime configuration.

# ---------- build ----------
FROM golang:1.22-alpine AS build
WORKDIR /src

COPY go.mod go.sum* ./
RUN go mod download

COPY . .

ENV CGO_ENABLED=0 GOOS=linux
RUN go build -trimpath -ldflags="-s -w" -o /out/cursor-watcher .
RUN go test ./...

# ---------- runtime ----------
FROM gcr.io/distroless/static-debian12:nonroot
WORKDIR /
COPY --from=build /out/cursor-watcher /cursor-watcher

# Defaults assume the watcher runs as a sidecar in the chromium pod
# and writes envelopes to a localhost ws relay. The image has no
# shell, so to override these flags pass them through your
# orchestrator (compose `command:` / k8s `args:`).
ENTRYPOINT ["/cursor-watcher", \
            "--cdp-url", "http://127.0.0.1:9222", \
            "--sink", "ws", \
            "--sink-url", "ws://127.0.0.1:9200/cursor"]

USER nonroot:nonroot

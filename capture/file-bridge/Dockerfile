# syntax=docker/dockerfile:1.7
#
# file-bridge — server-side file-upload sidecar (T74).
# See capture/file-bridge/README.md for runtime configuration.

# ---------- build ----------
FROM golang:1.22-alpine AS build
WORKDIR /src

COPY go.mod go.sum* ./
RUN go mod download

COPY . .
ENV CGO_ENABLED=0 GOOS=linux
RUN go build -trimpath -ldflags="-s -w" -o /out/file-bridge .
RUN go test ./...

# ---------- runtime ----------
FROM gcr.io/distroless/static-debian12:nonroot
WORKDIR /
COPY --from=build /out/file-bridge /file-bridge

# Defaults match the in-pod sidecar layout. Override at run time.
EXPOSE 9400

# distroless has no shell, so flags are baked in here. Orchestrator
# can override via `command:` or `args:` to vary --upload-dir,
# --session-id, --mime-allowlist, etc.
ENTRYPOINT ["/file-bridge", \
            "--ws-addr",      "0.0.0.0:9400", \
            "--ws-path",      "/files", \
            "--cdp-url",      "http://127.0.0.1:9222", \
            "--upload-dir",   "/var/lib/cb-uploads"]

USER nonroot:nonroot

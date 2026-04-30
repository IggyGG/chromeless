# syntax=docker/dockerfile:1.7
#
# input-bridge — server-side WebRTC-input → CDP forwarder.
# See capture/input-bridge/README.md for runtime configuration.

# ---------- build ----------
FROM golang:1.22-alpine AS build
WORKDIR /src

# Cache module downloads on go.mod/go.sum changes.
COPY go.mod go.sum ./
RUN go mod download

COPY . .

# Static, stripped binary suitable for distroless.
ENV CGO_ENABLED=0 GOOS=linux
RUN go build -trimpath -ldflags="-s -w" -o /out/input-bridge .

# Optional: run tests at build time so a broken commit doesn't ship.
RUN go test ./...

# ---------- runtime ----------
FROM gcr.io/distroless/static-debian12:nonroot
WORKDIR /
COPY --from=build /out/input-bridge /input-bridge

# Defaults match the local-dev compose layout. Override at run time.
ENV INPUT_BRIDGE_SOURCE=ws \
    INPUT_BRIDGE_WS_ADDR=0.0.0.0:9100 \
    INPUT_BRIDGE_CDP_URL=http://127.0.0.1:9222 \
    INPUT_BRIDGE_METRICS_ADDR=0.0.0.0:9101

EXPOSE 9100 9101

USER nonroot:nonroot

# Wrap the env vars into flags. distroless has no shell so we point
# straight at the binary; supply args explicitly via --command-style
# environment substitution at the orchestrator level if you need
# overrides.
ENTRYPOINT ["/input-bridge", \
            "--source", "ws", \
            "--ws-addr", "0.0.0.0:9100", \
            "--cdp-url", "http://127.0.0.1:9222", \
            "--metrics-addr", "0.0.0.0:9101"]

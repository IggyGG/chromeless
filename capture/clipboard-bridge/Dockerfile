# syntax=docker/dockerfile:1.7
#
# clipboard-bridge — bidirectional v1 clipboard sync over CDP.
# See capture/clipboard-bridge/README.md.

# ---------- build ----------
FROM golang:1.22-alpine AS build
WORKDIR /src

COPY go.mod go.sum* ./
RUN go mod download

COPY . .
ENV CGO_ENABLED=0 GOOS=linux
RUN go build -trimpath -ldflags="-s -w" -o /out/clipboard-bridge .
RUN go test ./...

# ---------- runtime ----------
FROM gcr.io/distroless/static-debian12:nonroot
WORKDIR /
COPY --from=build /out/clipboard-bridge /clipboard-bridge

EXPOSE 9300

ENTRYPOINT ["/clipboard-bridge", \
            "--source", "ws", \
            "--ws-addr", "0.0.0.0:9300", \
            "--ws-path", "/clipboard", \
            "--cdp-url", "http://127.0.0.1:9222", \
            "--sink", "ws", \
            "--sink-url", "ws://127.0.0.1:9301/clipboard"]

USER nonroot:nonroot

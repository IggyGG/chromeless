# syntax=docker/dockerfile:1.7
# infra/controllers/browser-session-controller/Dockerfile
#
# Multi-stage. golang-alpine builder produces a static binary; runtime
# is distroless/static (no shell, no libc, no package manager). Runs
# as nonroot (uid 65532) per distroless's :nonroot tag convention.

# ---------- build ----------
FROM golang:1.23-alpine AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download

COPY cmd/    ./cmd/
COPY pkg/    ./pkg/

ENV CGO_ENABLED=0 GOOS=linux
RUN go build \
        -trimpath -ldflags="-s -w" \
        -o /out/browser-session-controller \
        ./cmd/controller

# ---------- runtime ----------
FROM gcr.io/distroless/static-debian12:nonroot
WORKDIR /
COPY --from=build /out/browser-session-controller /browser-session-controller

# /metrics on 8080, /healthz on 8081 — match cmd/controller flags.
EXPOSE 8080 8081
USER nonroot:nonroot
ENTRYPOINT ["/browser-session-controller"]

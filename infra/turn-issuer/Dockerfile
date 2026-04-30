# syntax=docker/dockerfile:1.7
# infra/turn-issuer/Dockerfile (T76)
#
# Multi-stage. golang-alpine builder produces a static binary;
# distroless/static-debian12:nonroot is the runtime.

# ---------- build ----------
FROM golang:1.22-alpine AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY *.go ./
ENV CGO_ENABLED=0 GOOS=linux
RUN go build \
        -trimpath -ldflags="-s -w" \
        -o /out/turn-issuer .

# ---------- runtime ----------
FROM gcr.io/distroless/static-debian12:nonroot
WORKDIR /
COPY --from=build /out/turn-issuer /turn-issuer

# /issue-turn-cred + /metrics + /healthz on :8090 (LISTEN env override).
EXPOSE 8090
USER nonroot:nonroot
ENTRYPOINT ["/turn-issuer"]

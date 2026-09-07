# syntax=docker/dockerfile:1.7
# infra/gateway/Dockerfile — the standalone gateway.
#
# Multi-stage, mirroring infra/turn-issuer/Dockerfile: an alpine builder
# produces a static binary, distroless/static-debian12:nonroot runs it.
#
# BUILD CONTEXT IS THE REPO ROOT, not this directory — see the compose
# service, which sets `context: ..`. Two things here need it: the token
# package is shared with signaling/ via a `replace` directive that reaches
# outside this module, and the client bundle is built from client/.
# Building with this directory as the context fails on both.

# ---------- client bundle ----------
# Built here rather than COPYing client/dist, because client/dist is
# gitignored (.gitignore:6): on a fresh clone it does not exist, so a COPY
# would fail — or, worse, silently ship a stale bundle from whenever the
# operator last ran `npm run build` on the host.
FROM node:20-alpine AS client
WORKDIR /client
COPY client/package.json client/package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY client/ ./
RUN npm run build

# ---------- build ----------
FROM golang:1.22-alpine AS build
WORKDIR /src

# The replace directive points at ../../signaling, so that module's files must
# be present before `go mod download` can resolve anything.
COPY signaling/go.mod signaling/go.sum ./signaling/
COPY infra/gateway/go.mod infra/gateway/go.sum* ./infra/gateway/
WORKDIR /src/infra/gateway
RUN go mod download

WORKDIR /src
COPY signaling/ ./signaling/
COPY infra/gateway/ ./infra/gateway/

WORKDIR /src/infra/gateway
ENV CGO_ENABLED=0 GOOS=linux
RUN go build -trimpath -ldflags="-s -w" -o /out/gateway .

# An empty directory to seed /data/certs with. distroless has no shell and no
# mkdir, so the runtime stage cannot create it — and it MUST exist in the image
# with the right owner. See the COPY in the runtime stage for why.
#
# The .keep file is load-bearing. Docker's LEGACY builder (a daemon without
# buildx) drops empty directories when it COPYs from a stage, so on those hosts
# an empty /out/certs never reaches the runtime image, the volume mountpoint
# comes back root-owned, and the first-boot failure this guards against returns
# silently. A "clean this up" that removes the file reverts the fix. The gateway
# opens cert.pem and key.pem by name and never lists the directory, so the file
# is inert at runtime.
RUN mkdir -p /out/certs && chmod 700 /out/certs && touch /out/certs/.keep

# ---------- runtime ----------
FROM gcr.io/distroless/static-debian12:nonroot
WORKDIR /

COPY --from=build /out/gateway /gateway

# The client bundle, from the stage above. Baked in so the published image is
# self-contained; compose can still mount client/dist over this path for live
# reload during development.
COPY --from=client --chown=nonroot:nonroot /client/dist/ /srv/client/

# /data/certs must exist IN THE IMAGE, owned by nonroot, or the gateway dies
# ~1s after start with no useful symptom.
#
# compose mounts the named volume `gateway_certs` here. Docker seeds a fresh
# named volume from the image's content at that path — INCLUDING its ownership.
# When the path does not exist in the image, docker creates the mountpoint
# itself, root-owned 0755. The gateway runs as nonroot (uid 65532), so:
#
#   os.MkdirAll("/data/certs", 0700)   -> succeeds, it already exists (no-op)
#   os.WriteFile("/data/certs/key.pem") -> EACCES
#
# loadOrCreateCert then returns an error and main os.Exit(2)s. Measured on
# 2026-08-19 (task 340081): `Container cbwrtc-gateway Started` at 11:57:00.703,
# `container cbwrtc-gateway is unhealthy` at 11:57:01.573 — 870ms, with
# start_period 5s, so NO healthcheck had run. Compose reports the dead
# container as "unhealthy", which reads as a failing probe rather than a
# process that exited before the first one.
#
# Reproduced and the fix verified on a real daemon (docker 27.5.1): a fresh
# named volume over a root-owned mountpoint gives WRITE-DENIED to uid 65532;
# with this COPY it gives WRITE-OK.
COPY --from=build --chown=nonroot:nonroot /out/certs /data/certs

# TLS terminates here. This is the only port a standalone deployment
# publishes: the signaling broker and Chromium's DevTools stay internal.
EXPOSE 8443
USER nonroot:nonroot
ENTRYPOINT ["/gateway"]

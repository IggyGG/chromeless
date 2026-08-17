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

# ---------- runtime ----------
FROM gcr.io/distroless/static-debian12:nonroot
WORKDIR /

COPY --from=build /out/gateway /gateway

# The client bundle, from the stage above. Baked in so the published image is
# self-contained; compose can still mount client/dist over this path for live
# reload during development.
COPY --from=client --chown=nonroot:nonroot /client/dist/ /srv/client/

# TLS terminates here. This is the only port a standalone deployment
# publishes: the signaling broker and Chromium's DevTools stay internal.
EXPOSE 8443
USER nonroot:nonroot
ENTRYPOINT ["/gateway"]

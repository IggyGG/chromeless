#!/bin/sh
# infra/devtools-proxy.sh — socat bridge for Chromium DevTools (T52).
#
# Why this exists
# ---------------
# Chromium 147+ ignores `--remote-debugging-address=0.0.0.0` and binds
# DevTools to 127.0.0.1:9222 only (security hardening on the upstream
# side). Result: a container that exposes 9222 via `docker run -p 9222:9222`
# or a K8s containerPort cannot reach DevTools from outside, because
# Docker NATs host:9222 → container_eth0:9222 and there's no listener
# on eth0.
#
# Fix: run socat in the same container, listening on the container's
# eth0 IP at 9222 and forwarding to 127.0.0.1:9222 where Chromium
# actually binds. We bind to a *specific* IP rather than 0.0.0.0 so
# we don't conflict with Chromium's loopback bind (0.0.0.0:9222 would
# overlap 127.0.0.1:9222; per-IP binds don't).
#
# Chromium's advertised webSocketDebuggerUrl still says
# `ws://127.0.0.1:9222/...`. From the host that resolves correctly
# because the docker port mapping forwards host:9222 → eth0:9222 →
# (this proxy) → 127.0.0.1:9222 → Chromium.
#
# Internal probes (cb-metrics-sidecar, idle-watchdog) keep dialing
# 127.0.0.1:9222 directly and bypass this proxy — one less moving
# part in the hot path.
#
# This is the pragmatic stop-gap. The Chromium-recommended path for
# non-localhost DevTools is `--remote-debugging-pipe`, which would
# require a Go/C++ pipe<->TCP wrapper. Phase 3 follow-up if we ever
# need to remove the small surface area socat adds.

set -eu

# `hostname -i` may print multiple IPs (IPv4 + IPv6); take the first
# IPv4. busybox's hostname doesn't accept --ip-address, so use awk.
POD_IP=$(hostname -i 2>/dev/null | awk '{print $1}')
if [ -z "$POD_IP" ]; then
    echo "[devtools-proxy] FATAL: could not resolve container IP via 'hostname -i'" >&2
    exit 2
fi

echo "[devtools-proxy] forwarding ${POD_IP}:9222 -> 127.0.0.1:9222" >&2

# fork: one socat child per accepted connection; the listener stays up.
# reuseaddr: don't get stuck in TIME_WAIT after a restart.
exec /usr/bin/socat \
    "TCP-LISTEN:9222,bind=${POD_IP},fork,reuseaddr" \
    "TCP:127.0.0.1:9222"

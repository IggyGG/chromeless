#!/usr/bin/env bash
#
# tests/smoke/container-boot.sh — T9.
#
# Boot the chromeless container, drive Chromium via DevTools
# Protocol to navigate a real URL, capture a screenshot, and assert it's
# a valid PNG. Designed to catch real regressions: a launcher that
# crashes, a Dockerfile that builds but doesn't actually start Chromium,
# an Xvfb misconfiguration that leaves Chromium blocked on display init,
# and so on.
#
# All CDP traffic happens INSIDE the container via `docker exec`. We
# don't rely on host port-forwarding for two reasons:
#   1. As of Chromium 147, --remote-debugging-address=0.0.0.0 is
#      effectively ignored — DevTools binds 127.0.0.1 only. A `-p
#      9222:9222` on docker run therefore can't reach DevTools. Running
#      from inside the container's net namespace bypasses this entirely
#      and isolates the smoke from "is host networking right" concerns.
#      (Filed as a follow-up; once launch-chromeless.sh is fixed, this
#      smoke continues to pass unchanged.)
#   2. Avoiding host pip installs keeps the smoke self-contained and
#      compatible with any host that has Docker + curl + python3, which
#      is the entire CI matrix already.
#
# Layered checks (each step fails loud and identifies which step failed):
#   1. Resolve the image (pull it if a tag was named but isn't local).
#   2. Start the container detached; poll DevTools (inside) up to 60 s.
#   3. Assert /json/version returns HTTP 200 and a Browser identifier.
#   4. Discover the first page target and its webSocketDebuggerUrl.
#   5. Drive CDP: Page.navigate -> wait for document.readyState=complete.
#   6. Page.captureScreenshot (PNG) and base64-decode to disk inside the
#      container; `docker cp` it back out to the host path.
#   7. Assert PNG file magic and size > 5 KiB.
#
# Usage:
#   bash tests/smoke/container-boot.sh
#
# Knobs (env):
#   SMOKE_IMAGE_TAG        Docker image tag to test. Pulled if not present
#                          locally. Default: auto-detect (prefers
#                          chromeless:ci, then chromeless:dev). There is no
#                          build fallback — see resolve_or_pull_image.
#   SMOKE_NAVIGATE_URL     URL Chromium navigates to. Default:
#                          https://example.com. Override to an offline
#                          target if the runner has no public outbound
#                          (e.g. data:text/html;base64,...).
#   SMOKE_SCREENSHOT_PATH  Where to write the screenshot. Default:
#                          /tmp/chromeless-smoke.png.
#   SMOKE_READY_TIMEOUT_S  How long to wait for DevTools. Default: 60.
#   SMOKE_LOAD_TIMEOUT_S   How long to wait for navigation to complete.
#                          Default: 30.
#
# Coordination notes (per infra-dev / T31):
#   - We run with IDLE_TIMEOUT_S=999999 so the lifecycle idle-watchdog
#     can't race the smoke and shut the container down mid-test. Only the
#     Phase-1 lineage has that program at all — see step 3.
#   - cold-start.sh wipes /home/cbuser/.config/chromium each boot, so
#     this smoke gets a clean profile on every run with no extra effort.

set -euo pipefail

# ---------- config & helpers ---------------------------------------------

SMOKE_IMAGE_TAG="${SMOKE_IMAGE_TAG:-}"
SMOKE_NAVIGATE_URL="${SMOKE_NAVIGATE_URL:-https://example.com}"
SMOKE_SCREENSHOT_PATH="${SMOKE_SCREENSHOT_PATH:-/tmp/chromeless-smoke.png}"
SMOKE_READY_TIMEOUT_S="${SMOKE_READY_TIMEOUT_S:-60}"
SMOKE_LOAD_TIMEOUT_S="${SMOKE_LOAD_TIMEOUT_S:-30}"

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CONTAINER_NAME="chromeless-smoke-$$"
CONTAINER_ID=""
SCREENSHOT_IN_CONTAINER="/tmp/chromeless-smoke.png"

step()  { printf '\n\033[1m== %s ==\033[0m\n' "$*" >&2; }
log()   { printf '[smoke] %s\n' "$*" >&2; }
fail()  {
    printf '\033[31m[smoke] FAIL: %s\033[0m\n' "$*" >&2
    if [ -n "$CONTAINER_ID" ]; then
        printf '\033[31m[smoke] last 80 lines of container logs:\033[0m\n' >&2
        docker logs --tail 80 "$CONTAINER_ID" 2>&1 | sed 's/^/  /' >&2 || true
    fi
    exit 1
}

cleanup() {
    if [ -n "$CONTAINER_ID" ]; then
        log "stopping container $CONTAINER_ID..."
        docker stop -t 5 "$CONTAINER_ID" >/dev/null 2>&1 || true
        # We started with --rm, so stop should remove. Belt-and-braces:
        docker rm -f "$CONTAINER_ID" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

require() {
    command -v "$1" >/dev/null 2>&1 || fail "required command not found: $1"
}

# Hard prerequisites on the host.
require docker
require python3

# ---------- 1. resolve image tag (build if necessary) -------------------

# OSS-W1: there is no build fallback any more. Every branch below used to
# `docker build -f infra/Dockerfile .` — the legacy stock-Chromium image,
# deleted when the M7 native-peer migration removed capture/streamer-page/.
# The dead branch turned "image absent" into the maximally-misleading
#   ERROR: resolve : lstat infra: no such file or directory
#   [smoke] FAIL: docker build failed
# which is what a pruned image looks like on a shared CI daemon: observed
# 2026-07-30 on forgejo-runner-7, where dind-gc's high-water
# `docker image prune -af --filter until=30m` had evicted the image the CI
# job pulled minutes earlier. So: pull when a tag is named (the image comes
# from a registry, never from here), and fail with the real reason otherwise.
resolve_or_pull_image() {
    if [ -n "$SMOKE_IMAGE_TAG" ]; then
        if docker image inspect "$SMOKE_IMAGE_TAG" >/dev/null 2>&1; then
            log "using existing image: $SMOKE_IMAGE_TAG"
            return
        fi
        log "image $SMOKE_IMAGE_TAG not present locally; pulling"
        docker pull "$SMOKE_IMAGE_TAG" >/dev/null \
            || fail "image $SMOKE_IMAGE_TAG is neither present locally nor pullable (registry auth? tag typo?)"
        return
    fi

    # Auto-detect order: :ci (CI fast path) -> :dev (local convention).
    if docker image inspect "chromeless:ci" >/dev/null 2>&1; then
        SMOKE_IMAGE_TAG="chromeless:ci"
        log "auto-detected image: $SMOKE_IMAGE_TAG"
    elif docker image inspect "chromeless:dev" >/dev/null 2>&1; then
        SMOKE_IMAGE_TAG="chromeless:dev"
        log "auto-detected image: $SMOKE_IMAGE_TAG"
    else
        fail "no image to test: set SMOKE_IMAGE_TAG to a cloud_browser_worker image (this repo cannot build one — see build/chromeless-build.sh)"
    fi
}

step "1. resolve image"
resolve_or_pull_image

# ---------- 2. start container & wait for DevTools (in container) ------

step "2. start container"
CONTAINER_ID=$(docker run --rm -d \
    --name "$CONTAINER_NAME" \
    -e IDLE_TIMEOUT_S=999999 \
    --shm-size=1g \
    "$SMOKE_IMAGE_TAG") \
    || fail "docker run failed"
log "container started: $CONTAINER_ID"

log "polling DevTools (inside container) for up to ${SMOKE_READY_TIMEOUT_S}s..."
ready=0
for i in $(seq 1 "$SMOKE_READY_TIMEOUT_S"); do
    if docker exec "$CONTAINER_ID" curl -fsS \
            http://127.0.0.1:9222/json/version >/dev/null 2>&1
    then
        ready=1
        log "DevTools is up after ${i}s"
        break
    fi
    # If the container died early, fail fast — no point polling for 60s.
    if ! docker inspect -f '{{.State.Running}}' "$CONTAINER_ID" 2>/dev/null \
        | grep -q true
    then
        fail "container exited before DevTools came up"
    fi
    sleep 1
done
[ "$ready" = "1" ] || fail "DevTools did not respond within ${SMOKE_READY_TIMEOUT_S}s"

# ---------- 3. watchdog env propagation --------------------------------
#
# Phase-1 images run [program:idle-watchdog] under supervisord, and this
# step asserts the IDLE_TIMEOUT_S we passed on docker run actually reached
# it. M7 images (build/Dockerfile.runtime + infra/supervisord.phase2.conf)
# deliberately ship NO idle-watchdog — the native peer owns lifecycle — so
# there is nothing to assert. The first CI run against a real cr7727-*
# image (2026-07-30) failed exactly here: 20 s polling for a log file that
# never exists in that lineage. Probe the baked conf for the program and
# skip honestly when the image doesn't carry it, so the smoke stays valid
# for both lineages.

step "3. watchdog env"
if docker exec "$CONTAINER_ID" grep -q '^\[program:idle-watchdog\]' \
        /etc/supervisor/supervisord.conf 2>/dev/null; then
    WATCHDOG_LOG=""
    for i in $(seq 1 20); do
        WATCHDOG_LOG=$(docker exec "$CONTAINER_ID" sh -c \
            'cat /var/log/supervisor/idle-watchdog.log 2>/dev/null || true') \
            || fail "reading idle-watchdog log failed"
        if printf '%s\n' "$WATCHDOG_LOG" | grep -q 'idle_timeout=999999s'; then
            log "idle-watchdog inherited IDLE_TIMEOUT_S=999999"
            break
        fi
        sleep 1
    done
    printf '%s\n' "$WATCHDOG_LOG" | grep -q 'idle_timeout=999999s' \
        || fail "idle-watchdog did not inherit IDLE_TIMEOUT_S=999999"
else
    log "image has no [program:idle-watchdog] (M7 native-peer lineage); skipping watchdog-env assertion"
fi

# ---------- 4. /json/version returns Chromium ---------------------------

step "4. /json/version"
VERSION_JSON=$(docker exec "$CONTAINER_ID" curl -fsS \
    http://127.0.0.1:9222/json/version) \
    || fail "curl /json/version failed (HTTP non-200)"

python3 -c '
import json, sys
data = json.loads(sys.stdin.read())
if "Browser" not in data:
    print("missing Browser key in /json/version response", file=sys.stderr)
    sys.exit(1)
print("  Browser:          " + str(data["Browser"]), file=sys.stderr)
print("  Protocol-Version: " + str(data.get("Protocol-Version", "?")), file=sys.stderr)
' <<<"$VERSION_JSON" || fail "/json/version response did not contain Browser key"

# ---------- 5. discover page target webSocketDebuggerUrl ---------------

step "5. discover page target"
TARGETS_JSON=$(docker exec "$CONTAINER_ID" curl -fsS \
    http://127.0.0.1:9222/json) \
    || fail "curl /json failed"

WS_URL=$(python3 -c '
import json, sys
targets = json.loads(sys.stdin.read())
pages = [t for t in targets
         if t.get("type") == "page" and t.get("webSocketDebuggerUrl")]
if not pages:
    print("no page targets with webSocketDebuggerUrl", file=sys.stderr)
    sys.exit(1)
print(pages[0]["webSocketDebuggerUrl"])
' <<<"$TARGETS_JSON") || fail "no page targets with webSocketDebuggerUrl"
log "page target: $WS_URL"

# ---------- 6+7. drive CDP inside the container ------------------------
#
# Bundled stdlib WebSocket client. Avoids any pip / apt dependency on the
# in-container python3, so the smoke runs against a fresh image with no
# network installs. The CDP wire is straightforward: text frames carrying
# JSON, no fragmentation in practice from chromium DevTools.

step "6. drive CDP (navigate + screenshot)"

docker exec -i "$CONTAINER_ID" python3 - \
    "$WS_URL" "$SMOKE_NAVIGATE_URL" "$SCREENSHOT_IN_CONTAINER" "$SMOKE_LOAD_TIMEOUT_S" \
    <<'PY' || fail "CDP smoke failed"
import base64
import json
import os
import secrets
import socket
import struct
import sys
import time
from urllib.parse import urlparse

ws_url, nav_url, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
load_timeout_s = int(sys.argv[4])

# ----- minimal RFC 6455 WebSocket client (text frames only) -----

class WS:
    def __init__(self, url: str, timeout: float = 10.0):
        u = urlparse(url)
        if u.scheme != "ws":
            raise SystemExit(f"unsupported ws scheme: {u.scheme}")
        host = u.hostname or "127.0.0.1"
        port = u.port or 80
        path = u.path + (f"?{u.query}" if u.query else "") or "/"
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        key = base64.b64encode(secrets.token_bytes(16)).decode("ascii")
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n"
            f"\r\n"
        ).encode("ascii")
        self.sock.sendall(req)
        # Read headers until CRLFCRLF.
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise SystemExit("ws: connection closed during handshake")
            buf += chunk
        head, _, leftover = buf.partition(b"\r\n\r\n")
        status = head.split(b"\r\n", 1)[0]
        if b" 101 " not in status:
            raise SystemExit(f"ws: handshake failed: {status.decode(errors='replace')}")
        self._buf = bytearray(leftover)

    def _read(self, n: int) -> bytes:
        while len(self._buf) < n:
            chunk = self.sock.recv(max(4096, n - len(self._buf)))
            if not chunk:
                raise SystemExit("ws: connection closed mid-frame")
            self._buf.extend(chunk)
        out = bytes(self._buf[:n])
        del self._buf[:n]
        return out

    def send_text(self, text: str) -> None:
        payload = text.encode("utf-8")
        mask = secrets.token_bytes(4)
        n = len(payload)
        if n < 126:
            header = struct.pack("!BB", 0x81, 0x80 | n)
        elif n < (1 << 16):
            header = struct.pack("!BBH", 0x81, 0x80 | 126, n)
        else:
            header = struct.pack("!BBQ", 0x81, 0x80 | 127, n)
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def recv_text(self) -> str:
        # Loop in case server sends a control frame (ping/close/pong) we
        # need to handle silently.
        while True:
            b1, b2 = self._read(2)
            fin = b1 & 0x80
            opcode = b1 & 0x0F
            masked = b2 & 0x80
            n = b2 & 0x7F
            if n == 126:
                (n,) = struct.unpack("!H", self._read(2))
            elif n == 127:
                (n,) = struct.unpack("!Q", self._read(8))
            if masked:
                # Server-to-client frames must NOT be masked per RFC.
                # Tolerate it anyway by unmasking.
                mask = self._read(4)
                payload = bytes(b ^ mask[i & 3]
                                for i, b in enumerate(self._read(n)))
            else:
                payload = self._read(n) if n else b""
            if not fin:
                # CDP doesn't fragment in practice; treat as fatal.
                raise SystemExit("ws: fragmented frame (unsupported)")
            if opcode == 0x1:  # text
                return payload.decode("utf-8")
            if opcode == 0x9:  # ping -> pong
                self._send_control(0xA, payload)
                continue
            if opcode in (0xA,):  # pong, ignore
                continue
            if opcode == 0x8:  # close
                raise SystemExit("ws: server closed connection")
            # Unknown opcode — tolerate.
            continue

    def _send_control(self, opcode: int, payload: bytes) -> None:
        mask = secrets.token_bytes(4)
        n = len(payload)
        if n >= 126:
            raise SystemExit("ws: control payload too large")
        header = struct.pack("!BB", 0x80 | opcode, 0x80 | n)
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def close(self) -> None:
        try:
            self._send_control(0x8, b"")
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass


# ----- CDP convenience -----

ws = WS(ws_url, timeout=10)
seq = 0


def call(method: str, params=None) -> dict:
    global seq
    seq += 1
    rid = seq
    ws.send_text(json.dumps({"id": rid, "method": method, "params": params or {}}))
    while True:
        msg = json.loads(ws.recv_text())
        if msg.get("id") == rid:
            if "error" in msg:
                raise SystemExit(f"CDP error on {method}: {msg['error']}")
            return msg.get("result", {})
        # Otherwise this is an unsolicited event we don't care about; loop.


print("[cdp] Page.enable", file=sys.stderr)
call("Page.enable")

print(f"[cdp] Page.navigate {nav_url}", file=sys.stderr)
call("Page.navigate", {"url": nav_url})

# Poll readyState until "complete" or timeout. Cheap and robust against
# missed lifecycle events.
deadline = time.time() + load_timeout_s
last_state = "?"
while time.time() < deadline:
    res = call("Runtime.evaluate", {
        "expression": "document.readyState",
        "returnByValue": True,
    })
    last_state = (res.get("result") or {}).get("value")
    if last_state == "complete":
        break
    time.sleep(0.5)
else:
    raise SystemExit(f"document.readyState never reached 'complete' (last={last_state!r})")

print(f"[cdp] readyState={last_state}", file=sys.stderr)

# Give first paint a moment to settle. Page.captureScreenshot would
# trigger a fresh paint anyway, but a small sleep also lets late inline
# images / CSS apply.
time.sleep(2)

print("[cdp] Page.captureScreenshot", file=sys.stderr)
shot = call("Page.captureScreenshot", {"format": "png"})
data = shot.get("data")
if not data:
    raise SystemExit("Page.captureScreenshot returned no data")

raw = base64.b64decode(data)
with open(out_path, "wb") as f:
    f.write(raw)

print(f"[cdp] wrote {out_path} ({len(raw)} bytes inside container)",
      file=sys.stderr)

ws.close()
PY

# Pull the screenshot out of the container.
docker cp "$CONTAINER_ID:$SCREENSHOT_IN_CONTAINER" "$SMOKE_SCREENSHOT_PATH" \
    || fail "docker cp screenshot from container failed"

# ---------- 7. validate screenshot --------------------------------------

step "7. validate screenshot"
[ -f "$SMOKE_SCREENSHOT_PATH" ] || fail "$SMOKE_SCREENSHOT_PATH does not exist"

SIZE=$(wc -c <"$SMOKE_SCREENSHOT_PATH" | tr -d ' ')
if [ "$SIZE" -lt 5120 ]; then
    fail "$SMOKE_SCREENSHOT_PATH is too small ($SIZE bytes; need > 5120)"
fi

# PNG file magic: 89 50 4E 47 0D 0A 1A 0A
if command -v xxd >/dev/null 2>&1; then
    MAGIC=$(head -c 8 "$SMOKE_SCREENSHOT_PATH" | xxd -p)
else
    MAGIC=$(head -c 8 "$SMOKE_SCREENSHOT_PATH" | od -A n -v -t x1 | tr -d ' \n')
fi
if [ "$MAGIC" != "89504e470d0a1a0a" ]; then
    fail "$SMOKE_SCREENSHOT_PATH is not a valid PNG (magic=$MAGIC)"
fi

log "screenshot OK: $SMOKE_SCREENSHOT_PATH ($SIZE bytes, valid PNG magic)"

# ---------- done --------------------------------------------------------

step "smoke passed"
echo "[smoke] container-boot: PASS" >&2
exit 0

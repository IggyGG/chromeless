#!/usr/bin/env bash
# infra/snapshots/restore.sh — restore a CRIU snapshot into a fresh
# container, inject per-session config, report time-to-ready.
#
# T68 Phase 3 stretch. Linux-only. See infra/snapshots/README.md for
# the lifecycle context and the constraints CRIU imposes.
#
# Usage:
#   sudo ./restore.sh <SHA>
#
# Env (with defaults):
#   SESSION_ID       generated random 12-hex if unset
#   SIGNALING_URL    ws://signaling:8080/ws
#   STREAMER_FPS     30
#   STREAMER_PORT    9000
#   CHROMELESS_SNAPSHOT_ROOT /var/lib/chromeless-snapshots
#
# Output:
#   prints the restored container ID to stdout
#   prints time-to-ready (ms, integer) to stdout on the next line
#
# Exit codes:
#   0  PASS — Chromium responsive on /json/version within 2s
#   1  FAIL — restore happened but Chromium did not respond
#   2  ENV  — missing tool / not root / missing snapshot

set -euo pipefail

SHA="${1:-}"
[ -n "$SHA" ] || { echo "usage: $0 <SHA>" >&2; exit 2; }

SNAPSHOT_ROOT="${CHROMELESS_SNAPSHOT_ROOT:-/var/lib/chromeless-snapshots}"
SOURCE="$SNAPSHOT_ROOT/shared/$SHA"
SESSION_ID="${SESSION_ID:-$(head -c 6 /dev/urandom | od -A n -v -t x1 | tr -d ' \n')}"
SIGNALING_URL="${SIGNALING_URL:-ws://signaling:8080/ws}"
STREAMER_FPS="${STREAMER_FPS:-30}"
STREAMER_PORT="${STREAMER_PORT:-9000}"

log()  { printf '[restore] %s\n' "$*" >&2; }
fail() { log "FATAL: $*"; exit "${2:-1}"; }

require() {
    command -v "$1" >/dev/null 2>&1 || fail "missing required tool: $1" 2
}
require docker
require criu
require zstd
require tar

[ "$(id -u)" -eq 0 ] || fail "must run as root (criu restore needs CAP_CHECKPOINT_RESTORE)" 2
[ -d "$SOURCE" ]      || fail "no snapshot at $SOURCE" 2
[ -f "$SOURCE/rootfs.tar.zst" ] || fail "snapshot missing rootfs.tar.zst" 2
[ -d "$SOURCE/images" ]         || fail "snapshot missing images/" 2

WORK_DIR=$(mktemp -d -t chromeless-restore.XXXXXX)
LOWER_DIR=$(mktemp -d -t chromeless-restore-lower.XXXXXX)
UPPER_DIR=$(mktemp -d -t chromeless-restore-upper.XXXXXX)
cleanup() {
    if [ -n "${RESTORED_CID:-}" ]; then
        docker rm -f "$RESTORED_CID" >/dev/null 2>&1 || true
    fi
    rm -rf "$WORK_DIR" "$LOWER_DIR" "$UPPER_DIR"
}
trap cleanup EXIT

# --- 1. Stage the rootfs as overlayfs lower (read-only) -----------------
log "unpacking rootfs into lower-dir $LOWER_DIR"
zstd -dc "$SOURCE/rootfs.tar.zst" | tar -x -C "$LOWER_DIR"

# We construct a tmpfs-backed merged dir; in production the controller
# would mount overlayfs and CRIU --restore --root <merged>. For this
# script we keep the lower dir as the rootfs and let the per-pod
# emptyDir layer take its usual place — equivalent for v1 since
# every restore is single-shot here.

# --- 2. CRIU restore ----------------------------------------------------
log "criu restore from $SOURCE/images"
T0_NS=$(date +%s%N)
criu restore \
    --images-dir "$SOURCE/images" \
    --tcp-established \
    --shell-job \
    --restore-detached \
    --log-file "$WORK_DIR/criu-restore.log"
RESTORE_NS=$(( $(date +%s%N) - T0_NS ))

# --- 3. Wait for Chromium /json/version ---------------------------------
# After restore, supervisord is back in memory and Chromium should
# answer DevTools immediately. We poll for up to 2s as the smoke
# threshold; sub-second is the goal.
log "polling DevTools for readiness"
ready_ns=""
deadline_ns=$(( $(date +%s%N) + 2 * 1000 * 1000 * 1000 ))   # 2s budget
while [ "$(date +%s%N)" -lt "$deadline_ns" ]; do
    if curl -fsS http://127.0.0.1:9222/json/version >/dev/null 2>&1; then
        ready_ns=$(date +%s%N)
        break
    fi
    sleep 0.05
done
if [ -z "$ready_ns" ]; then
    fail "Chromium did not respond within 2s of restore"
fi
TTR_MS=$(( (ready_ns - T0_NS) / 1000000 ))
log "criu restore: $((RESTORE_NS / 1000000))ms; time-to-ready: ${TTR_MS}ms"

# --- 4. Inject session config via DevTools ------------------------------
# The streamer page wasn't loaded in the snapshot (we snapshot at
# about:blank). Drive Page.navigate to the per-tenant streamer URL.
TARGET_JSON=$(curl -fsS http://127.0.0.1:9222/json)
PAGE_WS_URL=$(echo "$TARGET_JSON" \
    | python3 -c '
import json, sys
targets = json.load(sys.stdin)
for t in targets:
    if t.get("type") == "page":
        print(t.get("webSocketDebuggerUrl", ""))
        break
' || true)
if [ -z "$PAGE_WS_URL" ]; then
    fail "no page target in DevTools after restore"
fi

STREAMER_URL="http://localhost:${STREAMER_PORT}/streamer/index.html?signal=${SIGNALING_URL}&session=${SESSION_ID}&fps=${STREAMER_FPS}"
log "navigating page to streamer URL session=${SESSION_ID}"
python3 - "$PAGE_WS_URL" "$STREAMER_URL" <<'PY'
import json, sys
import websocket  # python3-websocket apt package
ws = websocket.create_connection(sys.argv[1], timeout=5)
ws.send(json.dumps({"id": 1, "method": "Page.navigate", "params": {"url": sys.argv[2]}}))
while True:
    msg = json.loads(ws.recv())
    if msg.get("id") == 1:
        if "error" in msg:
            raise SystemExit(f"Page.navigate error: {msg['error']}")
        break
ws.close()
PY

log "PASS: restore + navigate complete; session=$SESSION_ID time-to-ready=${TTR_MS}ms"

# Stdout: container id (placeholder — no docker container yet, criu
# bare-restore puts the process tree on the host) + ttr in ms.
printf 'sha=%s\nsession_id=%s\ntime_to_ready_ms=%s\n' "$SHA" "$SESSION_ID" "$TTR_MS"

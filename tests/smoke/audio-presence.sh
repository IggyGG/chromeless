#!/usr/bin/env bash
# tests/smoke/audio-presence.sh — assert outbound-rtp audio is being sent
# from the streamer page's RTCPeerConnection.
#
# Usage:
#   ./tests/smoke/audio-presence.sh [DEVTOOLS_URL]
# Default DEVTOOLS_URL: http://127.0.0.1:9222
#
# Dependencies (run inside CI or on the test host, not the container):
#   bash, curl, jq, python3, python3-pip
#   websocket-client (auto-installed via pip --user if missing)
#
# Contract: T23's streamer page exposes the active RTCPeerConnection as
# `window.pc` and has a window title containing "streamer" (case
# insensitive). This script will exit non-zero with a clear message
# until that contract is met — that's expected pre-T23.
#
# Exit codes:
#   0  PASS — at least one outbound-rtp audio stream with bytesSent > 0
#   1  FAIL — streamer reachable but audio path silent
#   2  ENV  — required tool / dependency missing
#   3  PRE  — streamer page not loaded (T23 prerequisite unmet)

set -euo pipefail

DEVTOOLS_URL="${1:-http://127.0.0.1:9222}"

log() { printf '[audio-smoke] %s\n' "$*" >&2; }
die() { local code=$1; shift; printf '[audio-smoke] %s\n' "$*" >&2; exit "$code"; }

require() {
  command -v "$1" >/dev/null 2>&1 || die 2 "missing required tool: $1"
}
require curl
require jq
require python3

# websocket-client is the only non-stdlib python dep. Install on demand
# so this script is drop-in usable in CI without a separate setup step.
if ! python3 -c "import websocket" 2>/dev/null; then
  log "installing websocket-client (pip --user)..."
  python3 -m pip install --quiet --user websocket-client \
    || die 2 "failed to install websocket-client"
fi

log "devtools=${DEVTOOLS_URL}"

TARGETS=$(curl -fsS --max-time 5 "${DEVTOOLS_URL}/json") \
  || die 3 "cannot reach DevTools at ${DEVTOOLS_URL}/json — is the container up?"

WS_URL=$(echo "$TARGETS" \
  | jq -r '.[]
            | select(.type=="page" and (.title | test("streamer"; "i")))
            | .webSocketDebuggerUrl' \
  | head -n1)

if [ -z "$WS_URL" ] || [ "$WS_URL" = "null" ]; then
  log "no streamer page found (looked for type=page with title matching /streamer/i)"
  log "available pages:"
  echo "$TARGETS" | jq -r '.[] | "  - type=\(.type)\ttitle=\(.title // "")\turl=\(.url)"' >&2 || true
  die 3 "streamer page not loaded — load T23's streamer page and retry"
fi

log "streamer ws=${WS_URL}"

python3 - "$WS_URL" <<'PY'
import json
import sys
import websocket  # provided by websocket-client

ws_url = sys.argv[1]
ws = websocket.create_connection(ws_url, timeout=10)

req_id = 0
def call(method, params=None):
    global req_id
    req_id += 1
    ws.send(json.dumps({"id": req_id, "method": method, "params": params or {}}))
    while True:
        msg = json.loads(ws.recv())
        if msg.get("id") == req_id:
            if "error" in msg:
                raise SystemExit(f"CDP error on {method}: {msg['error']}")
            return msg.get("result", {})

# T23 contract: window.pc is the active RTCPeerConnection.
expr = """
(async () => {
  if (typeof window.pc === 'undefined' || window.pc === null) {
    return {error: 'window.pc not found'};
  }
  if (typeof window.pc.getStats !== 'function') {
    return {error: 'window.pc is not an RTCPeerConnection'};
  }
  const stats = await window.pc.getStats();
  const out = [];
  stats.forEach(s => {
    if (s.type === 'outbound-rtp' && s.kind === 'audio') {
      out.push({
        bytesSent: s.bytesSent,
        packetsSent: s.packetsSent,
        ssrc: s.ssrc,
      });
    }
  });
  return out;
})()
"""

result = call("Runtime.evaluate", {
    "expression": expr,
    "awaitPromise": True,
    "returnByValue": True,
})

value = result.get("result", {}).get("value")

# T23 contract violation -> PRE (3), not FAIL (1)
if isinstance(value, dict) and value.get("error"):
    print(f"streamer-page contract violation: {value['error']}", file=sys.stderr)
    sys.exit(3)

if not isinstance(value, list) or not value:
    print("FAIL: no outbound-rtp audio streams", file=sys.stderr)
    sys.exit(1)

print(f"outbound-rtp audio: {json.dumps(value)}")

if not any((s.get("bytesSent") or 0) > 0 for s in value):
    print("FAIL: outbound-rtp audio has bytesSent=0", file=sys.stderr)
    sys.exit(1)

print("PASS")
PY

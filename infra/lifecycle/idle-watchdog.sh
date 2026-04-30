#!/usr/bin/env bash
# infra/lifecycle/idle-watchdog.sh — declare the container idle and shut
# it down gracefully when the streamer page's RTCPeerConnection has been
# disconnected for IDLE_TIMEOUT_S seconds.
#
# Run by [program:idle-watchdog] in supervisord, as root (so
# supervisorctl can talk to /run/supervisor.sock at chmod 0700).
#
# Activity signal is `window.pc.connectionState` on the T23 streamer
# page, queried via DevTools `Runtime.evaluate`. States {connected,
# connecting, new} count as active. {disconnected, failed, closed,
# null/undefined} count as idle.
#
# Phase 2 will replace this with a real signal from the signaling
# server (per-session "last activity" timestamp). See infra/lifecycle/
# README.md for the limitations of the current heuristic.
#
# Env (with defaults):
#   DEVTOOLS_URL       http://127.0.0.1:9222
#   IDLE_TIMEOUT_S     600   shut down after this much continuous idle
#   WATCHDOG_GRACE_S   60    grace before idle accounting starts
#   WATCHDOG_POLL_S    30    poll interval

set -eu

DEVTOOLS_URL="${DEVTOOLS_URL:-http://127.0.0.1:9222}"
IDLE_TIMEOUT_S="${IDLE_TIMEOUT_S:-600}"
WATCHDOG_GRACE_S="${WATCHDOG_GRACE_S:-60}"
WATCHDOG_POLL_S="${WATCHDOG_POLL_S:-30}"

# T53: websocket-client comes from the python3-websocket Debian package
# installed in infra/Dockerfile. We deliberately do NOT lazy-install
# via pip — that path failed because python3-pip isn't in the runtime
# image, and adding it would bloat the image and require network access
# at container start. Sanity-check that the import works; bail loudly
# if the package is missing so a broken image fails fast instead of
# entering a supervisord crash loop.
if ! /usr/bin/python3 -c "import websocket" 2>/dev/null; then
    echo "[idle-watchdog] FATAL: python3-websocket missing from image" >&2
    echo "[idle-watchdog]   add 'python3-websocket' to infra/Dockerfile apt-get install" >&2
    exit 2
fi

exec /usr/bin/python3 - "$DEVTOOLS_URL" "$IDLE_TIMEOUT_S" "$WATCHDOG_GRACE_S" "$WATCHDOG_POLL_S" <<'PY'
import json
import subprocess
import sys
import time
from urllib.error import URLError
from urllib.request import urlopen

import websocket  # provided by websocket-client

devtools_url   = sys.argv[1]
idle_timeout_s = int(sys.argv[2])
grace_s        = int(sys.argv[3])
poll_s         = int(sys.argv[4])

ACTIVE = {"connected", "connecting", "new"}

def now() -> float:
    return time.monotonic()

def call(ws, method, params=None, _state=[0]):
    _state[0] += 1
    rid = _state[0]
    ws.send(json.dumps({"id": rid, "method": method, "params": params or {}}))
    while True:
        msg = json.loads(ws.recv())
        if msg.get("id") == rid:
            if "error" in msg:
                raise RuntimeError(f"{method}: {msg['error']}")
            return msg.get("result", {})

def probe_state():
    """Return the streamer page's window.pc.connectionState, or None
    if the page or DevTools are unreachable."""
    try:
        with urlopen(f"{devtools_url}/json", timeout=5) as r:
            targets = json.load(r)
    except (URLError, OSError) as e:
        print(f"[idle-watchdog] devtools unreachable: {e}", flush=True)
        return None

    for t in targets:
        if t.get("type") != "page":
            continue
        title = (t.get("title") or "").lower()
        if "streamer" not in title:
            continue
        ws_url = t.get("webSocketDebuggerUrl")
        if not ws_url:
            continue
        try:
            ws = websocket.create_connection(ws_url, timeout=5)
        except Exception as e:
            print(f"[idle-watchdog] cdp connect failed: {e}", flush=True)
            return None
        try:
            expr = "(window.pc && window.pc.connectionState) || null"
            result = call(ws, "Runtime.evaluate", {
                "expression": expr,
                "returnByValue": True,
            })
            return result.get("result", {}).get("value")
        finally:
            try:
                ws.close()
            except Exception:
                pass
    return None

started = now()
last_active = started
print(f"[idle-watchdog] devtools={devtools_url} idle_timeout={idle_timeout_s}s "
      f"grace={grace_s}s poll={poll_s}s", flush=True)

while True:
    state = probe_state()
    t = now()
    if state in ACTIVE:
        last_active = t
        print(f"[idle-watchdog] active state={state}", flush=True)
    else:
        idle_for = t - last_active
        in_grace = (t - started) <= grace_s
        print(f"[idle-watchdog] idle state={state!r} idle_for={idle_for:.0f}s "
              f"grace={'yes' if in_grace else 'no'}", flush=True)
        if not in_grace and idle_for >= idle_timeout_s:
            print(f"[idle-watchdog] idle threshold breached ({idle_for:.0f}s >= "
                  f"{idle_timeout_s}s); calling supervisorctl shutdown",
                  flush=True)
            try:
                subprocess.run(
                    ["/usr/bin/supervisorctl",
                     "-c", "/etc/supervisor/supervisord.conf",
                     "shutdown"],
                    check=False, timeout=10,
                )
            except subprocess.TimeoutExpired:
                print("[idle-watchdog] supervisorctl shutdown timed out", flush=True)
            sys.exit(0)
    time.sleep(poll_s)
PY

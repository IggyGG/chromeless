#!/usr/bin/env bash
# infra/lifecycle/cold-start.sh — per-container-boot setup. Runs from the
# entrypoint chain BEFORE supervisord starts, as root (PID 1 inherits
# root from docker run / dumb-init).
#
# Responsibilities (T31):
#   1. Clear stale Chromium user-data-dir so each container session
#      starts from a fresh profile. Without this, a container that
#      tombstones and respawns under the same docker volume mount would
#      inherit cached pages, cookies, and Service Worker state from the
#      previous session.
#   2. Generate a SESSION_ID if the env didn't supply one. The id is
#      written to /run/cb-session/id (and, with other resolved env
#      values, to /run/cb-session/env) so the watchdog and supervisord
#      programs can see the same value.
#   3. Echo resolved values so `docker logs <container>` is greppable.

set -eu

USER_DATA_DIR="/home/cbuser/.config/chromium"
SESSION_DIR="/run/cb-session"
SESSION_ID_FILE="$SESSION_DIR/id"
SESSION_ENV_FILE="$SESSION_DIR/env"

echo "[cold-start] booting cloud-browser-webrtc container" >&2

# --- 1. stale profile cleanup ---------------------------------------------
if [ -d "$USER_DATA_DIR" ]; then
    echo "[cold-start] clearing stale user-data-dir at $USER_DATA_DIR" >&2
    rm -rf "$USER_DATA_DIR"
fi
mkdir -p "$USER_DATA_DIR"
chown -R cbuser:cbuser "$USER_DATA_DIR"

# --- 2. resolve session env -----------------------------------------------
mkdir -p "$SESSION_DIR"
chmod 0755 "$SESSION_DIR"

if [ -z "${SESSION_ID:-}" ]; then
    if command -v xxd >/dev/null 2>&1; then
        rand_hex=$(head -c 6 /dev/urandom | xxd -p)
    else
        rand_hex=$(head -c 6 /dev/urandom | od -A n -v -t x1 | tr -d ' \n')
    fi
    SESSION_ID="auto-${rand_hex}"
fi
SIGNALING_URL="${SIGNALING_URL:-ws://signaling:8080/ws}"
STREAMER_FPS="${STREAMER_FPS:-30}"
STREAMER_PORT="${STREAMER_PORT:-9000}"

# /run/cb-session/id: just the id, plain text. Easy to read from
# scripts that don't want to source a full env file.
printf '%s\n' "$SESSION_ID" > "$SESSION_ID_FILE"
chmod 0644 "$SESSION_ID_FILE"

# /run/cb-session/env: KEY=VALUE per line, sourceable by entrypoint.sh.
{
    printf 'SESSION_ID=%s\n'    "$SESSION_ID"
    printf 'SIGNALING_URL=%s\n' "$SIGNALING_URL"
    printf 'STREAMER_FPS=%s\n'  "$STREAMER_FPS"
    printf 'STREAMER_PORT=%s\n' "$STREAMER_PORT"
} > "$SESSION_ENV_FILE"
chmod 0644 "$SESSION_ENV_FILE"

# --- 3. echo for grepability ----------------------------------------------
echo "[cold-start] session_id=$SESSION_ID" >&2
echo "[cold-start] signaling_url=$SIGNALING_URL" >&2
echo "[cold-start] streamer_fps=$STREAMER_FPS streamer_port=$STREAMER_PORT" >&2
echo "[cold-start] complete; handing off to supervisord" >&2

#!/usr/bin/env bash
# infra/lifecycle/cold-start.sh — per-container-boot setup. Runs from the
# entrypoint chain BEFORE supervisord starts, as cbuser (T57: the whole
# entrypoint chain is non-root since we set USER cbuser in the
# Dockerfile).
#
# Responsibilities:
#   1. (T57) Re-create writable directories that may have been wiped by
#      K8s emptyDir overlays at /run, /var/log/supervisor, /home/cbuser.
#      In a plain `docker run` these are pre-created in the image fs by
#      the Dockerfile; mkdir -p is a no-op there.
#   2. (T31) Clear stale Chromium user-data-dir so each container
#      session starts from a fresh profile.
#   3. (T31) Generate a SESSION_ID if the env didn't supply one. The id
#      is written to /run/cb-session/id (and, with other resolved env
#      values, to /run/cb-session/env) so the watchdog and supervisord
#      programs can see the same value.
#   4. Echo resolved values so `docker logs <container>` is greppable.

set -eu

USER_DATA_DIR="/home/cbuser/.config/chromium"
SESSION_DIR="/run/cb-session"
SESSION_ID_FILE="$SESSION_DIR/id"
SESSION_ENV_FILE="$SESSION_DIR/env"

echo "[cold-start] booting cloud-browser-webrtc container" >&2

# --- 0. K8s emptyDir overlay safety net (T57) -----------------------------
# These dirs are pre-created in the image fs (see Dockerfile), but a K8s
# Pod with `readOnlyRootFilesystem: true` mounts emptyDir at /run,
# /var/log/supervisor, /home/cbuser. Those mounts arrive empty and
# overlay our chowned dirs — supervisord then can't bind its socket and
# pulse can't open its runtime dir. Re-create here, idempotently.
mkdir -p /run/supervisor \
         /run/user/1000/pulse \
         /run/cb-session \
         /var/log/supervisor \
         /home/cbuser/.config/chromium
chmod 0700 /run/user/1000

# --- 1. stale profile cleanup ---------------------------------------------
# We're cbuser; the dir is owned by cbuser; rm + mkdir works without
# the previous chown step.
if [ -d "$USER_DATA_DIR" ]; then
    echo "[cold-start] clearing stale user-data-dir at $USER_DATA_DIR" >&2
    rm -rf "$USER_DATA_DIR"
fi
mkdir -p "$USER_DATA_DIR"

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

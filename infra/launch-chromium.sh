#!/bin/sh
# infra/launch-chromium.sh — wrapper invoked by [program:chromium] in
# supervisord. Expands runtime envvars into the streamer URL, then execs
# Chromium with the full Phase 1 flag list.
#
# Splitting this out of supervisord.conf lets us:
#   - parameterise per-session values (SESSION_ID, SIGNALING_URL) without
#     a re-baked image,
#   - keep the Chromium command line in one shellcheck-friendly place,
#   - exec Chromium directly so it inherits supervisord's PID slot
#     (signals propagate, no extra wrapper process).
#
# Env (all optional, with the same defaults the streamer page assumes):
#   SESSION_ID      session identifier            (default: dev)
#   SIGNALING_URL   WS URL the streamer dials     (default: ws://signaling:8080/ws)
#   STREAMER_FPS    display capture target fps    (default: 30)
#   STREAMER_PORT   local static-server port      (default: 9000)
#
# Source of truth for the flag list: capture/streamer-page/launch.md.
# When you change flags here, update that file in the same commit.

set -eu

: "${SESSION_ID:=dev}"
: "${SIGNALING_URL:=ws://signaling:8080/ws}"
: "${STREAMER_FPS:=30}"
: "${STREAMER_PORT:=9000}"

STREAMER_ORIGIN="http://localhost:${STREAMER_PORT}"
STREAMER_URL="${STREAMER_ORIGIN}/streamer/index.html?signal=${SIGNALING_URL}&session=${SESSION_ID}&fps=${STREAMER_FPS}"

echo "[launch-chromium] session=${SESSION_ID} signaling=${SIGNALING_URL} fps=${STREAMER_FPS}" >&2
echo "[launch-chromium] url=${STREAMER_URL}" >&2

exec /usr/bin/chromium \
  --no-sandbox \
  --disable-dev-shm-usage \
  --display=:99 \
  --remote-debugging-port=9222 \
  --remote-debugging-address=0.0.0.0 \
  --remote-allow-origins=* \
  --user-data-dir=/home/cbuser/.config/chromium \
  --no-first-run \
  --no-default-browser-check \
  --disable-features=TranslateUI,MediaRouter \
  --window-size=1920,1080 \
  --window-position=0,0 \
  --autoplay-policy=no-user-gesture-required \
  --use-fake-ui-for-media-stream \
  --auto-select-desktop-capture-source="Entire screen" \
  --auto-accept-this-tab-capture \
  --enable-usermedia-screen-capturing \
  --unsafely-treat-insecure-origin-as-secure="${STREAMER_ORIGIN}" \
  --app="${STREAMER_URL}"

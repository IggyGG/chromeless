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
#   SESSION_ID            session identifier             (default: dev)
#   SIGNALING_URL         WS URL the streamer dials      (default: ws://signaling:8080/ws)
#   STREAMER_FPS          display capture target fps     (default: 30)
#   STREAMER_PORT         local static-server port       (default: 9000)
#   CBWRTC_USE_FAKE_MEDIA T86 unblock switch — when set
#                         to "1", appends
#                         --use-fake-device-for-media-stream so
#                         getUserMedia/getDisplayMedia returns
#                         Chromium's synthetic test pattern + tone
#                         instead of capturing the X11 display.
#                         Bypasses the NotReadableError that T78's
#                         X11+SwiftShader pin didn't fix; useful for
#                         T64/T65 testing while chromium-dev debugs
#                         the real getDisplayMedia path. **Do not
#                         set in production**: synthetic media defeats
#                         the cb_audio null-sink routing (T24).
#
# Source of truth for the flag list: capture/streamer-page/launch.md.
# When you change flags here, update that file in the same commit.

set -eu

: "${SESSION_ID:=dev}"
: "${SIGNALING_URL:=ws://signaling:8080/ws}"
: "${STREAMER_FPS:=30}"
: "${STREAMER_PORT:=9000}"
: "${CBWRTC_USE_FAKE_MEDIA:=}"

STREAMER_ORIGIN="http://localhost:${STREAMER_PORT}"
STREAMER_URL="${STREAMER_ORIGIN}/streamer/index.html?signal=${SIGNALING_URL}&session=${SESSION_ID}&fps=${STREAMER_FPS}"

echo "[launch-chromium] session=${SESSION_ID} signaling=${SIGNALING_URL} fps=${STREAMER_FPS}" >&2
echo "[launch-chromium] url=${STREAMER_URL}" >&2

# T86: optional --use-fake-device-for-media-stream gate. When the env
# var is "1", we add the flag at the end of argv so it overrides any
# earlier defaults. Logged at startup so docker-compose/k8s logs make
# the synthetic-media mode obvious — silent fakes are how production
# accidents happen.
fake_media_arg=""
if [ "${CBWRTC_USE_FAKE_MEDIA}" = "1" ]; then
    fake_media_arg="--use-fake-device-for-media-stream"
    echo "[launch-chromium] WARNING: CBWRTC_USE_FAKE_MEDIA=1 -> appending ${fake_media_arg}" >&2
    echo "[launch-chromium] WARNING: synthetic media is for T86 testing only; do not ship this way" >&2
fi

# T86 direction #1: --auto-select-tab-capture-source-by-title is a
# sibling to --auto-select-desktop-capture-source="Entire screen".
# Some Chromium 147 picker code paths now prefer tab capture over
# desktop; the sibling flag matches the streamer page's <title>
# ("cloud-browser streamer", per capture/streamer-page/index.html).
# Belt-and-braces; harmless if the desktop-capture path is the one
# Chromium picks.
#
# shellcheck disable=SC2086  # fake_media_arg is intentionally word-split
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
  --disable-features=TranslateUI,MediaRouter,Vulkan,VaapiVideoDecodeLinuxGL \
  --ozone-platform=x11 \
  --use-gl=angle \
  --use-angle=swiftshader-webgl \
  --disable-gpu-vsync \
  --window-size=1920,1080 \
  --window-position=0,0 \
  --autoplay-policy=no-user-gesture-required \
  --use-fake-ui-for-media-stream \
  --auto-select-desktop-capture-source="Entire screen" \
  --auto-select-tab-capture-source-by-title="cloud-browser streamer" \
  --auto-accept-this-tab-capture \
  --enable-usermedia-screen-capturing \
  --unsafely-treat-insecure-origin-as-secure="${STREAMER_ORIGIN}" \
  ${fake_media_arg} \
  --app="${STREAMER_URL}"

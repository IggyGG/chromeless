#!/bin/sh
# infra/launch-chromeless.sh — wrapper invoked by [program:chromium] in
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
#   SIGNALING_TOKEN       optional browser-side JWT      (default: empty)
#   STREAMER_FPS          display capture target fps     (default: 30)
#   STREAMER_PORT         local static-server port       (default: 9000)
#   STREAMER_INPUT_URL    ws endpoint for input relay    (default: ws://localhost:9200/input)
#   STREAMER_METRICS_URL  stats endpoint                 (default: http://localhost:9100/stats-update)
#   STREAMER_WEBRTC_METRICS_URL event endpoint           (default: http://localhost:9100/webrtc-event)
#   CHROMELESS_BROWSER_BIN browser executable            (default: /usr/local/bin/chromeless when present,
#                                                          otherwise /usr/bin/chromium)
#   CHROMELESS_USE_FAKE_MEDIA T86 unblock switch — when set
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

# T90: re-source the per-session env on every chromium start so that
# `supervisorctl restart chromium` after a ScrubAndReturn cycle
# picks up a fresh SESSION_ID without rebuilding the pod. cold-start.sh
# writes this file at container boot; scrub-pod.sh deletes it (and a
# follow-up writer recreates it with new values) before the restart.
if [ -f /run/chromeless-session/env ]; then
    set -a
    # shellcheck disable=SC1091
    . /run/chromeless-session/env
    set +a
fi

: "${SESSION_ID:=dev}"
: "${SIGNALING_URL:=ws://signaling:8080/ws}"
: "${SIGNALING_TOKEN:=}"
: "${STREAMER_FPS:=30}"
: "${STREAMER_PORT:=9000}"
: "${STREAMER_INPUT_URL:=ws://localhost:9200/input}"
: "${STREAMER_METRICS_URL:=http://localhost:9100/stats-update}"
: "${STREAMER_WEBRTC_METRICS_URL:=http://localhost:9100/webrtc-event}"
: "${STREAMER_CDP_URL:=/cdp}"
: "${CHROMELESS_USE_FAKE_MEDIA:=}"
# T109: pre-recorded harness fixture for real T65 numbers. When set,
# Chromium's synthetic camera reads frames from this y4m file instead
# of generating the moving green square. Critically, the y4m IS a
# recording of the harness page's flashing-block + QR output, so
# reconcile.py sees real per-frame QR codes (the synthetic-media
# moving-green-square path doesn't carry QRs and produces zero
# decode rate per qa-tester's T65 finding).
#
# The path is the in-container location of the y4m. The compose.yaml
# bind-mount points /opt/chromeless-fixtures/ at harness/latency/fixtures/
# read-only; the same path works in K8s via a hostPath or configMap
# mount. Generate the y4m via harness/latency/record-y4m.sh — it's
# NOT checked in (~150 MiB at 720p × 30s would push git-lfs).
: "${CHROMELESS_USE_FAKE_MEDIA_FILE:=}"

STREAMER_ORIGIN="http://localhost:${STREAMER_PORT}"
STREAMER_URL="${STREAMER_ORIGIN}/streamer/index.html?signal=${SIGNALING_URL}&session=${SESSION_ID}&fps=${STREAMER_FPS}&input=${STREAMER_INPUT_URL}&metrics=${STREAMER_METRICS_URL}&webrtc_metrics=${STREAMER_WEBRTC_METRICS_URL}&cdp=${STREAMER_CDP_URL}"
if [ -n "${SIGNALING_TOKEN}" ]; then
    STREAMER_URL="${STREAMER_URL}&token=${SIGNALING_TOKEN}"
fi

if [ -z "${CHROMELESS_BROWSER_BIN:-}" ]; then
    if [ -x /usr/local/bin/chromeless ]; then
        CHROMELESS_BROWSER_BIN=/usr/local/bin/chromeless
    else
        CHROMELESS_BROWSER_BIN=/usr/bin/chromium
    fi
fi

echo "[launch-chromium] session=${SESSION_ID} signaling=${SIGNALING_URL} fps=${STREAMER_FPS}" >&2
LOG_STREAMER_URL="${STREAMER_URL}"
if [ -n "${SIGNALING_TOKEN}" ]; then
    LOG_STREAMER_URL="${STREAMER_URL%%token=*}token=<redacted>"
fi
echo "[launch-chromium] url=${LOG_STREAMER_URL}" >&2
echo "[launch-chromium] browser_bin=${CHROMELESS_BROWSER_BIN}" >&2

open_streamer_after_devtools() {
    encoded_url=$(
        STREAMER_URL="${STREAMER_URL}" python3 - <<'PY'
import os
import urllib.parse

print(urllib.parse.quote(os.environ["STREAMER_URL"], safe=""))
PY
    )

    attempts="${STREAMER_LAUNCH_WAIT_ATTEMPTS:-150}"
    i=0
    while [ "$i" -lt "$attempts" ]; do
        if curl -fsS http://127.0.0.1:9222/json/version >/dev/null 2>&1; then
            if curl -fsS -X PUT "http://127.0.0.1:9222/json/new?${encoded_url}" \
                    >/tmp/chromeless-streamer-target.json \
                    2>/tmp/chromeless-streamer-open.err; then
                echo "[launch-chromium] streamer target opened via DevTools" >&2
                return 0
            fi
        fi
        i=$((i + 1))
        sleep 0.2
    done

    echo "[launch-chromium] WARNING: DevTools did not open streamer target after ${attempts} attempts" >&2
    if [ -s /tmp/chromeless-streamer-open.err ]; then
        sed 's/^/[launch-chromium] streamer-open: /' /tmp/chromeless-streamer-open.err >&2 || true
    fi
    return 0
}

# T86: optional --use-fake-device-for-media-stream gate. When the env
# var is "1", we add the flag at the end of argv so it overrides any
# earlier defaults. Logged at startup so docker-compose/k8s logs make
# the synthetic-media mode obvious — silent fakes are how production
# accidents happen.
#
# T109 layers on top: when CHROMELESS_USE_FAKE_MEDIA_FILE points at an
# existing readable y4m, we additionally pass
# --use-file-for-fake-video-capture-loop=<path>. The `-loop` suffix
# (vs the unsuffixed flag) tells Chromium to play the file
# continuously instead of stopping after one pass — necessary for a
# 60-second T65 measurement against a 30-second fixture.
fake_media_arg=""
if [ "${CHROMELESS_USE_FAKE_MEDIA}" = "1" ]; then
    fake_media_arg="--use-fake-device-for-media-stream"
    echo "[launch-chromium] WARNING: CHROMELESS_USE_FAKE_MEDIA=1 -> appending ${fake_media_arg}" >&2
    echo "[launch-chromium] WARNING: synthetic media is for T86/T109 testing only; do not ship this way" >&2
    if [ -n "${CHROMELESS_USE_FAKE_MEDIA_FILE}" ]; then
        if [ -r "${CHROMELESS_USE_FAKE_MEDIA_FILE}" ]; then
            fake_media_arg="${fake_media_arg} --use-file-for-fake-video-capture-loop=${CHROMELESS_USE_FAKE_MEDIA_FILE}"
            echo "[launch-chromium] T109: replaying fixture ${CHROMELESS_USE_FAKE_MEDIA_FILE}" >&2
        else
            echo "[launch-chromium] WARNING: CHROMELESS_USE_FAKE_MEDIA_FILE=${CHROMELESS_USE_FAKE_MEDIA_FILE} is not readable; falling back to synthetic green square" >&2
        fi
    fi
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
"${CHROMELESS_BROWSER_BIN}" \
  --no-sandbox \
  --disable-dev-shm-usage \
  --display=:99 \
  --remote-debugging-port=9222 \
  --remote-debugging-address=127.0.0.1 \
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
  --app="${STREAMER_URL}" &

chromium_pid=$!

terminate() {
    kill -TERM "${chromium_pid}" 2>/dev/null || true
}
trap terminate INT TERM

open_streamer_after_devtools &
opener_pid=$!

wait "${chromium_pid}"
status=$?
wait "${opener_pid}" 2>/dev/null || true
exit "${status}"

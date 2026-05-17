#!/bin/sh
# infra/launch-chromeless.sh — wrapper invoked by [program:chromium] in
# supervisord. Execs Chromium with the native-peer flag list.
#
# Splitting this out of supervisord.conf lets us:
#   - parameterise per-session values (SESSION_ID, SIGNALING_URL) without
#     a re-baked image,
#   - keep the Chromium command line in one shellcheck-friendly place,
#   - exec Chromium directly so it inherits supervisord's PID slot
#     (signals propagate, no extra wrapper process).
#
# Env (all optional):
#   SESSION_ID            session identifier             (default: dev)
#   SIGNALING_URL         WS URL the native peer dials   (default: ws://signaling:8080/ws)
#   SIGNALING_TOKEN       optional browser-side JWT      (default: empty)
#   CHROMIUM_START_URL    URL the first tab opens at     (default: about:blank)
#   CHROMELESS_BROWSER_BIN browser executable            (default: /usr/local/bin/chromeless when present,
#                                                          otherwise /usr/bin/chromium)
#   CHROMELESS_USE_FAKE_MEDIA  when "1", appends
#                         --use-fake-device-for-media-stream for harness
#                         testing. Do not set in production.

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
: "${CHROMELESS_USE_FAKE_MEDIA:=}"
# M7 R3: STREAMER_* env vars removed. Native peer (M1) builds the
# PeerConnection in the browser process; signaling URL still flows
# through SIGNALING_URL but the streamer page that consumed it is gone.
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

if [ -z "${CHROMELESS_BROWSER_BIN:-}" ]; then
    if [ -x /usr/local/bin/chromeless ]; then
        CHROMELESS_BROWSER_BIN=/usr/local/bin/chromeless
    else
        CHROMELESS_BROWSER_BIN=/usr/bin/chromium
    fi
fi

echo "[launch-chromium] session=${SESSION_ID} signaling=${SIGNALING_URL}" >&2
echo "[launch-chromium] browser_bin=${CHROMELESS_BROWSER_BIN}" >&2

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

# Determine the URL chromium opens at startup. M7 R3: streamer
# page is gone; native peer (M1) builds the PeerConnection in the
# browser process.
#   1. CHROMIUM_START_URL from pod env (Triform's BSP CR template
#      injects a per-pool default URL).
#   2. about:blank — the safe default when neither knob is set.
if [ -z "${CHROMIUM_START_URL:-}" ]; then
    CHROMIUM_START_URL="about:blank"
fi

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
  --auto-accept-this-tab-capture \
  --enable-usermedia-screen-capturing \
  ${fake_media_arg} \
  --app="${CHROMIUM_START_URL}" &

chromium_pid=$!

terminate() {
    kill -TERM "${chromium_pid}" 2>/dev/null || true
}
trap terminate INT TERM

wait "${chromium_pid}"
exit "$?"

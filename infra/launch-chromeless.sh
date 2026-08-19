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
#   CHROMELESS_ICE_SERVERS           ICE server JSON (array, or an object with
#                         an `iceServers` array). Unset → public STUN only.
#   CHROMELESS_ICE_TRANSPORT_POLICY  "all" (default) or "relay"
#
# OSS-W1: the four friendly vars above are TRANSLATED below into the
# WEBRTC_SIGNALING_* / WEBRTC_ICE_* vars the browser process actually reads.
# Setting a WEBRTC_* var directly always wins, so the Kubernetes controller
# path (which sets them directly) is unchanged.
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

# OSS-W1 — translate SIGNALING_URL into the WEBRTC_SIGNALING_* vars the
# native peer actually reads.
#
# The browser process reads WEBRTC_SIGNALING_HOST / _SESSION_ID / _TLS /
# _TOKEN (capture/signaling/cb_signaling_ws_client.cc:35-38). This script
# only ever knew SIGNALING_URL and never translated it, so the ONLY thing
# in the repo that could start a session was the Kubernetes controller
# (infra/controllers/.../reconciler/session.go:450). A plain `docker run`
# or `docker compose up` booted a CDP-only worker with no peer at all —
# exactly the state cloud_browser_browser_main_parts.cc:665 warns about.
#
# Precedence: an explicitly-set WEBRTC_SIGNALING_* always wins. That keeps
# the controller path byte-identical (it sets those vars directly and never
# sets SIGNALING_URL), so this is purely additive for Kubernetes while
# making the compose/docker path work for the first time.
#
# The URL is parsed with POSIX parameter expansion rather than sed/awk to
# keep this dash-compatible and dependency-free.
if [ -z "${WEBRTC_SIGNALING_HOST:-}" ] && [ -n "${SIGNALING_URL:-}" ]; then
    # Strip the scheme, remembering whether it was TLS.
    case "${SIGNALING_URL}" in
        wss://*|https://*)
            _cb_tls=1
            _cb_rest="${SIGNALING_URL#*://}"
            ;;
        ws://*|http://*)
            _cb_tls=0
            _cb_rest="${SIGNALING_URL#*://}"
            ;;
        *)
            # No scheme — assume host[:port][/path] and default to TLS, which
            # matches WsClientConfig::use_tls's own default.
            _cb_tls=1
            _cb_rest="${SIGNALING_URL}"
            ;;
    esac

    # host[:port] is everything before the first '/'. The path is dropped:
    # the peer builds its own path from the session id, so a path here would
    # be silently ignored downstream. Warn instead of pretending otherwise.
    _cb_hostport="${_cb_rest%%/*}"
    _cb_path="${_cb_rest#"${_cb_hostport}"}"

    if [ -n "${_cb_hostport}" ]; then
        WEBRTC_SIGNALING_HOST="${_cb_hostport}"
        WEBRTC_SIGNALING_TLS="${_cb_tls}"
        export WEBRTC_SIGNALING_HOST WEBRTC_SIGNALING_TLS
        echo "[launch-chromium] derived WEBRTC_SIGNALING_HOST=${WEBRTC_SIGNALING_HOST}" \
             "WEBRTC_SIGNALING_TLS=${WEBRTC_SIGNALING_TLS} from SIGNALING_URL" >&2
        if [ -n "${_cb_path}" ] && [ "${_cb_path}" != "/" ]; then
            echo "[launch-chromium] NOTE: ignoring path '${_cb_path}' from" \
                 "SIGNALING_URL — the native peer builds its own signaling" \
                 "path from the session id." >&2
        fi
    else
        echo "[launch-chromium] WARNING: could not parse a host out of" \
             "SIGNALING_URL='${SIGNALING_URL}'; the native peer will not start." >&2
    fi
    unset _cb_tls _cb_rest _cb_hostport _cb_path
fi

# Session id and token likewise flow through to the peer. Same precedence
# rule: an explicit WEBRTC_SIGNALING_* wins.
if [ -z "${WEBRTC_SIGNALING_SESSION_ID:-}" ] && [ -n "${SESSION_ID:-}" ]; then
    WEBRTC_SIGNALING_SESSION_ID="${SESSION_ID}"
    export WEBRTC_SIGNALING_SESSION_ID
fi
if [ -z "${WEBRTC_SIGNALING_TOKEN:-}" ] && [ -n "${SIGNALING_TOKEN:-}" ]; then
    WEBRTC_SIGNALING_TOKEN="${SIGNALING_TOKEN}"
    export WEBRTC_SIGNALING_TOKEN
fi

# ICE servers. CHROMELESS_ICE_SERVERS is the friendly name; the peer reads
# WEBRTC_ICE_SERVERS (capture/signaling/cb_ice_config.cc:30). Without either,
# the peer falls back to public STUN, which will not traverse most NATs — say
# so once at boot rather than letting it be discovered as a silent failure.
if [ -z "${WEBRTC_ICE_SERVERS:-}" ] && [ -n "${CHROMELESS_ICE_SERVERS:-}" ]; then
    WEBRTC_ICE_SERVERS="${CHROMELESS_ICE_SERVERS}"
    export WEBRTC_ICE_SERVERS
fi
if [ -z "${WEBRTC_ICE_TRANSPORT_POLICY:-}" ] && [ -n "${CHROMELESS_ICE_TRANSPORT_POLICY:-}" ]; then
    WEBRTC_ICE_TRANSPORT_POLICY="${CHROMELESS_ICE_TRANSPORT_POLICY}"
    export WEBRTC_ICE_TRANSPORT_POLICY
fi
if [ -n "${WEBRTC_SIGNALING_HOST:-}" ] && [ -z "${WEBRTC_ICE_SERVERS:-}" ]; then
    echo "[launch-chromium] NOTE: no ICE servers configured (set" \
         "CHROMELESS_ICE_SERVERS) — falling back to public STUN, which will" \
         "not traverse most NATs." >&2
fi

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

# CV2-ICE: BeginFrame-driven capture flags (read cb_begin_frame_driver.h for the
# full why). The video pipeline drives frame production by issuing external
# BeginFrames at 30fps on the root compositor (the FrameSinkVideoCapturer is
# pull-mode and Xvfb has no real vsync, so without a driven source the captured
# renderer idles at ~0.5fps). Two flags make that loop actually produce frames:
#
#   --disable-new-content-rendering-timeout
#       The 4s new-content timeout would otherwise force a stale/blank surface
#       after a cross-document navigation, fighting the externally-paced frames;
#       disabling it lets the paced BeginFrames own frame production end-to-end.
#
#   --run-all-compositor-stages-before-draw   (LayerTreeSettings::
#       wait_for_all_pipeline_stages_before_draw). DECISIVE for this fix.
#       Without it, the renderer's cc::SchedulerStateMachine skips the
#       main-thread stages (BeginMainFrame/commit/activate) whenever
#       ShouldSendBeginMainFrame() sees no damage — so a delivered external
#       BeginFrame on a momentarily-quiescent frame produces nothing and capture
#       stalls between damage events. With it, the renderer runs the FULL
#       pipeline on EVERY BeginFrame it receives, so once the (visible,
#       subscribed) renderer is on our external source, each 33ms tick
#       deterministically yields a fresh CompositorFrame the capturer delivers.
#       This is the canonical headless-deterministic-capture configuration
#       (the same setting web-tests / headless screenshotting rely on).
#       NOTE: it does NOT, by itself, make an IDLE renderer (no rAF, no damage,
#       so client_needs_begin_frame_=false → support not subscribed to our
#       source) start producing — that gate is renderer-controlled and is why
#       the captured tab must be WasShown()+Focus()'d (cb_devtools_agent.cc) and
#       why a truly static page still needs encoder-side hold-and-repeat for CFR.
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
  --disable-new-content-rendering-timeout \
  --run-all-compositor-stages-before-draw \
  --window-size=1920,1080 \
  --window-position=0,0 \
  --autoplay-policy=no-user-gesture-required \
  --use-fake-ui-for-media-stream \
  --auto-select-desktop-capture-source="Entire screen" \
  --auto-accept-this-tab-capture \
  --enable-usermedia-screen-capturing \
  --enable-logging=stderr \
  --v="${CHROMELESS_CHROME_VLOG:-0}" \
  --vmodule="${CHROMELESS_CHROME_VMODULE:-cb_offerer_driver=1,cb_ice_config=1,cb_devtools_agent=1}" \
  ${fake_media_arg} \
  --app="${CHROMIUM_START_URL}" &

chromium_pid=$!

terminate() {
    kill -TERM "${chromium_pid}" 2>/dev/null || true
}
trap terminate INT TERM

wait "${chromium_pid}"
exit "$?"

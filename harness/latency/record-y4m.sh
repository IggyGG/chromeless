#!/usr/bin/env bash
# record-y4m.sh — record the harness page as a Chromium-compatible
# y4m fixture for T109's --use-file-for-fake-video-capture-loop path.
#
# The stack today (Phase 1, Chromium 147 + Xvfb under the
# CBWRTC_USE_FAKE_MEDIA path) can't do real getDisplayMedia, so the
# cloud Chromium's synthetic camera is the only signal we have.
# The synthetic camera is a moving green square though — it doesn't
# carry the harness page's QR codes — so reconcile.py sees zero
# decode rate (qa-tester's T65 finding).
#
# T109's workaround: pre-record the harness page running in a
# regular Chrome on the dev host into a y4m file. The file IS
# the harness output, frame for frame, so reconcile.py recovers
# real timestamps when the cloud Chromium replays it.
#
# Output: harness/latency/fixtures/harness-loop-<W>p<FPS>.y4m
#
# Usage:
#   bash harness/latency/record-y4m.sh             # 1280x720 @ 30 fps × 30 s
#   DURATION=60 RES=1080p bash harness/latency/record-y4m.sh
#
# Env knobs (all optional):
#   RES         "720p" (default) | "1080p" | "WxH"
#   FPS         "30"  (default)
#   DURATION    "30"  seconds (default)
#   PORT        "8765" (default) — port for the local http server
#               that serves the harness page during recording
#   OUT         override output filename
#
# Requirements:
#   - chromium (or chromium-browser, or google-chrome) on PATH
#   - ffmpeg with libx264 + the y4m muxer (Debian: "ffmpeg" package)
#   - python3 (for the throwaway http server)
#
# This is a developer-machine tool. It is NOT run inside the cloud
# Chromium container — the recording pass is on a host with a real
# display server (or Xvfb + xdotool, but the simpler path is
# headed Chrome at a localhost URL).

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
FIXTURES_DIR="$HERE/fixtures"
mkdir -p "$FIXTURES_DIR"

RES="${RES:-720p}"
FPS="${FPS:-30}"
DURATION="${DURATION:-30}"
PORT="${PORT:-8765}"

case "$RES" in
    720p)   WIDTH=1280; HEIGHT=720;   RES_LABEL=720p ;;
    1080p)  WIDTH=1920; HEIGHT=1080;  RES_LABEL=1080p ;;
    *x*)    WIDTH="${RES%x*}"; HEIGHT="${RES#*x}"; RES_LABEL="${WIDTH}x${HEIGHT}" ;;
    *)      echo "RES must be 720p|1080p|WxH (got '$RES')" >&2; exit 2 ;;
esac

OUT="${OUT:-$FIXTURES_DIR/harness-loop-${RES_LABEL}${FPS}.y4m}"

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "ERROR: ffmpeg not on PATH; install ffmpeg first" >&2
    exit 2
fi
# Find a Chromium-family browser. Order matters: we prefer the same
# Chromium build as the cloud container so frame timing matches.
CHROME=""
for c in chromium chromium-browser google-chrome chrome; do
    if command -v "$c" >/dev/null 2>&1; then
        CHROME="$c"; break
    fi
done
if [ -z "$CHROME" ]; then
    echo "ERROR: no chromium/google-chrome on PATH" >&2
    exit 2
fi

echo ">>> recording $RES_LABEL @ ${FPS} fps × ${DURATION}s"
echo "    output: $OUT"
echo "    chrome: $CHROME"

# 1. Spin up a local HTTP server pointed at harness/latency. The
#    harness page won't open the websocket sink (we run with no
#    sink), and that's fine — the recording captures the visual
#    output, which is all reconcile.py needs.
SERVER_LOG="$(mktemp -t harness-y4m-XXXXXX.log)"
trap 'rm -f "$SERVER_LOG" 2>/dev/null || true' EXIT

(
    cd "$HERE"
    python3 -m http.server "$PORT" >"$SERVER_LOG" 2>&1
) &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true; rm -f "$SERVER_LOG"' EXIT

# Wait for the server to bind.
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if curl -sf "http://127.0.0.1:${PORT}/index.html" >/dev/null 2>&1; then
        break
    fi
    sleep 0.2
done
if ! curl -sf "http://127.0.0.1:${PORT}/index.html" >/dev/null 2>&1; then
    echo "ERROR: harness http server did not come up on :${PORT}" >&2
    cat "$SERVER_LOG" >&2
    exit 1
fi

# 2. Launch Chromium in a fresh user-data-dir, headless-but-with-
#    a-real-frame-buffer (--headless=new) at exactly WxH so the
#    page fills the viewport. We use the harness page's `period`
#    query param to set the flash period to 1000ms — matches the
#    sample-input fixture's expectation.
USERDATA="$(mktemp -d -t harness-y4m-userdata-XXXXXX)"
trap 'kill $SERVER_PID 2>/dev/null || true; rm -rf "$USERDATA"; rm -f "$SERVER_LOG"' EXIT

URL="http://127.0.0.1:${PORT}/index.html?period=1000&run=y4m-${RES_LABEL}${FPS}&sink=ws://127.0.0.1:1/disabled"

# Chromium's --screenshot mode doesn't give us a y4m. Instead, we
# launch Chromium pointed at the page, then use ffmpeg's x11grab
# (Linux) or avfoundation (macOS) to capture the rendered window.
# The simpler portable path: use Chromium's --kiosk mode + ffmpeg
# screen capture. To keep the script truly portable we go with the
# screen-capture approach gated on platform.

UNAME="$(uname -s)"
case "$UNAME" in
    Linux)
        # Use xvfb-run + ffmpeg x11grab for headless-capable Linux
        # CI; falls back to direct x11grab on a desktop.
        if command -v xvfb-run >/dev/null 2>&1 && [ -z "${DISPLAY:-}" ]; then
            export DISPLAY=:99
            Xvfb "$DISPLAY" -screen 0 "${WIDTH}x${HEIGHT}x24" &
            XVFB_PID=$!
            trap "kill \$SERVER_PID 2>/dev/null || true; kill $XVFB_PID 2>/dev/null || true; rm -rf \"$USERDATA\"; rm -f \"$SERVER_LOG\"" EXIT
            sleep 0.5
        fi
        "$CHROME" \
            --user-data-dir="$USERDATA" \
            --no-first-run --no-default-browser-check \
            --window-size="${WIDTH},${HEIGHT}" \
            --kiosk "$URL" \
            >/dev/null 2>&1 &
        CHROME_PID=$!
        trap "kill \$CHROME_PID 2>/dev/null || true; kill \$SERVER_PID 2>/dev/null || true; kill \${XVFB_PID:-1} 2>/dev/null || true; rm -rf \"$USERDATA\"; rm -f \"$SERVER_LOG\"" EXIT
        sleep 2  # let the harness page fully load + start flashing
        ffmpeg -y -framerate "$FPS" -video_size "${WIDTH}x${HEIGHT}" \
            -f x11grab -i "${DISPLAY:-:0}" \
            -t "$DURATION" \
            -pix_fmt yuv420p \
            -f yuv4mpegpipe "$OUT"
        ;;
    Darwin)
        # macOS dev box — use -f avfoundation. The "1:none" device
        # is the main display; on multi-monitor systems set
        # AVF_DEVICE explicitly. We launch Chromium in a window
        # rather than --kiosk so the developer can see what's
        # being captured.
        AVF_DEVICE="${AVF_DEVICE:-1:none}"
        "$CHROME" \
            --user-data-dir="$USERDATA" \
            --no-first-run --no-default-browser-check \
            --window-size="${WIDTH},${HEIGHT}" \
            --window-position=0,0 \
            --app="$URL" \
            >/dev/null 2>&1 &
        CHROME_PID=$!
        trap "kill \$CHROME_PID 2>/dev/null || true; kill \$SERVER_PID 2>/dev/null || true; rm -rf \"$USERDATA\"; rm -f \"$SERVER_LOG\"" EXIT
        sleep 2
        ffmpeg -y -framerate "$FPS" -video_size "${WIDTH}x${HEIGHT}" \
            -f avfoundation -i "$AVF_DEVICE" \
            -t "$DURATION" \
            -pix_fmt yuv420p \
            -f yuv4mpegpipe "$OUT"
        ;;
    *)
        echo "ERROR: unsupported platform '$UNAME'; record-y4m.sh supports Linux + macOS" >&2
        exit 2
        ;;
esac

if [ ! -s "$OUT" ]; then
    echo "ERROR: ffmpeg produced no output at $OUT" >&2
    exit 1
fi

echo
echo "OK: wrote $(du -h "$OUT" | cut -f1) to $OUT"
echo
echo "Next:"
echo "  1. Bind-mount harness/latency/fixtures/ at /opt/cb-fixtures/ in the"
echo "     compose / k8s pod (compose.yaml T109 entry already does this)."
echo "  2. Set CBWRTC_USE_FAKE_MEDIA=1 +"
echo "         CBWRTC_USE_FAKE_MEDIA_FILE=/opt/cb-fixtures/$(basename "$OUT")"
echo "  3. Run the T65 latency-measurement workflow against the running stack."

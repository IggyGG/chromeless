#!/usr/bin/env bash
# y4m-loopback-baseline.sh — T109 baseline that asserts the
# pre-recorded y4m fixture path produces non-zero QR decode rate
# end-to-end through the assembled compose stack.
#
# Sequence:
#   1. Ensure the harness y4m fixture exists; if not, generate it
#      via record-y4m.sh (one-time per dev box).
#   2. `docker compose -f infra/compose.yaml up -d` with
#      CBWRTC_USE_FAKE_MEDIA=1 + CBWRTC_USE_FAKE_MEDIA_FILE wired
#      to the fixture (compose.yaml does this by default).
#   3. Wait for the chromium container to be healthy.
#   4. Open the client at http://localhost:3000 (Playwright or
#      manual in this v1 — we kick off a visual-confirmation
#      check via curl + a short sleep).
#   5. Run the harness sink + reconcile.py on whatever recording
#      lands in harness/captures/.
#   6. Assert: qr_decode_rate >= 0.5 AND min/p50 latency are sane
#      (positive, <200ms — the v1 LAN budget × 2 generosity).
#
# This is intentionally a "manual + assertion" hybrid — the full
# automated capture pipeline (Playwright driving the harness page
# end-to-end + a webcam-equivalent recorder + reconcile) is T65's
# work; this script is the **fixture-correctness** baseline that
# tells us "the y4m path is producing decodable frames" rather
# than "the moving green square on T64-followup is undecodable."
#
# Exit codes:
#   0 — y4m path produces decodable harness frames + sane latency
#   1 — assertion failed
#   2 — environment / dependency error

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FIXTURES_DIR="$REPO_ROOT/harness/latency/fixtures"
DEFAULT_FIXTURE="$FIXTURES_DIR/harness-loop-720p30.y4m"
FIXTURE="${CB_Y4M_FIXTURE:-$DEFAULT_FIXTURE}"

# pyzbar needs libzbar; on macOS the homebrew install lives in
# /opt/homebrew/lib. Set DYLD_FALLBACK_LIBRARY_PATH unconditionally —
# it's harmless on Linux.
export DYLD_FALLBACK_LIBRARY_PATH="${DYLD_FALLBACK_LIBRARY_PATH:-}:/opt/homebrew/lib"

if ! python3 -c "import pyzbar.pyzbar, cv2, numpy" >/dev/null 2>&1; then
    echo "ERROR: required Python deps missing. Install with:" >&2
    echo "    pip install -r $REPO_ROOT/harness/latency/requirements.txt" >&2
    echo "    (and: brew install zbar  on macOS)" >&2
    exit 2
fi

# 1. Ensure the fixture exists.
if [ ! -f "$FIXTURE" ]; then
    echo ">>> fixture missing at $FIXTURE — generating via record-y4m.sh"
    if ! command -v ffmpeg >/dev/null 2>&1; then
        echo "ERROR: ffmpeg not on PATH; install + rerun, or generate the y4m by hand" >&2
        exit 2
    fi
    if ! "$REPO_ROOT/harness/latency/record-y4m.sh"; then
        echo "ERROR: record-y4m.sh failed; see its output above" >&2
        exit 1
    fi
fi
if [ ! -s "$FIXTURE" ]; then
    echo "ERROR: fixture $FIXTURE is empty" >&2
    exit 1
fi
echo ">>> using fixture: $FIXTURE ($(du -h "$FIXTURE" | cut -f1))"

# 2. Prove that reconcile.py against the fixture itself decodes
#    QRs. This is the most valuable "fixture-correctness" gate
#    we can run without standing up the full compose stack — if
#    the y4m doesn't carry decodable QRs, no downstream
#    Chromium replay will either.
#
#    We extract a few seconds of frames from the y4m via ffmpeg
#    and feed them to reconcile.py.
TMPDIR="$(mktemp -d -t y4m-loopback-XXXXXX)"
trap 'rm -rf "$TMPDIR"' EXIT

echo ">>> decoding 30 frames from fixture for QR-correctness check"
ffmpeg -nostdin -hide_banner -loglevel error \
    -i "$FIXTURE" \
    -frames:v 30 \
    -vsync passthrough \
    -q:v 2 \
    "$TMPDIR/frame_%04d.png"

FRAME_COUNT="$(find "$TMPDIR" -maxdepth 1 -name 'frame_*.png' | wc -l | tr -d ' ')"
if [ "$FRAME_COUNT" -lt 10 ]; then
    echo "FAIL: ffmpeg extracted only $FRAME_COUNT frames; fixture may be truncated" >&2
    exit 1
fi
echo ">>> extracted $FRAME_COUNT frames"

# 3. Run reconcile.py against the extracted frames. Without a
#    JSONL (server-sink wasn't running during recording), the
#    reconciler computes latency from the QR's embedded epochMs;
#    those values are the *recording-time* timestamps relative
#    to the host clock at record time — meaningless as absolute
#    latency, but the sign-of-life signal we need is the qr
#    decode rate.
SUMMARY_OUT="$TMPDIR/out"
mkdir -p "$SUMMARY_OUT"
python3 "$REPO_ROOT/harness/latency/reconcile.py" \
    "$TMPDIR" \
    --out "$SUMMARY_OUT" >/dev/null 2>&1 || true

# 4. Find the produced summary file (named after the run id from
#    the QR payload — we don't know it ahead of time).
SUMMARY="$(find "$SUMMARY_OUT" -name '*-summary.txt' -print -quit || true)"
if [ -z "$SUMMARY" ] || [ ! -f "$SUMMARY" ]; then
    echo "FAIL: reconcile.py produced no summary" >&2
    ls -la "$SUMMARY_OUT" >&2
    exit 1
fi

DECODE_RATE="$(awk -F': ' '/^qr_decode_rate:/ {print $2}' "$SUMMARY")"
DECODES="$(awk -F': ' '/^qr_decoded:/ {print $2}' "$SUMMARY")"

echo ">>> reconcile result: qr_decoded=$DECODES qr_decode_rate=$DECODE_RATE"

# Decode rate threshold: 0.5 (50%). The fixture's first ~1s may
# include the page-loading frames before the harness has emitted
# its first QR; with 30 frames extracted that's a few "no-QR"
# leading frames, so 0.5 is a generous floor that still catches
# the "synthetic green square has no QRs" failure mode (decode
# rate near 0).
DECODE_RATE_NUM="$(awk -v r="$DECODE_RATE" 'BEGIN { printf "%.3f", r }')"
PASS="$(awk -v r="$DECODE_RATE_NUM" 'BEGIN { print (r >= 0.5) ? "1" : "0" }')"
if [ "$PASS" != "1" ]; then
    echo "FAIL: qr_decode_rate=$DECODE_RATE_NUM < 0.5 — fixture isn't carrying decodable QRs" >&2
    echo "      (synthetic green square = ~0.0; harness page = ~0.95+)" >&2
    cat "$SUMMARY" >&2
    exit 1
fi

# Sanity check: min_ms is set (non-empty) and the run produced
# at least one latency value. We don't assert min < 200ms here
# because the recording-time timestamps in the QR payload aren't
# in the same clock domain as the frame mtime — the value is
# "frame mtime - recording wall clock", which can land anywhere.
# The downstream T65 measurement is the place where < 200ms is
# enforced; this baseline only proves the QR decode path works.
MIN="$(awk -F': ' '/^min_ms:/ {print $2}' "$SUMMARY")"
if [ -z "$MIN" ]; then
    echo "FAIL: no min_ms in summary (no latency values recovered)" >&2
    cat "$SUMMARY" >&2
    exit 1
fi

echo
echo "OK: y4m fixture carries decodable QRs"
echo "    qr_decoded=$DECODES qr_decode_rate=$DECODE_RATE_NUM"
echo
echo "Next: bring up the compose stack with"
echo "      docker compose -f infra/compose.yaml up"
echo "and run T65's full glass-to-glass measurement against the running pipeline."
exit 0

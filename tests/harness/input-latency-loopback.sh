#!/usr/bin/env bash
# Loopback baseline for the input-latency harness.
#
# Companion to T12's video-latency loopback baseline (see
# tests/harness/README.md). This script runs the input-latency
# reconciler against the bundled deterministic sample frames and
# asserts the recovered numbers match the planted values exactly.
#
# It is the cheapest gate that proves the harness itself is sane:
# any drift here indicates a regression in reconcile.py's parsing,
# pairing, or summary code, not in the system under test.
#
# Usage:
#   bash tests/harness/input-latency-loopback.sh
#
# Exit codes:
#   0 — all assertions pass
#   1 — at least one assertion failed
#   2 — environment / dependency error (caller must fix and retry)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SAMPLE_DIR="$REPO_ROOT/harness/input-latency/sample-input"
KEYSTROKES="$SAMPLE_DIR/keystrokes.jsonl"
OUT_DIR="$(mktemp -d -t input-latency-loopback-XXXXXX)"
trap 'rm -rf "$OUT_DIR"' EXIT

# pyzbar needs libzbar; on macOS the homebrew install lives in
# /opt/homebrew/lib. Set DYLD_FALLBACK_LIBRARY_PATH unconditionally —
# it's harmless on Linux.
export DYLD_FALLBACK_LIBRARY_PATH="${DYLD_FALLBACK_LIBRARY_PATH:-}:/opt/homebrew/lib"

if ! python3 -c "import pyzbar.pyzbar, cv2, numpy, matplotlib" >/dev/null 2>&1; then
    echo "ERROR: required Python deps missing. Install with:" >&2
    echo "    pip install -r $REPO_ROOT/harness/latency/requirements.txt" >&2
    echo "    (and: brew install zbar  on macOS)" >&2
    exit 2
fi

if [[ ! -d "$SAMPLE_DIR" ]] || [[ ! -f "$KEYSTROKES" ]]; then
    echo "ERROR: sample input missing at $SAMPLE_DIR" >&2
    exit 2
fi

echo ">>> running input-latency reconcile on $SAMPLE_DIR"
python3 "$REPO_ROOT/harness/input-latency/reconcile.py" \
    "$SAMPLE_DIR" \
    --manifest "$KEYSTROKES" \
    --pair-window-ms 500 \
    --out "$OUT_DIR" \
    --exit-nonzero-if-no-pairs

CSV="$OUT_DIR/sample-input-input-latencies.csv"
SUMMARY="$OUT_DIR/sample-input-input-summary.txt"

if [[ ! -f "$CSV" ]] || [[ ! -f "$SUMMARY" ]]; then
    echo "FAIL: reconcile produced no CSV/summary" >&2
    ls -la "$OUT_DIR"
    exit 1
fi

# Expected: 3 pairs with planted latencies 55, 72, 89 ms.
EXPECTED_LATENCIES=(55 72 89)

# Python's csv module writes CRLF on every platform; strip the \r so
# integer comparisons don't fail with cryptic "55 != 55" errors.
ACTUAL_LATENCIES=()
while IFS= read -r row; do
    ACTUAL_LATENCIES+=("${row%$'\r'}")
done < <(tail -n +2 "$CSV" | awk -F, '{print $8}')

if [[ "${#ACTUAL_LATENCIES[@]}" -ne 3 ]]; then
    echo "FAIL: expected 3 pairs, got ${#ACTUAL_LATENCIES[@]}" >&2
    cat "$CSV" >&2
    exit 1
fi

# We sort both before comparing so test ordering is irrelevant.
mapfile -t EXP_SORTED < <(printf '%s\n' "${EXPECTED_LATENCIES[@]}" | sort -n)
mapfile -t GOT_SORTED < <(printf '%s\n' "${ACTUAL_LATENCIES[@]}"   | sort -n)
for i in 0 1 2; do
    if [[ "${EXP_SORTED[$i]}" != "${GOT_SORTED[$i]}" ]]; then
        echo "FAIL: latency[$i]: want ${EXP_SORTED[$i]}, got ${GOT_SORTED[$i]}" >&2
        echo "--- CSV ---"          >&2
        cat "$CSV"                  >&2
        echo "--- summary ---"      >&2
        cat "$SUMMARY"              >&2
        exit 1
    fi
done

# Min/max sanity check on the summary text too.
EXPECTED_MIN=55.0
EXPECTED_MAX=89.0
ACTUAL_MIN="$(awk -F': ' '/^min_ms:/ {print $2}' "$SUMMARY")"
ACTUAL_MAX="$(awk -F': ' '/^max_ms:/ {print $2}' "$SUMMARY")"
if [[ "$ACTUAL_MIN" != "$EXPECTED_MIN" ]] || [[ "$ACTUAL_MAX" != "$EXPECTED_MAX" ]]; then
    echo "FAIL: min/max mismatch"  >&2
    echo "want min=$EXPECTED_MIN max=$EXPECTED_MAX" >&2
    echo "got  min=$ACTUAL_MIN max=$ACTUAL_MAX"     >&2
    cat "$SUMMARY"                  >&2
    exit 1
fi

echo "OK: all 3 pairs recovered with expected latencies"
echo "    min=$ACTUAL_MIN max=$ACTUAL_MAX"
exit 0

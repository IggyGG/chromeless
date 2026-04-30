#!/usr/bin/env bash
# Regression baseline for the Nyquist 2× rule warning (T60).
#
# The fixture harness/latency/sample-aliased/ has cam frames at 5 Hz
# and a source-emits.jsonl at 10 Hz. cam_fps < 2 × flash_freq, so
# reconcile.py MUST emit:
#   - the WARN line on stdout
#   - the WARN line in the summary text file
#   - aliased_warning: True in the summary
#   - exit code 4 when --strict is passed
#
# Usage:
#   bash tests/harness/aliased-warning-baseline.sh
#
# Exit codes:
#   0 — all assertions pass
#   1 — at least one assertion failed
#   2 — environment / dependency error

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SAMPLE_DIR="$REPO_ROOT/harness/latency/sample-aliased"
JSONL="$SAMPLE_DIR/source-emits.jsonl"
OUT_DIR="$(mktemp -d -t aliased-warning-XXXXXX)"
trap 'rm -rf "$OUT_DIR"' EXIT

export DYLD_FALLBACK_LIBRARY_PATH="${DYLD_FALLBACK_LIBRARY_PATH:-}:/opt/homebrew/lib"

if ! python3 -c "import pyzbar.pyzbar, cv2, numpy, matplotlib" >/dev/null 2>&1; then
    echo "ERROR: required Python deps missing. Install with:" >&2
    echo "    pip install -r $REPO_ROOT/harness/latency/requirements.txt" >&2
    echo "    (and: brew install zbar  on macOS)" >&2
    exit 2
fi

if [[ ! -d "$SAMPLE_DIR" ]] || [[ ! -f "$JSONL" ]]; then
    echo "ERROR: aliased fixture missing at $SAMPLE_DIR" >&2
    exit 2
fi

# ----- Run 1: default (no --strict) — should print WARN, exit 0 -----
echo ">>> running reconcile against aliased fixture (no --strict)"
STDOUT="$(python3 "$REPO_ROOT/harness/latency/reconcile.py" \
    "$SAMPLE_DIR" --jsonl "$JSONL" --out "$OUT_DIR" 2>&1)"
EXIT_DEFAULT=$?

if [[ "$EXIT_DEFAULT" -ne 0 ]]; then
    echo "FAIL: default mode should exit 0, got $EXIT_DEFAULT" >&2
    echo "$STDOUT" >&2
    exit 1
fi

if ! grep -q "WARN: cam_fps=5\.000 < 2 × flash_freq=10\.000" <<<"$STDOUT"; then
    echo "FAIL: stdout should contain the WARN line" >&2
    echo "--- stdout ---" >&2
    echo "$STDOUT" >&2
    exit 1
fi

SUMMARY="$OUT_DIR/sample-aliased-summary.txt"
if [[ ! -f "$SUMMARY" ]]; then
    echo "FAIL: summary file not produced" >&2
    exit 1
fi
if ! grep -q "^WARN: cam_fps=5\.000 < 2 × flash_freq=10\.000" "$SUMMARY"; then
    echo "FAIL: summary file should contain the WARN line" >&2
    cat "$SUMMARY" >&2
    exit 1
fi
if ! grep -q "^aliased_warning: True$" "$SUMMARY"; then
    echo "FAIL: summary should report aliased_warning: True" >&2
    cat "$SUMMARY" >&2
    exit 1
fi
if ! grep -q "^cam_fps_hz: 5\.0$" "$SUMMARY"; then
    echo "FAIL: summary should report cam_fps_hz: 5.0" >&2
    cat "$SUMMARY" >&2
    exit 1
fi
if ! grep -q "^flash_freq_hz: 10\.0$" "$SUMMARY"; then
    echo "FAIL: summary should report flash_freq_hz: 10.0" >&2
    cat "$SUMMARY" >&2
    exit 1
fi

# ----- Run 2: --strict — should exit 4 -----
echo ">>> running reconcile with --strict (expects exit 4)"
set +e
python3 "$REPO_ROOT/harness/latency/reconcile.py" \
    "$SAMPLE_DIR" --jsonl "$JSONL" --strict --out "$OUT_DIR" >/dev/null 2>&1
EXIT_STRICT=$?
set -e

if [[ "$EXIT_STRICT" -ne 4 ]]; then
    echo "FAIL: --strict should exit 4 on aliased run, got $EXIT_STRICT" >&2
    exit 1
fi

# ----- Run 3: clean sample-input/ — no warning, exit 0 even with --strict
echo ">>> running reconcile against clean sample-input (--strict)"
CLEAN_OUT="$(mktemp -d -t aliased-clean-XXXXXX)"
trap 'rm -rf "$OUT_DIR" "$CLEAN_OUT"' EXIT

set +e
CLEAN_STDOUT="$(python3 "$REPO_ROOT/harness/latency/reconcile.py" \
    "$REPO_ROOT/harness/latency/sample-input" --strict --out "$CLEAN_OUT" 2>&1)"
EXIT_CLEAN=$?
set -e

if [[ "$EXIT_CLEAN" -ne 0 ]]; then
    echo "FAIL: clean sample with --strict should exit 0, got $EXIT_CLEAN" >&2
    echo "$CLEAN_STDOUT" >&2
    exit 1
fi
if grep -q "WARN: cam_fps" <<<"$CLEAN_STDOUT"; then
    echo "FAIL: clean sample should NOT emit the aliased WARN line" >&2
    echo "$CLEAN_STDOUT" >&2
    exit 1
fi

echo "OK: Nyquist warning fires on aliased fixture (exit=$EXIT_DEFAULT default, $EXIT_STRICT strict)"
echo "    cam_fps_hz=5.0  flash_freq_hz=10.0  aliased_warning=True"
echo "    clean sample-input/ remains warning-free under --strict"
exit 0

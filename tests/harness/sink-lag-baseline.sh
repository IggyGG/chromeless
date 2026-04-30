#!/usr/bin/env bash
# Regression baseline for sink_lag stats in reconcile.py (T61).
#
# server-sink.py stamps sinkRecvEpochMs on every record it writes, so
# reconcile.py can compute page→sink WebSocket delivery latency. The
# bundled sample-aliased/source-emits.jsonl includes deterministic
# sinkRecvEpochMs values (1..16 ms after each emit), giving:
#
#   sink_lag_count   = 16
#   sink_lag_p50_ms  = 8.5
#   sink_lag_p95_ms  = 15.25
#   sink_lag_max_ms  = 16.0
#
# Usage:
#   bash tests/harness/sink-lag-baseline.sh
#
# Exit codes:
#   0 — all assertions pass
#   1 — at least one assertion failed
#   2 — environment / dependency error

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SAMPLE_DIR="$REPO_ROOT/harness/latency/sample-aliased"
JSONL="$SAMPLE_DIR/source-emits.jsonl"
OUT_DIR="$(mktemp -d -t sink-lag-XXXXXX)"
trap 'rm -rf "$OUT_DIR"' EXIT

export DYLD_FALLBACK_LIBRARY_PATH="${DYLD_FALLBACK_LIBRARY_PATH:-}:/opt/homebrew/lib"

if ! python3 -c "import pyzbar.pyzbar, cv2, numpy, matplotlib" >/dev/null 2>&1; then
    echo "ERROR: required Python deps missing." >&2
    exit 2
fi
if [[ ! -f "$JSONL" ]]; then
    echo "ERROR: source-emits.jsonl missing at $JSONL" >&2
    exit 2
fi

echo ">>> running reconcile against sample-aliased with --jsonl"
python3 "$REPO_ROOT/harness/latency/reconcile.py" \
    "$SAMPLE_DIR" --jsonl "$JSONL" --out "$OUT_DIR" >/dev/null 2>&1 || true

SUMMARY="$OUT_DIR/sample-aliased-summary.txt"
if [[ ! -f "$SUMMARY" ]]; then
    echo "FAIL: summary not produced" >&2
    exit 1
fi

# Pull the sink_lag_* fields from the summary; assert their values.
get() {
    awk -F': ' "/^$1:/ {print \$2}" "$SUMMARY"
}

CNT="$(get sink_lag_count)"
P50="$(get sink_lag_p50_ms)"
P95="$(get sink_lag_p95_ms)"
MAX="$(get sink_lag_max_ms)"

if [[ "$CNT" != "16" ]]; then
    echo "FAIL: sink_lag_count = $CNT, want 16" >&2
    cat "$SUMMARY" >&2; exit 1
fi
if [[ "$P50" != "8.5" ]]; then
    echo "FAIL: sink_lag_p50_ms = $P50, want 8.5" >&2
    cat "$SUMMARY" >&2; exit 1
fi
if [[ "$P95" != "15.25" ]]; then
    echo "FAIL: sink_lag_p95_ms = $P95, want 15.25" >&2
    cat "$SUMMARY" >&2; exit 1
fi
if [[ "$MAX" != "16.0" ]]; then
    echo "FAIL: sink_lag_max_ms = $MAX, want 16.0" >&2
    cat "$SUMMARY" >&2; exit 1
fi

echo "OK: sink_lag stats populate correctly"
echo "    count=$CNT  p50=$P50  p95=$P95  max=$MAX"

# Sanity: when --jsonl is NOT supplied, the fields MUST be absent.
echo ">>> running reconcile WITHOUT --jsonl (sink_lag fields should be absent)"
NOJSONL_OUT="$(mktemp -d -t sink-lag-nojsonl-XXXXXX)"
trap 'rm -rf "$OUT_DIR" "$NOJSONL_OUT"' EXIT
python3 "$REPO_ROOT/harness/latency/reconcile.py" \
    "$REPO_ROOT/harness/latency/sample-input" --out "$NOJSONL_OUT" >/dev/null 2>&1 || true
NJ_SUMMARY="$NOJSONL_OUT/sample-run-summary.txt"
if grep -q "^sink_lag_" "$NJ_SUMMARY"; then
    echo "FAIL: sink_lag fields appeared without --jsonl:" >&2
    grep "^sink_lag_" "$NJ_SUMMARY" >&2
    exit 1
fi
echo "OK: sink_lag fields are absent when --jsonl is not supplied"
exit 0

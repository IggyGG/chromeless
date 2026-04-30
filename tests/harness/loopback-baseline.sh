#!/usr/bin/env bash
#
# tests/harness/loopback-baseline.sh — T12 deliverable.
#
# Hermetic regression check for the latency reconciler. Runs
# harness/latency/reconcile.py against the bundled deterministic
# sample-input/ fixture and asserts the recovered statistics match
# the manifest within tight bounds.
#
# This is the CI-friendly "loopback" — it tests the *reconciler*
# (parsing, time math, percentiles, manifest precedence), not a
# physical webcam-pointed-at-source loop. The latter is documented
# as a manual operator step in tests/harness/validation.md §4.
#
# Why a deterministic fixture instead of a live cam?
#   - Reproducible across hosts, OSes, and CI runners.
#   - No webcam, no recording, no cam-specific tuning.
#   - Catches regressions in reconcile.py itself, which is the only
#     piece that can drift between releases. Cam tuning changes are
#     covered by the operator checklist.
#
# What it asserts (all bounds derived from sample-input/manifest.jsonl):
#   frames_seen     == 4
#   qr_decode_rate  == 1.0
#   negative_count  == 0
#   30 <= min_ms    <= 60
#   35 <= p50_ms    <= 65
#         p95_ms    <= 100
#         max_ms    <= 100
#
# Override the bounds via env if the sample manifest is intentionally
# extended in a future PR; defaults are baked in for the bundled
# fixture as of T12.
#
# Usage:
#   bash tests/harness/loopback-baseline.sh
#
# Knobs (env, all optional):
#   LOOPBACK_OUT_DIR     where reconciler writes its CSV/PNG/summary
#                        (default: /tmp/cb-harness-loopback)
#   LOOPBACK_PYTHON      python3 binary (default: python3)
#
# Prereqs:
#   - python3 with the deps in harness/latency/requirements.txt
#     (opencv-python, pyzbar, numpy, matplotlib).
#   - On macOS: `brew install zbar`. The script auto-sets
#     DYLD_FALLBACK_LIBRARY_PATH if libzbar is in /opt/homebrew/lib
#     or /usr/local/lib.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SAMPLE_DIR="$REPO_ROOT/harness/latency/sample-input"
OUT_DIR="${LOOPBACK_OUT_DIR:-/tmp/cb-harness-loopback}"
PYTHON="${LOOPBACK_PYTHON:-python3}"

step() { printf '\n\033[1m== %s ==\033[0m\n' "$*" >&2; }
log()  { printf '[loopback] %s\n' "$*" >&2; }
fail() {
    printf '\033[31m[loopback] FAIL: %s\033[0m\n' "$*" >&2
    if [ -f "$OUT_DIR/sample-run-summary.txt" ]; then
        printf '\033[31m[loopback] reconciler summary:\033[0m\n' >&2
        sed 's/^/  /' "$OUT_DIR/sample-run-summary.txt" >&2 || true
    fi
    exit 1
}

require() {
    command -v "$1" >/dev/null 2>&1 || fail "required command not found: $1"
}
require "$PYTHON"

# macOS: pyzbar's import-time libzbar discovery needs help.
case "$(uname -s)" in
    Darwin)
        for prefix in /opt/homebrew /usr/local; do
            if [ -e "$prefix/lib/libzbar.dylib" ]; then
                export DYLD_FALLBACK_LIBRARY_PATH="$prefix/lib${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"
                log "set DYLD_FALLBACK_LIBRARY_PATH=$DYLD_FALLBACK_LIBRARY_PATH"
                break
            fi
        done
        ;;
esac

[ -d "$SAMPLE_DIR" ] || fail "sample-input dir not found at $SAMPLE_DIR"
[ -f "$SAMPLE_DIR/manifest.jsonl" ] || fail "sample-input manifest missing"

# ---- 1. run reconcile.py ------------------------------------------------

step "1. run reconcile.py against bundled sample"
log "input:  $SAMPLE_DIR"
log "output: $OUT_DIR"
mkdir -p "$OUT_DIR"

"$PYTHON" "$REPO_ROOT/harness/latency/reconcile.py" "$SAMPLE_DIR" \
    --out "$OUT_DIR" \
    --exit-nonzero-if-no-decodes \
    || fail "reconcile.py exited non-zero"

SUMMARY="$OUT_DIR/sample-run-summary.txt"
[ -f "$SUMMARY" ] || fail "expected $SUMMARY not produced"

# ---- 2. assert thresholds ----------------------------------------------

step "2. assert thresholds against bundled-fixture-derived bounds"

"$PYTHON" - "$SUMMARY" <<'PY' || fail "threshold assertion failed"
import re
import sys

text = open(sys.argv[1]).read()


def num(key):
    m = re.search(rf"^{re.escape(key)}: (.+)$", text, re.M)
    if not m:
        raise SystemExit(f"missing key {key!r} in summary")
    raw = m.group(1).strip()
    try:
        return float(raw)
    except ValueError:
        return raw


# Bounds match validation.md §5 — keep in sync.
checks = [
    ("frames_seen",    lambda v: v == 4.0,    "expected 4 sample frames"),
    ("qr_decoded",     lambda v: v == 4.0,    "expected all 4 QRs decoded"),
    ("qr_decode_rate", lambda v: v == 1.0,    "QR decode rate must be 1.000 for hand-crafted sample"),
    ("negative_count", lambda v: v == 0.0,    "negative_count must be 0"),
    ("min_ms",         lambda v: 30.0 <= v <= 60.0,
                       "min_ms must be in [30,60] for the bundled sample (manifest minima 42)"),
    ("p50_ms",         lambda v: 35.0 <= v <= 65.0,
                       "p50_ms must be in [35,65] for the bundled sample (~49 expected)"),
    ("p95_ms",         lambda v: v <= 100.0,
                       "p95_ms must be <= 100 for the bundled sample (~82 expected)"),
    ("max_ms",         lambda v: v <= 100.0,
                       "max_ms must be <= 100 for the bundled sample (88 expected)"),
]

ok = True
for key, pred, desc in checks:
    v = num(key)
    if not (isinstance(v, float) and pred(v)):
        print(f"  FAIL  {key:>16}={v}    {desc}", file=sys.stderr)
        ok = False
    else:
        print(f"  OK    {key:>16}={v}", file=sys.stderr)

sys.exit(0 if ok else 1)
PY

# ---- done ---------------------------------------------------------------

step "loopback baseline passed"
echo "[loopback] PASS" >&2
exit 0

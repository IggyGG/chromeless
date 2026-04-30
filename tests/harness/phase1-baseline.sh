#!/usr/bin/env bash
#
# tests/harness/phase1-baseline.sh — T65.
#
# Operator wrapper for a real-pipeline glass-to-glass measurement run.
# This is the *measurement-and-bookkeeping* tool — it does not record
# the webcam capture or run the Phase 1 stack itself. The operator
# does that physical setup once per measurement session, following
# the checklist in tests/harness/validation.md §4. This script then:
#
#   1. Runs harness/latency/reconcile.py against the captured cam.mp4
#      + the source-side JSONL.
#   2. Pulls p50/p95/p99/max + qr_decode_rate + negative_count out of
#      the reconciler summary.
#   3. Asserts those numbers against the v1 budget from
#      docs/v1-success-criteria.md and the recommended-ranges from
#      tests/harness/validation.md §3:
#         LAN p50  <= 100 ms,  p95 <= 130 ms
#         regional p50 <= 200 ms, p95 <= 260 ms
#         loopback floor min_ms positive, qr_decode_rate >= 0.95
#   4. Writes a baseline JSON file at
#      tests/harness/baselines/phase1-<runId>.json so future PRs can
#      assert no >5 ms regression vs the recorded baseline (per the
#      tests/README.md regression budget).
#   5. Updates the symlink tests/harness/baselines/phase1-latest.json
#      to point at the new run.
#
# This is NOT the hermetic CI gate — that's loopback-baseline.sh, which
# tests the reconciler logic against deterministic fixtures. This
# script is the operator's tool for producing the v1-defining number.
#
# Usage:
#   bash tests/harness/phase1-baseline.sh \
#       --cam        path/to/cam.mp4 \
#       --jsonl      harness/captures/<runId>.jsonl \
#       --start-ms   "$(cat cam.start.txt)" \
#       --offset-ms  45 \
#       --target     lan          # one of: loopback, lan, regional
#       --note       "first phase 1 measurement"
#
# Knobs (all optional flags except --cam, --jsonl):
#   --cam PATH                webcam mp4. Required.
#   --jsonl PATH              source-side JSONL from server-sink.py. Required.
#   --start-ms N              recording start epoch ms (cat cam.start.txt). Required.
#   --offset-ms N             clock offset from a loopback measurement. Default 0.
#   --target {loopback,lan,regional}
#                             which budget bucket to assert against. Default lan.
#   --note "text"             freeform note recorded in the baseline JSON.
#   --no-assert               don't fail on threshold breach; produce the
#                             baseline anyway. Use when *recording* the
#                             first baseline rather than gating against
#                             an existing one.
#   --out-dir PATH            reconcile.py --out. Default: harness/captures/.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PYTHON="${PYTHON:-python3}"

CAM=""
JSONL=""
START_MS=""
OFFSET_MS="0"
TARGET="lan"
NOTE=""
NO_ASSERT="0"
OUT_DIR="$REPO_ROOT/harness/captures"

step() { printf '\n\033[1m== %s ==\033[0m\n' "$*" >&2; }
log()  { printf '[phase1-baseline] %s\n' "$*" >&2; }
fail() { printf '\033[31m[phase1-baseline] FAIL: %s\033[0m\n' "$*" >&2; exit 1; }

usage() { sed -n '3,50p' "$0" >&2; exit 2; }

while [ $# -gt 0 ]; do
    case "$1" in
        --cam)        CAM="$2"; shift 2 ;;
        --jsonl)      JSONL="$2"; shift 2 ;;
        --start-ms)   START_MS="$2"; shift 2 ;;
        --offset-ms)  OFFSET_MS="$2"; shift 2 ;;
        --target)     TARGET="$2"; shift 2 ;;
        --note)       NOTE="$2"; shift 2 ;;
        --out-dir)    OUT_DIR="$2"; shift 2 ;;
        --no-assert)  NO_ASSERT="1"; shift ;;
        -h|--help)    usage ;;
        *)            fail "unknown arg: $1" ;;
    esac
done

[ -n "$CAM" ]     || { log "missing --cam";    usage; }
[ -n "$JSONL" ]   || { log "missing --jsonl";  usage; }
[ -n "$START_MS" ]|| { log "missing --start-ms (the cam recording's PTS=0 epoch ms)"; usage; }

[ -f "$CAM" ]     || fail "--cam not found: $CAM"
[ -f "$JSONL" ]   || fail "--jsonl not found: $JSONL"

case "$TARGET" in
    loopback|lan|regional) ;;
    *) fail "--target must be one of: loopback, lan, regional (got $TARGET)" ;;
esac

# macOS: pyzbar's import-time libzbar discovery needs help.
case "$(uname -s)" in
    Darwin)
        for prefix in /opt/homebrew /usr/local; do
            if [ -e "$prefix/lib/libzbar.dylib" ]; then
                export DYLD_FALLBACK_LIBRARY_PATH="$prefix/lib${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"
                break
            fi
        done
        ;;
esac

# ---- 1. run reconcile.py -----------------------------------------------

step "1. reconcile.py"
log "cam:       $CAM"
log "jsonl:     $JSONL"
log "start-ms:  $START_MS"
log "offset-ms: $OFFSET_MS"
log "target:    $TARGET"

mkdir -p "$OUT_DIR"

"$PYTHON" "$REPO_ROOT/harness/latency/reconcile.py" "$CAM" \
    --recording-start-epoch-ms "$START_MS" \
    --clock-offset-ms "$OFFSET_MS" \
    --jsonl "$JSONL" \
    --out "$OUT_DIR" \
    --exit-nonzero-if-no-decodes \
    || fail "reconcile.py exited non-zero"

# Extract the runId from the JSONL filename — server-sink.py names files
# <runId>.jsonl, and reconcile.py emits <runId>-summary.txt.
RUN_ID="$(basename "$JSONL" .jsonl)"
SUMMARY="$OUT_DIR/$RUN_ID-summary.txt"
[ -f "$SUMMARY" ] || fail "expected summary missing: $SUMMARY"

# ---- 2. extract numbers + assert thresholds ----------------------------

step "2. assert thresholds for target=$TARGET"

# Bounds match tests/harness/validation.md §3.
"$PYTHON" - "$SUMMARY" "$TARGET" "$NO_ASSERT" <<'PY' || fail "threshold assertion failed"
import json, re, sys

text = open(sys.argv[1]).read()
target = sys.argv[2]
no_assert = sys.argv[3] == "1"

def num(key, optional=False):
    m = re.search(rf"^{re.escape(key)}: (.+)$", text, re.M)
    if not m:
        if optional: return None
        raise SystemExit(f"missing key {key!r} in summary")
    raw = m.group(1).strip()
    try: return float(raw)
    except ValueError: return raw

bounds = {
    "loopback": {"p50_max": 50.0,  "p95_max": 70.0,  "p99_max": 100.0},
    "lan":      {"p50_max": 100.0, "p95_max": 130.0, "p99_max": 180.0},
    "regional": {"p50_max": 200.0, "p95_max": 260.0, "p99_max": 320.0},
}[target]

checks = [
    ("qr_decode_rate", lambda v: v >= 0.95,
        f"qr_decode_rate must be >= 0.95 (cam setup); got %v"),
    ("negative_count", lambda v: v == 0.0,
        f"negative_count must be 0 (clock alignment); got %v"),
    ("min_ms",         lambda v: v > 0.0,
        f"min_ms must be strictly positive; got %v"),
    ("p50_ms",         lambda v: v <= bounds["p50_max"],
        f"p50_ms must be <= {bounds['p50_max']} for target=%s; got %v" % (target, "%v")),
    ("p95_ms",         lambda v: v <= bounds["p95_max"],
        f"p95_ms must be <= {bounds['p95_max']} for target=%s; got %v" % (target, "%v")),
    ("p99_ms",         lambda v: v <= bounds["p99_max"],
        f"p99_ms must be <= {bounds['p99_max']} for target=%s; got %v" % (target, "%v")),
]

ok = True
for key, pred, msg in checks:
    v = num(key, optional=(key in ("p99_ms",)))
    if v is None:
        print(f"  WARN  {key}: not reported (small sample?); skipping", file=sys.stderr)
        continue
    if not (isinstance(v, float) and pred(v)):
        line = msg.replace("%v", str(v))
        print(f"  FAIL  {key}={v}    {line}", file=sys.stderr)
        ok = False
    else:
        print(f"  OK    {key}={v}", file=sys.stderr)

if not ok and not no_assert:
    sys.exit(1)
PY

# ---- 3. record baseline JSON -------------------------------------------

step "3. write baseline JSON"

BASELINES_DIR="$REPO_ROOT/tests/harness/baselines"
mkdir -p "$BASELINES_DIR"

BASELINE_JSON="$BASELINES_DIR/phase1-$RUN_ID.json"

"$PYTHON" - "$SUMMARY" "$BASELINE_JSON" "$TARGET" "$NOTE" \
        "$CAM" "$JSONL" "$START_MS" "$OFFSET_MS" "$RUN_ID" <<'PY' || fail "baseline write failed"
import datetime as dt
import json, os, re, sys

(summary_path, baseline_path, target, note,
 cam, jsonl, start_ms, offset_ms, run_id) = sys.argv[1:10]

text = open(summary_path).read()
def num(key, default=None):
    m = re.search(rf"^{re.escape(key)}: (.+)$", text, re.M)
    if not m: return default
    raw = m.group(1).strip()
    try: return float(raw)
    except ValueError: return raw

baseline = {
    "schema_version": 1,
    "kind": "phase1-glass-to-glass",
    "run_id": run_id,
    "target": target,
    "recorded_at": dt.datetime.utcnow().isoformat() + "Z",
    "note": note,
    "inputs": {
        "cam": os.path.abspath(cam),
        "jsonl": os.path.abspath(jsonl),
        "start_epoch_ms": int(start_ms),
        "clock_offset_ms": float(offset_ms),
    },
    "numbers": {
        "frames_seen":     num("frames_seen"),
        "qr_decoded":      num("qr_decoded"),
        "qr_decode_rate":  num("qr_decode_rate"),
        "negative_count":  num("negative_count"),
        "min_ms":          num("min_ms"),
        "p50_ms":          num("p50_ms"),
        "p95_ms":          num("p95_ms"),
        "p99_ms":          num("p99_ms"),
        "max_ms":          num("max_ms"),
        "mean_ms":         num("mean_ms"),
        "stdev_ms":        num("stdev_ms"),
    },
}

with open(baseline_path, "w") as f:
    json.dump(baseline, f, indent=2, sort_keys=True)
    f.write("\n")
print(f"wrote {baseline_path}", file=sys.stderr)
PY

# Update phase1-latest.json -> phase1-<runId>.json (relative symlink so
# git tracks intent rather than absolute paths).
( cd "$BASELINES_DIR" && ln -sf "phase1-$RUN_ID.json" "phase1-latest.json" )
log "phase1-latest.json -> phase1-$RUN_ID.json"

# ---- done --------------------------------------------------------------

step "phase1 baseline recorded"
echo "[phase1-baseline] PASS" >&2
exit 0

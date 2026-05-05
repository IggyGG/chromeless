#!/usr/bin/env bash
#
# tests/run-regression.sh — T105 regression-suite orchestrator.
#
# Walks every layer in canonical order and emits a green/yellow/red
# summary at the end. Useful as a developer's pre-PR sanity check, or
# to walk the suite ahead of declaring v1 sign-off.
#
# Layers (per tests/regression-suite.md):
#   1. unit            — fast, hermetic, no Docker
#   2. integration     — Go toolchain only, no Docker
#   3. smoke           — Docker; Linux runners on CI
#   4. harness         — hermetic Python (opencv/pyzbar/numpy/matplotlib)
#   5. e2e             — full compose stack + Playwright
#
# Usage:
#   bash tests/run-regression.sh                  # everything (PR + Nightly)
#   bash tests/run-regression.sh --pr-only        # PR-blocking subset only
#   bash tests/run-regression.sh --nightly-only   # harness + e2e only
#
# Exit codes:
#   0 — every run layer GREEN
#   1 — at least one layer RED (real failure)
#   2 — at least one layer YELLOW (skipped because pre-req missing,
#       e.g. Docker daemon down, Python deps absent); no reds; consult
#       human. NOT the same as a green run for v1 sign-off purposes.
#
# Pre-requisites:
#   - Go toolchain (1.22+)
#   - Node 20+ (for the client unit tests + Playwright harness)
#   - python3 + harness deps (loopback baseline + reconciler)
#   - Docker daemon (for smoke + e2e layers)
#
# Skips behave deliberately (yellow, not red): if Docker is down on
# a developer's macOS box, we want the orchestrator to still report
# unit + integration + harness math, not fail the whole sequence.
# CI runs on Linux + Docker so the same script returns green.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT" || exit 2

# ---- arg parsing -------------------------------------------------------

MODE="all"
case "${1:-}" in
    "")               MODE="all" ;;
    --pr-only)        MODE="pr" ;;
    --nightly-only)   MODE="nightly" ;;
    -h|--help)        sed -n '3,32p' "$0"; exit 0 ;;
    *)                echo "unknown arg: $1 (use --pr-only / --nightly-only / -h)"; exit 2 ;;
esac

# ---- terminal colors ---------------------------------------------------

if [ -t 1 ] && command -v tput >/dev/null 2>&1; then
    GREEN="$(tput setaf 2)"
    YELLOW="$(tput setaf 3)"
    RED="$(tput setaf 1)"
    BOLD="$(tput bold)"
    DIM="$(tput dim)"
    RESET="$(tput sgr0)"
else
    GREEN=""; YELLOW=""; RED=""; BOLD=""; DIM=""; RESET=""
fi

# ---- result accounting -------------------------------------------------

declare -a RESULTS_NAME RESULTS_STATE RESULTS_NOTE RESULTS_DURATION
RUN_GROUPS_RED=0
RUN_GROUPS_YELLOW=0
RUN_GROUPS_GREEN=0

run_layer() {
    local name="$1"
    local skip_reason="$2"
    shift 2

    local start
    start=$(date +%s)
    printf "\n${BOLD}== %s ==${RESET}\n" "$name"

    if [ -n "$skip_reason" ]; then
        printf "${YELLOW}SKIP${RESET} %s — %s\n" "$name" "$skip_reason"
        RESULTS_NAME+=("$name")
        RESULTS_STATE+=("YELLOW")
        RESULTS_NOTE+=("$skip_reason")
        RESULTS_DURATION+=("0")
        RUN_GROUPS_YELLOW=$((RUN_GROUPS_YELLOW+1))
        return 0
    fi

    if "$@"; then
        local end
        end=$(date +%s)
        local dur=$((end - start))
        printf "${GREEN}PASS${RESET} %s (%ds)\n" "$name" "$dur"
        RESULTS_NAME+=("$name")
        RESULTS_STATE+=("GREEN")
        RESULTS_NOTE+=("")
        RESULTS_DURATION+=("$dur")
        RUN_GROUPS_GREEN=$((RUN_GROUPS_GREEN+1))
    else
        local rc=$?
        local end
        end=$(date +%s)
        local dur=$((end - start))
        printf "${RED}FAIL${RESET} %s (rc=%d, %ds)\n" "$name" "$rc" "$dur"
        RESULTS_NAME+=("$name")
        RESULTS_STATE+=("RED")
        RESULTS_NOTE+=("rc=$rc")
        RESULTS_DURATION+=("$dur")
        RUN_GROUPS_RED=$((RUN_GROUPS_RED+1))
    fi
}

# ---- pre-requisite probes ----------------------------------------------

probe_docker() {
    docker info >/dev/null 2>&1
}
probe_node() {
    command -v node >/dev/null 2>&1
}
probe_go() {
    command -v go >/dev/null 2>&1
}
probe_python_harness() {
    python3 -c 'import cv2, pyzbar.pyzbar, numpy, matplotlib' >/dev/null 2>&1
}
probe_playwright_browsers() {
    [ -d "$REPO_ROOT/tests/e2e/node_modules/playwright-core/.local-browsers" ] || \
    find "$HOME/Library/Caches/ms-playwright" -maxdepth 1 -name "chromium*" 2>/dev/null | head -1 | grep -q chromium || \
    find "$HOME/.cache/ms-playwright" -maxdepth 1 -name "chromium*" 2>/dev/null | head -1 | grep -q chromium
}

# ---- layer runners -----------------------------------------------------

run_unit() {
    local skip=""
    probe_go || skip="go toolchain not installed"
    [ -z "$skip" ] && probe_node || skip="${skip:-node not installed}"
    run_layer "unit (signaling+capture+controller+turn-issuer+client)" "$skip" \
        make test-unit
}

run_integration() {
    local skip=""
    probe_go || skip="go toolchain not installed"
    run_layer "integration (tests/integration, no Docker)" "$skip" \
        make test-integration
}

run_smoke() {
    local skip=""
    probe_docker || skip="docker daemon unreachable"
    run_layer "smoke (container-boot)" "$skip" \
        make test-smoke
}

run_harness() {
    local skip=""
    probe_python_harness || skip="python harness deps missing (cv2/pyzbar/numpy/matplotlib)"
    run_layer "harness baselines (hermetic)" "$skip" \
        make test-harness-all
}

run_e2e() {
    local skip=""
    probe_docker || skip="docker daemon unreachable"
    [ -z "$skip" ] && probe_node || skip="${skip:-node not installed}"
    [ -z "$skip" ] && probe_playwright_browsers || skip="${skip:-Playwright browsers not installed; run npx playwright install --with-deps chromium in tests/e2e}"
    run_layer "e2e (Playwright vs compose stack)" "$skip" \
        make test-e2e
}

# ---- driver ------------------------------------------------------------

START=$(date +%s)
printf "${BOLD}chromeless regression suite${RESET}\n"
printf "${DIM}mode=%s repo=%s${RESET}\n" "$MODE" "$REPO_ROOT"

case "$MODE" in
    all|pr)
        run_unit
        run_integration
        run_smoke
        ;;
esac
case "$MODE" in
    all|nightly)
        run_harness
        run_e2e
        ;;
esac

END=$(date +%s)
TOTAL_DUR=$((END - START))

# ---- summary -----------------------------------------------------------

printf "\n${BOLD}== regression summary ==${RESET}\n"
printf "%-58s  %-6s  %s\n" "layer" "status" "note"
printf "%-58s  %-6s  %s\n" "----------------------------------------------------------" "------" "----"
for i in "${!RESULTS_NAME[@]}"; do
    state="${RESULTS_STATE[$i]}"
    color=""
    case "$state" in
        GREEN)  color="$GREEN"  ;;
        YELLOW) color="$YELLOW" ;;
        RED)    color="$RED"    ;;
    esac
    dur="${RESULTS_DURATION[$i]}s"
    note="${RESULTS_NOTE[$i]}"
    [ "$state" = "GREEN" ] && note="$dur"
    printf "${color}%-58s  %-6s  %s${RESET}\n" \
        "${RESULTS_NAME[$i]}" "$state" "$note"
done
printf "\n${BOLD}totals:${RESET}  ${GREEN}%d green${RESET}  ${YELLOW}%d yellow${RESET}  ${RED}%d red${RESET}  (%ds wall)\n" \
    "$RUN_GROUPS_GREEN" "$RUN_GROUPS_YELLOW" "$RUN_GROUPS_RED" "$TOTAL_DUR"

if [ "$RUN_GROUPS_RED" -gt 0 ]; then
    printf "${RED}REGRESSION SUITE: RED${RESET}\n"
    exit 1
elif [ "$RUN_GROUPS_YELLOW" -gt 0 ]; then
    printf "${YELLOW}REGRESSION SUITE: YELLOW (skips present; not the same as green)${RESET}\n"
    exit 2
fi

printf "${GREEN}REGRESSION SUITE: GREEN${RESET}\n"
exit 0

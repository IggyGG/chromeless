#!/usr/bin/env bash
# tests/smoke/snapshot-restore.sh — round-trip smoke for T68. Snapshot
# a fresh container, restore it, assert Chromium is responsive within
# 2s of restore.
#
# Linux-only. Requires CRIU + root. The smoke is a thin wrapper around
# infra/snapshots/{snapshot,restore}.sh; the real assertions live in
# those scripts. We just check the round-trip succeeds and parse the
# `time_to_ready_ms` line from restore.sh's stdout.
#
# Usage:
#   sudo ./tests/smoke/snapshot-restore.sh
#
# Env (with defaults):
#   READY_BUDGET_MS  2000   restore must report at-or-below this
#
# Exit codes:
#   0  PASS
#   1  FAIL (round-trip failed or time-to-ready exceeded budget)
#   2  ENV (missing tool or not root)

set -euo pipefail

READY_BUDGET_MS="${READY_BUDGET_MS:-2000}"

REPO_ROOT="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
SNAPSHOT_SH="$REPO_ROOT/infra/snapshots/snapshot.sh"
RESTORE_SH="$REPO_ROOT/infra/snapshots/restore.sh"

log()  { printf '[snapshot-smoke] %s\n' "$*" >&2; }
fail() { log "FATAL: $*"; exit "${2:-1}"; }

[ "$(id -u)" -eq 0 ] || fail "must run as root (CRIU)" 2
[ -x "$SNAPSHOT_SH" ] || fail "missing $SNAPSHOT_SH" 2
[ -x "$RESTORE_SH" ]  || fail "missing $RESTORE_SH" 2

log "1. snapshot a fresh container"
SHA=$("$SNAPSHOT_SH" chromeless-snapshot-smoke)
log "   sha = $SHA"

log "2. restore from sha"
RESTORE_OUT=$("$RESTORE_SH" "$SHA")
log "   restore output:"
printf '%s\n' "$RESTORE_OUT" | sed 's/^/     /' >&2

TTR_MS=$(echo "$RESTORE_OUT" | awk -F= '/^time_to_ready_ms=/ { print $2 }')
if [ -z "$TTR_MS" ]; then
    fail "could not parse time_to_ready_ms from restore output"
fi

log "time-to-ready: ${TTR_MS} ms (budget: ${READY_BUDGET_MS} ms)"
if [ "$TTR_MS" -gt "$READY_BUDGET_MS" ]; then
    fail "time-to-ready ${TTR_MS}ms exceeds budget ${READY_BUDGET_MS}ms"
fi

log "PASS: snapshot + restore round-trip in ${TTR_MS}ms"

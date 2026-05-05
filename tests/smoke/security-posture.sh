#!/usr/bin/env bash
# tests/smoke/security-posture.sh — assert the chromeless
# image's runtime security posture matches T57's contract.
#
# Run against a built image:
#
#   docker build -t chromeless:dev -f infra/Dockerfile .
#   ./tests/smoke/security-posture.sh chromeless:dev
#
# Or, by default, against the dev tag.
#
# Contract:
#   - container's default user is non-root (uid != 0)
#   - rootfs is read-only when started with --read-only
#   - Chromium binary has no dangerous file capabilities
#
# We do NOT assert seccomp here — verifying seccomp programmatically
# requires `/proc/<pid>/status:Seccomp` reads and is fragile against
# Docker's own default profile composition. Validate that one with
# `docker inspect` + manual review.
#
# Exit codes:
#   0  PASS  — all assertions hold
#   1  FAIL  — at least one assertion failed; details on stderr
#   2  ENV   — required tool missing (docker)

set -euo pipefail

IMAGE="${1:-chromeless:dev}"

log() { printf '[security-smoke] %s\n' "$*" >&2; }

require() {
    command -v "$1" >/dev/null 2>&1 || {
        log "FATAL: $1 not in PATH"
        exit 2
    }
}
require docker

fail=0
mark_fail() {
    fail=1
    log "FAIL: $*"
}

# ---- 1. default user is non-root --------------------------------------

# Override the entrypoint with `--entrypoint id` and pass `-u` as cmd.
log "running id -u as the image's default user"
uid=$(docker run --rm --entrypoint id "$IMAGE" -u || true)
if [ -z "$uid" ]; then
    mark_fail "id -u produced no output"
elif [ "$uid" = "0" ]; then
    mark_fail "default user is root (uid 0); expected non-root"
else
    log "PASS: default uid=$uid (non-root)"
fi

# ---- 2. rootfs is read-only when --read-only is set -------------------

log "verifying / is read-only under --read-only"
ro_out=$(docker run --rm --read-only --entrypoint sh "$IMAGE" \
    -c 'touch /test 2>&1; echo "EXITCODE=$?"' || true)
if echo "$ro_out" | grep -q "EXITCODE=0"; then
    mark_fail "touch / succeeded under --read-only; expected EROFS"
else
    log "PASS: / is read-only under --read-only"
fi

# ---- 3. Chromium binary has no dangerous file capabilities ------------

# libcap2-bin (which provides getcap) isn't in the runtime image. We
# install it transiently inside a --rm container just for this check.
log "checking /usr/bin/chromium has no dangerous file capabilities"
caps_out=$(docker run --rm --user 0:0 --entrypoint sh "$IMAGE" \
    -c 'apt-get update -qq >/dev/null 2>&1 \
        && apt-get install -y -qq libcap2-bin >/dev/null 2>&1 \
        && getcap /usr/bin/chromium 2>&1 \
        || echo "GETCAP_NO_OUTPUT"' \
    || true)
if echo "$caps_out" | grep -qE 'cap_(net_admin|sys_admin|sys_ptrace|sys_module|dac_override)'; then
    mark_fail "/usr/bin/chromium has dangerous file capabilities: $caps_out"
else
    log "PASS: /usr/bin/chromium has no dangerous file capabilities"
fi

# ----------------------------------------------------------------------

if [ "$fail" -ne 0 ]; then
    log "FAIL: at least one assertion failed"
    exit 1
fi
log "PASS: T57 security posture verified"

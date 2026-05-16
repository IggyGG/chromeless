#!/usr/bin/env bash
#
# verification/scripts/with-container.sh — R7 shell wrapper.
#
# Boots a chromeless container, exports CONTAINER_ID, runs "$@", tears down
# on exit (success OR failure). Mirrors tests/smoke/container-boot.sh boot
# skeleton; assertions #3/#4 (R5/R6) can use this when they want to script
# bash-side rather than driving the Node harness.
#
# Usage:
#   verification/scripts/with-container.sh -- <cmd> [args...]
#   verification/scripts/with-container.sh --image chromeless:ci -- ls /
#
# Env:
#   CHROMELESS_IMAGE   image tag to boot (default: chromeless:ci)
#   BOOT_TIMEOUT_S     seconds to wait for DevTools (default: 60)
#
# Exit code: forwards the wrapped command's exit code, or non-zero on boot
# failure.

set -euo pipefail

IMAGE="${CHROMELESS_IMAGE:-chromeless:ci}"
TIMEOUT_S="${BOOT_TIMEOUT_S:-60}"

while [ $# -gt 0 ]; do
    case "$1" in
        --image)
            IMAGE="$2"; shift 2;;
        --image=*)
            IMAGE="${1#--image=}"; shift;;
        --timeout)
            TIMEOUT_S="$2"; shift 2;;
        --timeout=*)
            TIMEOUT_S="${1#--timeout=}"; shift;;
        --)
            shift; break;;
        *)
            break;;
    esac
done

if [ $# -eq 0 ]; then
    echo "usage: with-container.sh [--image TAG] [--timeout SECS] -- CMD [ARGS...]" >&2
    exit 2
fi

if ! command -v docker >/dev/null 2>&1; then
    echo "with-container.sh: docker not found in PATH" >&2
    exit 127
fi

NAME="chromeless-harness-$(date +%s)-$$"
CONTAINER_ID=""

cleanup() {
    if [ -n "$CONTAINER_ID" ]; then
        docker rm -f "$CONTAINER_ID" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "with-container.sh: image not present: $IMAGE" >&2
    exit 1
fi

CONTAINER_ID=$(docker run --rm -d \
    --name "$NAME" \
    -e IDLE_TIMEOUT_S=999999 \
    --shm-size=1g \
    "$IMAGE")

# Poll DevTools readiness in-netns.
ready=0
for _ in $(seq 1 "$TIMEOUT_S"); do
    if docker exec "$CONTAINER_ID" curl -fsS \
            http://127.0.0.1:9222/json/version >/dev/null 2>&1; then
        ready=1
        break
    fi
    if ! docker inspect -f '{{.State.Running}}' "$CONTAINER_ID" 2>/dev/null | grep -q true; then
        echo "with-container.sh: container exited before DevTools came up" >&2
        docker logs --tail 80 "$CONTAINER_ID" >&2 || true
        exit 1
    fi
    sleep 1
done

if [ "$ready" != "1" ]; then
    echo "with-container.sh: DevTools did not respond within ${TIMEOUT_S}s" >&2
    exit 1
fi

export CONTAINER_ID
"$@"

#!/usr/bin/env bash
# Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
#
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# harness/m4-uat/run_uat.sh — bash orchestrator for the M4 functional
# UAT harness (CV2-49). DRAFT — see ./README.md for the run contract.
#
# Three responsibilities, in this order:
#   1. Boot (optional) — start a chromeless:ci container if --skip-boot
#      isn't passed. Reuses verification/scripts/with-container.sh from
#      the M0 gate harness so the boot semantics stay aligned.
#   2. Page — serve fixtures/uat-page.html on a local port and arrange
#      for the native Chromium in the container to navigate to it.
#   3. Drive — invoke uat.mjs with the resolved CDP URL and scenario
#      selection, then propagate its exit code.
#
# TODO(M4-R9-boot-wiring): when M3 R5's consumer-bind API stabilises a
# CDP port surface for this harness, replace the hard-coded 9222 with
# whatever M3 settles on. For now the assumption mirrors the M0
# native-peer.mjs default.

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
REPO_ROOT="$( cd "${SCRIPT_DIR}/../.." && pwd )"

IMAGE="chromeless:ci"
CDP_URL=""
SKIP_BOOT="false"
SCENARIO=""
MODE="scaffold"   # scaffold | strict
PAGE_PORT="8765"

print_usage() {
  cat <<EOF
usage: $0 [--image=TAG] [--cdp=URL] [--skip-boot]
          [--scenario=NAME] [--strict] [--page-port=PORT]

  --image=TAG       chromeless container image to boot (default: chromeless:ci)
  --cdp=URL         pre-running CDP endpoint (implies --skip-boot)
  --skip-boot       don't boot a container; expect --cdp= to be set
  --scenario=NAME   run only the named scenario
                    (mouse|keyboard|ime|touch|drag|clipboard|pointer-leave)
  --strict          every scenario must PASS (default: scaffold permits NYI)
  --page-port=PORT  port for the fixture page server (default: 8765)
  -h, --help        show this help
EOF
}

for arg in "$@"; do
  case "$arg" in
    --image=*)     IMAGE="${arg#--image=}" ;;
    --cdp=*)       CDP_URL="${arg#--cdp=}"; SKIP_BOOT="true" ;;
    --skip-boot)   SKIP_BOOT="true" ;;
    --scenario=*)  SCENARIO="${arg#--scenario=}" ;;
    --strict)      MODE="strict" ;;
    --page-port=*) PAGE_PORT="${arg#--page-port=}" ;;
    -h|--help)     print_usage; exit 0 ;;
    *)             echo "unknown argument: $arg" >&2; print_usage >&2; exit 2 ;;
  esac
done

# --- 1. Serve the fixture page ----------------------------------------------
# A long-running `python3 -m http.server` on PAGE_PORT, backgrounded. We
# use python because every chromeless image already has it for the
# existing latency harnesses; bringing in node:http here would add a
# second runtime to the container.
#
# TODO(M4-R9-static-server): swap to a node static handler if the page
# needs anything beyond static files (e.g. a WebSocket sink for diag).

PAGE_PID=""
cleanup() {
  if [[ -n "$PAGE_PID" ]]; then
    kill "$PAGE_PID" 2>/dev/null || true
    wait "$PAGE_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

(
  cd "${SCRIPT_DIR}/fixtures"
  python3 -m http.server "$PAGE_PORT" --bind 127.0.0.1 \
    >/tmp/m4-uat-page-${PAGE_PORT}.log 2>&1 &
  echo "$!" > /tmp/m4-uat-page-${PAGE_PORT}.pid
)
PAGE_PID="$(cat /tmp/m4-uat-page-${PAGE_PORT}.pid)"

# Wait for the server to actually accept connections — poll for up to 5s.
for _ in $(seq 1 50); do
  if curl -sf -o /dev/null "http://127.0.0.1:${PAGE_PORT}/uat-page.html"; then
    break
  fi
  sleep 0.1
done

# --- 2. Boot (or reuse) the container ---------------------------------------

if [[ "$SKIP_BOOT" == "true" ]]; then
  if [[ -z "$CDP_URL" ]]; then
    echo "--skip-boot requires --cdp=URL" >&2
    exit 2
  fi
else
  # Reuse the M0 container-boot harness — it gives us a CDP URL on
  # stdout in a known shape. TODO(M4-R9-with-container-arg): plumb
  # --image through; with-container.sh today reads IMAGE from env.
  export IMAGE
  source "${REPO_ROOT}/verification/scripts/with-container.sh"
  CDP_URL="${CB_CDP_URL:?with-container.sh did not export CB_CDP_URL}"
fi

# --- 3. Drive the gate ------------------------------------------------------

NODE_ARGS=( "${SCRIPT_DIR}/uat.mjs"
            "--cdp=${CDP_URL}"
            "--image=${IMAGE}"
            "--page-url=http://127.0.0.1:${PAGE_PORT}/uat-page.html"
            "--mode=${MODE}" )
if [[ -n "$SCENARIO" ]]; then
  NODE_ARGS+=( "--scenario=${SCENARIO}" )
fi

# uat.mjs handles its own stdout-is-JSON / stderr-is-progress split.
# Propagate its exit code unchanged.
exec node "${NODE_ARGS[@]}"

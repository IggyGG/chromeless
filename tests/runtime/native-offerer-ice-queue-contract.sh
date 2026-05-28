#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
driver_cc="$repo_root/capture/signaling/cb_offerer_driver.cc"
driver_h="$repo_root/capture/signaling/cb_offerer_driver.h"

require_grep() {
  local pattern="$1"
  local file="$2"
  local desc="$3"
  if ! grep -Eq "$pattern" "$file"; then
    echo "[native-offerer-ice-queue-contract] missing: $desc" >&2
    echo "  pattern: $pattern" >&2
    echo "  file: $file" >&2
    exit 1
  fi
}

reject_grep() {
  local pattern="$1"
  local file="$2"
  local desc="$3"
  if grep -Eq "$pattern" "$file"; then
    echo "[native-offerer-ice-queue-contract] forbidden: $desc" >&2
    echo "  pattern: $pattern" >&2
    echo "  file: $file" >&2
    exit 1
  fi
}

require_grep 'pending_remote_ice_' "$driver_h" \
  "CbOffererDriver retains ICE that arrives before the remote answer is applied"
require_grep 'QueueRemoteIceCandidate' "$driver_h" \
  "queue helper is declared"
require_grep 'FlushPendingRemoteIce' "$driver_h" \
  "flush helper is declared"
require_grep 'QueueRemoteIceCandidate\(env\);' "$driver_cc" \
  "HandleIceEnvelope queues early inbound ICE instead of dropping it"
require_grep 'FlushPendingRemoteIce\(\);' "$driver_cc" \
  "SetRemoteDescription completion drains queued inbound ICE"
reject_grep 'inbound ICE before answer; dropping' "$driver_cc" \
  "native offerer must not drop portal ICE during the answer-apply race"

echo "[native-offerer-ice-queue-contract] OK"

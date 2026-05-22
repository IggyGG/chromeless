#!/usr/bin/env bash
# Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
#
# run_r5_teardown_parity.sh — CV2-82 M5.5 R5 verification wrapper.
#
# R5 verdict (per /tmp/cv2-82-pulse-env-design.md Phase 3.2):
#
#   PASS-shape: `pactl list short clients` pre-teardown shows >= 1
#               cb-chromium-attributed client connected to Pulse;
#               same probe POST-teardown shows ZERO cb-chromium clients.
#
#   FAIL-class:
#     (a) no cb-chromium client pre-teardown        -> ADM Init never
#                                                      attached (R1 didn't
#                                                      pass; rerun R1)
#     (b) cb-chromium client persists post-teardown -> teardown parity
#                                                      regression
#                                                      (CV2-32 territory)
#
# Surface choice — out-of-pod kubectl exec (production-shape):
#
#   Under Option B (production-shape supervisord), Pulse and cb-chromium
#   live in the same container; killing cb-chromium leaves Pulse running
#   for the rest of the pod's lifetime (Pulse priority=20, autorestart=true,
#   but supervisord keeps it alive as long as the container is alive).
#   The harness exec's `pactl list short clients` BEFORE and AFTER asking
#   supervisord to stop only the chromium program, leaving Pulse to answer
#   the post-teardown probe.
#
#   The design doc Phase 3.2 also documents an Option A sidecar variant
#   (Pulse in a separate container) that would survive cb-chromium teardown
#   independently of supervisord; that variant is documented in the design
#   doc as a fallback if the production-shape Option B path proves
#   insufficient. CV2-82 ships Option B as canonical.
#
# Usage:
#   harness/m5.5/run_r5_teardown_parity.sh --pod cv2-82-real-pulse \
#       [--namespace chromeless] \
#       [--settle-seconds 5]
#
# Exit shape (mirrors harness/m5.5/run_red_test.sh):
#   0  -> PASS
#   1  -> FAIL
#   77 -> SKIPPED (e.g. stub variant; no Pulse to query)

set -euo pipefail

KUBECTL="${KUBECTL_BIN:-kubectl}"
POD=""
NAMESPACE="chromeless"
SETTLE_SECONDS=5
PULSE_SERVER_VALUE="${PULSE_SERVER_VALUE:-unix:/run/user/1000/pulse/native}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pod)            POD="$2";            shift 2 ;;
    --namespace)      NAMESPACE="$2";      shift 2 ;;
    --settle-seconds) SETTLE_SECONDS="$2"; shift 2 ;;
    -h|--help)
      sed -n '1,42p' "$0"
      exit 0
      ;;
    *)
      echo "ERROR: unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

if [[ -z "${POD}" ]]; then
  echo "ERROR: --pod is required" >&2
  exit 2
fi

emit_verdict() {
  local verdict="$1"
  local reason="$2"
  printf 'M55-R5-VERDICT {"verdict":"%s","pod":"%s","variant":"%s","reason":%s,"observed":{"preTeardownClients":%d,"postTeardownClients":%d}}\n' \
    "${verdict}" \
    "${POD}" \
    "${VARIANT:-unknown}" \
    "$(printf '%s' "${reason}" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))')" \
    "${PRE_COUNT:-0}" \
    "${POST_COUNT:-0}"
}

# Resolve variant.
err=$(mktemp)
VARIANT="$("${KUBECTL}" -n "${NAMESPACE}" get pod "${POD}" \
  -o "jsonpath={.metadata.labels.cv2-82\.variant}" 2>"${err}" || true)"
rm -f "${err}"

if [[ "${VARIANT}" == "stub" ]]; then
  PRE_COUNT=0; POST_COUNT=0
  emit_verdict "SKIPPED" "stub variant has no Pulse"
  exit 77
fi

err=$(mktemp)
PHASE="$("${KUBECTL}" -n "${NAMESPACE}" get pod "${POD}" \
  -o "jsonpath={.status.phase}" 2>"${err}" || true)"
rm -f "${err}"
if [[ "${PHASE}" != "Running" ]]; then
  PRE_COUNT=0; POST_COUNT=0
  emit_verdict "SKIPPED" "pod phase=${PHASE} (need Running)"
  exit 77
fi

count_cb_clients() {
  # cb-chromium attaches to Pulse via the libwebrtc PulseAudio module;
  # client name is typically "chromium" / "Chromium" / "cb_chromium".
  # Use case-insensitive grep across the multi-column output of
  # `pactl list short clients`.
  local out rc
  err=$(mktemp)
  out="$("${KUBECTL}" -n "${NAMESPACE}" exec "${POD}" -c cb-chromium -- \
    env PULSE_SERVER="${PULSE_SERVER_VALUE}" pactl list short clients 2>"${err}")" || rc=$? && rc=${rc:-0}
  local stderr_text
  stderr_text=$(cat "${err}")
  rm -f "${err}"
  if [[ "${rc}" -ne 0 ]]; then
    echo "ERROR: pactl list short clients rc=${rc}: ${stderr_text}" >&2
    return 1
  fi
  # Match common chromium / cb_audio_device_module client names. Chromium 147's
  # libwebrtc Pulse client currently reports application.name="WEBRTC
  # VoiceEngine" and application.process.binary="chromeless"; keep the older
  # names as accepted aliases for previous rv images.
  printf '%s\n' "${out}" | grep -ciE 'chromium|cb_audio|cb_chromium|chromeless|WEBRTC VoiceEngine' || true
}

# Pre-teardown count.
if ! PRE_COUNT="$(count_cb_clients)"; then
  PRE_COUNT=0; POST_COUNT=0
  emit_verdict "FAIL" "pre-teardown pactl list short clients failed"
  exit 1
fi

if [[ "${PRE_COUNT}" -eq 0 ]]; then
  POST_COUNT=0
  emit_verdict "FAIL" \
    "(a) no cb-chromium client pre-teardown -> ADM Init never attached (rerun R1)"
  exit 1
fi

# Ask supervisord to stop only the chromium program. The cb-chromium
# container stays alive because supervisord is still PID 1's child;
# Pulse priority=20 remains running and can answer the post-teardown probe.
err=$(mktemp)
"${KUBECTL}" -n "${NAMESPACE}" exec "${POD}" -c cb-chromium -- \
  supervisorctl -c /etc/supervisor/supervisord.conf stop chromium \
  >/dev/null 2>"${err}" || STOP_RC=$? && STOP_RC=${STOP_RC:-0}
STOP_STDERR=$(cat "${err}")
rm -f "${err}"

if [[ "${STOP_RC}" -ne 0 ]]; then
  POST_COUNT=0
  emit_verdict "FAIL" \
    "supervisorctl stop chromium failed (rc=${STOP_RC}): ${STOP_STDERR}"
  exit 1
fi

# Let Pulse notice the disconnect. libwebrtc's PulseAudio module
# disconnects on Terminate(); supervisord's killasgroup=true ensures the
# whole Chromium process tree dies; Pulse's client list updates within
# its event loop tick.
sleep "${SETTLE_SECONDS}"

# Post-teardown count.
if ! POST_COUNT="$(count_cb_clients)"; then
  emit_verdict "FAIL" "post-teardown pactl list short clients failed"
  exit 1
fi

if [[ "${POST_COUNT}" -gt 0 ]]; then
  emit_verdict "FAIL" \
    "(b) cb-chromium client persists post-teardown -> teardown parity regression"
  exit 1
fi

emit_verdict "PASS" "client count ${PRE_COUNT} -> ${POST_COUNT} across teardown"
exit 0

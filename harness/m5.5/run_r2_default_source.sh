#!/usr/bin/env bash
# Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
#
# run_r2_default_source.sh — CV2-82 M5.5 R2 verification wrapper.
#
# R2 verdict (per /tmp/cv2-82-pulse-env-design.md Phase 3.1):
#
#   PASS-shape: `pactl info` reports `Default Source: cb_capture.monitor`
#               AND `pactl list short sources` lists cb_capture.monitor as
#               non-suspended.
#
#   FAIL-class:
#     (a) wrong default source       -> pulse-default.pa not loaded /
#                                       set-default-source line missing
#     (b) suspended source           -> no playback driving the monitor
#                                       (real-pulse variant module-sine
#                                        postStart did not fire — check
#                                        kubectl describe pod ... events)
#     (c) absent source              -> null-sink module didn't load
#                                       (.fail in pulse-default.pa should
#                                        have crashed the daemon; check
#                                        /var/log/supervisor/pulseaudio.err.log)
#
# This verdict grounds entirely on the real-pulse (and dummy) variants.
# stub variant has no Pulse and is intentionally excluded — invocation
# against stub returns SKIPPED (exit 77).
#
# Usage:
#   harness/m5.5/run_r2_default_source.sh --pod cv2-82-real-pulse \
#       [--namespace chromeless]
#
# Exit shape (mirrors harness/m5.5/run_red_test.sh):
#   0  -> PASS
#   1  -> FAIL
#   77 -> SKIPPED (e.g. stub variant)
#
# The verdict line is emitted as a single JSON object prefixed with
# "M55-R2-VERDICT" on stdout, then the exit code.

set -euo pipefail

KUBECTL="${KUBECTL_BIN:-kubectl}"
POD=""
NAMESPACE="chromeless"
PULSE_SERVER_VALUE="${PULSE_SERVER_VALUE:-unix:/run/user/1000/pulse/native}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pod)
      POD="$2"
      shift 2
      ;;
    --namespace)
      NAMESPACE="$2"
      shift 2
      ;;
    -h|--help)
      sed -n '1,32p' "$0"
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
  # $1=verdict $2=reason
  local verdict="$1"
  local reason="$2"
  local default_src="${OBSERVED_DEFAULT_SRC:-}"
  local suspended="${OBSERVED_SUSPENDED:-}"
  printf 'M55-R2-VERDICT {"verdict":"%s","pod":"%s","variant":"%s","reason":%s,"observed":{"defaultSource":%s,"suspended":%s}}\n' \
    "${verdict}" \
    "${POD}" \
    "${VARIANT:-unknown}" \
    "$(printf '%s' "${reason}" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))')" \
    "$(printf '%s' "${default_src}" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))')" \
    "$(printf '%s' "${suspended}" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))')"
}

# Resolve variant from pod label.
err=$(mktemp)
VARIANT="$("${KUBECTL}" -n "${NAMESPACE}" get pod "${POD}" \
  -o "jsonpath={.metadata.labels.cv2-82\.variant}" 2>"${err}" || true)"
rm -f "${err}"

if [[ "${VARIANT}" == "stub" ]]; then
  emit_verdict "SKIPPED" "stub variant has no Pulse"
  exit 77
fi

# Pod must be Running.
err=$(mktemp)
PHASE="$("${KUBECTL}" -n "${NAMESPACE}" get pod "${POD}" \
  -o "jsonpath={.status.phase}" 2>"${err}" || true)"
rm -f "${err}"
if [[ "${PHASE}" != "Running" ]]; then
  emit_verdict "SKIPPED" "pod phase=${PHASE} (need Running)"
  exit 77
fi

# pactl info: capture the "Default Source:" line.
# stderr separation per CLAUDE.md "Bash + psql trap" pattern (same trap
# applies to any kubectl-exec that may emit info chatter to stderr).
err=$(mktemp)
INFO_OUT="$("${KUBECTL}" -n "${NAMESPACE}" exec "${POD}" -c cb-chromium -- \
  env PULSE_SERVER="${PULSE_SERVER_VALUE}" pactl info 2>"${err}")" || INFO_RC=$? && INFO_RC=${INFO_RC:-0}
INFO_STDERR=$(cat "${err}")
rm -f "${err}"

if [[ "${INFO_RC}" -ne 0 ]]; then
  emit_verdict "FAIL" "pactl info failed (rc=${INFO_RC}): ${INFO_STDERR}"
  exit 1
fi

OBSERVED_DEFAULT_SRC="$(printf '%s\n' "${INFO_OUT}" | awk -F': ' '/^Default Source:/ {print $2; exit}')"

if [[ -z "${OBSERVED_DEFAULT_SRC}" ]]; then
  emit_verdict "FAIL" "pactl info: 'Default Source:' line not present"
  exit 1
fi

if [[ "${OBSERVED_DEFAULT_SRC}" != "cb_capture.monitor" ]]; then
  emit_verdict "FAIL" \
    "(a) wrong default source -> pulse-default.pa not loaded or set-default-source missing"
  exit 1
fi

# pactl list short sources: locate cb_capture.monitor and inspect its
# RUNNING/SUSPENDED state column (typically column 5).
err=$(mktemp)
LIST_OUT="$("${KUBECTL}" -n "${NAMESPACE}" exec "${POD}" -c cb-chromium -- \
  env PULSE_SERVER="${PULSE_SERVER_VALUE}" pactl list short sources 2>"${err}")" || LIST_RC=$? && LIST_RC=${LIST_RC:-0}
LIST_STDERR=$(cat "${err}")
rm -f "${err}"

if [[ "${LIST_RC}" -ne 0 ]]; then
  emit_verdict "FAIL" "pactl list short sources failed (rc=${LIST_RC}): ${LIST_STDERR}"
  exit 1
fi

CB_CAPTURE_LINE="$(printf '%s\n' "${LIST_OUT}" | awk '/cb_capture\.monitor/ {print; exit}')"

if [[ -z "${CB_CAPTURE_LINE}" ]]; then
  emit_verdict "FAIL" "(c) absent source -> cb_capture.monitor not in pactl list short sources"
  exit 1
fi

# State is the last column (RUNNING / IDLE / SUSPENDED). For the real-pulse
# variant we expect RUNNING (module-sine drives the loopback); for dummy
# we accept IDLE (no stimulus). SUSPENDED is the failure case.
STATE="$(printf '%s' "${CB_CAPTURE_LINE}" | awk '{print $NF}')"
OBSERVED_SUSPENDED="$STATE"

if [[ "${STATE}" == "SUSPENDED" ]]; then
  emit_verdict "FAIL" "(b) suspended source -> no playback driving the monitor"
  exit 1
fi

# real-pulse variant: prefer RUNNING; warn on IDLE (postStart module-sine
# may not have fired). dummy variant: IDLE is the expected state.
if [[ "${VARIANT}" == "real-pulse" && "${STATE}" != "RUNNING" ]]; then
  emit_verdict "FAIL" \
    "(b) real-pulse variant: cb_capture.monitor state=${STATE}, expected RUNNING (postStart module-sine?)"
  exit 1
fi

emit_verdict "PASS" "default source=cb_capture.monitor, state=${STATE}"
exit 0

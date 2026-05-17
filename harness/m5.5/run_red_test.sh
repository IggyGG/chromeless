#!/usr/bin/env bash
# Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
#
# run_red_test.sh — M5.5 R2 (CV2-29) harness side.
#
# Wraps the cloud_browser_adm_unittests binary with the paplay-driven
# tone the iron test (MonitorSourceDeliversToneRms) gates on. This
# script is the OTHER half of the separation-of-concerns named in the
# test-fixture file header: the harness drives audio, the test
# measures it.
#
# Contract:
#   * Run AS the cb-chromium container's init order: PulseAudio is up,
#     infra/pulse-default.pa has been loaded, cb_capture.monitor is the
#     server-default source. If any of those preconditions don't hold,
#     this script fails LOUDLY rather than running the test against a
#     broken environment (which would produce a misleading RED for the
#     wrong reason).
#   * Before invoking the test binary, start a paplay subprocess that
#     emits a known 1 kHz sine wave at -12 dBFS into cb_capture for
#     the duration of the test window.
#   * Set CB_M55_R2_TONE_RUNNING=1 in the test binary's environment so
#     MonitorSourceDeliversToneRms runs (else it SKIPs — see test file
#     header for the contract).
#   * Capture the M55-R2-VERDICT JSON line emitted on the test
#     binary's stdout and surface it as the script's exit shape:
#     verdict=PASS → exit 0; verdict=FAIL → exit 1; verdict=SKIPPED
#     across all three tests → exit 77 (autotools "skipped" sentinel,
#     interpreted as SKIPPED-for-verdict by the M0 R2 gate scaffold).
#   * Stop the paplay subprocess on script exit regardless of test
#     outcome (trap EXIT) — leaving a tone running across the M0 gate
#     would pollute subsequent assertions.
#
# Not done here (intentional):
#   * Generating the sine WAV — pre-baked into the container image at
#     build time (see infra/m5.5-tone.wav, TODO below). Inlining a
#     sox / gst command here would couple the harness to whichever
#     audio-tools package the container happens to ship.
#   * Driving CB_M55_R2_TONE_RUNNING from outside the script — the
#     env var is THIS script's contract with the test binary, not a
#     general-purpose toggle.
#
# Usage:
#   harness/m5.5/run_red_test.sh /path/to/cloud_browser_adm_unittests
#
# Caller is the M0 R7 in-netns container-boot harness, which dispatches
# this script after PulseAudio + Xvfb come up. See harness/<...>.

set -euo pipefail

# ---------------------------------------------------------------------
# Arg parse + preflight.
# ---------------------------------------------------------------------

if [[ "$#" -lt 1 ]]; then
  echo "usage: $0 /path/to/cloud_browser_adm_unittests" >&2
  exit 2
fi
TEST_BIN="$1"

if [[ ! -x "$TEST_BIN" ]]; then
  echo "harness: test binary not executable: $TEST_BIN" >&2
  exit 2
fi

# TODO(M55-R2-tone-wav): place a pre-baked 10 s, 1 kHz, -12 dBFS WAV
# in the container image and reference it by absolute path. For the
# DRAFT, the path below is a placeholder — the M0 R7 harness image
# bake recipe will need a small addition (sox-generated sine,
# committed under infra/audio/ once a path is decided).
TONE_WAV="${CB_M55_R2_TONE_WAV:-/usr/share/cloud-browser/m5.5-tone.wav}"
if [[ ! -f "$TONE_WAV" ]]; then
  echo "harness: tone WAV missing: $TONE_WAV" >&2
  echo "       (set CB_M55_R2_TONE_WAV or pre-bake at /usr/share/cloud-browser/)" >&2
  exit 2
fi

# Confirm PulseAudio is up. `pactl info` exits 0 only when it can talk
# to a server; the explicit check makes a missing-Pulse failure mode
# show up here, not as a confusing test-time nullptr in
# SmokeAdmConstructible.
if ! pactl info >/dev/null 2>&1; then
  echo "harness: pactl info failed — PulseAudio not reachable" >&2
  exit 2
fi

# Confirm cb_capture exists as a sink and its monitor is the server-
# default source. If pulse-default.pa didn't run (or the source name
# drifted from cb_capture.monitor), we want to know NOW rather than
# emit a hard-to-interpret RED later.
DEFAULT_SOURCE="$(pactl info | awk -F': ' '/Default Source/ {print $2}')"
if [[ "$DEFAULT_SOURCE" != "cb_capture.monitor" ]]; then
  echo "harness: default source = '$DEFAULT_SOURCE' (expected cb_capture.monitor)" >&2
  echo "         pulse-default.pa may not have loaded; see infra/pulse-default.pa" >&2
  exit 2
fi

# ---------------------------------------------------------------------
# Tone start + cleanup trap.
# ---------------------------------------------------------------------

PAPLAY_PID=""
# shellcheck disable=SC2317  # bash trap dispatch is dynamic.
cleanup() {
  if [[ -n "$PAPLAY_PID" ]] && kill -0 "$PAPLAY_PID" 2>/dev/null; then
    kill "$PAPLAY_PID" 2>/dev/null || true
    wait "$PAPLAY_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

# Drive the tone INTO cb_capture (the sink, NOT cb_capture.monitor).
# The monitor is the *output* of the sink — paplay-ing to the monitor
# directly is rejected by PulseAudio. paplay --device=cb_capture
# routes audio into the sink, which the monitor source then mirrors,
# which the libwebrtc ADM (bound to server-default = cb_capture.monitor)
# captures. This is the routing the iron test asserts works.
paplay --device=cb_capture "$TONE_WAV" &
PAPLAY_PID=$!

# Give Pulse a moment to start mixing the tone — paplay returns
# immediately, but the first ~50 ms can be silence while the stream
# negotiates. The test's RMS-over-9s window comfortably absorbs a
# 200 ms head start; we don't need anything tighter here.
sleep 0.2

# ---------------------------------------------------------------------
# Run the test binary and capture the verdict line.
# ---------------------------------------------------------------------

# Pipe stdout through tee so the operator running the harness
# interactively sees output AND we can grep the verdict. The test
# binary emits other gtest chatter; we filter to the M55-R2-VERDICT
# lines after the fact.
LOG="$(mktemp)"
# shellcheck disable=SC2317  # bash trap dispatch is dynamic.
cleanup_log() { rm -f "$LOG"; }
trap 'cleanup; cleanup_log' EXIT INT TERM

set +e
CB_M55_R2_TONE_RUNNING=1 "$TEST_BIN" 2>&1 | tee "$LOG"
RC=${PIPESTATUS[0]}
set -e

# Surface every M55-R2-VERDICT line so the M0 gate scaffold sees them
# all. The R2 gate logic compares verdicts ACROSS the three tests:
#   * Smoke FAIL → R2 FAIL with "patches/0003 needs widening"
#   * Pump FAIL → R2 FAIL with "ADM constructed but pump dead"
#   * Iron FAIL → R2 FAIL with "escalate to choice (a)"
#   * All PASS → R2 PASS, choice (b) ratified
#   * Iron SKIPPED, others PASS → R2 SKIPPED (harness didn't run tone)
echo "---"
echo "verdict-lines:"
grep '^M55-R2-VERDICT: ' "$LOG" || echo "(no verdict lines emitted)"

# Exit shape: gtest's RC is the source of truth for FAIL; if all
# three tests SKIPPED, gtest returns 0 but the gate scaffold needs to
# see 77 (the autotools convention CV2-12's R2-fix already wired up).
if [[ "$RC" -eq 0 ]]; then
  if grep -q '"verdict":"FAIL"' "$LOG"; then
    # Defensive — should not happen (gtest would have set non-zero RC),
    # but if a test emitted FAIL without FAIL-ing the gtest assertion
    # we surface that as RC=1 rather than swallow it.
    exit 1
  fi
  if grep -q '"verdict":"PASS"' "$LOG"; then
    exit 0
  fi
  # No PASS, no FAIL → everything SKIPPED.
  exit 77
fi
exit "$RC"

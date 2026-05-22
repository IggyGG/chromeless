#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
//
// CV2 M5.5 R0 pass-by-vacuity verdict harness. Refined per the Ring-3
// finding (libwebrtc PulseAudio backend not compiled in pre-CV2-75):
//
// The naive design (grep for LS_INFO success line) FALSE-PASSes because:
//   1. The success line is below default libwebrtc verbosity threshold
//      → not visible in cb-chromium stderr.
//   2. The string is hardcoded "PulseAudio" regardless of actual backend
//      → would appear even on ALSA-fallback.
//
// Refined verdict design (negative-evidence anchor):
//   - PASS: ABSENCE of `PulseAudio is disabled using build flag` warning
//     AND ABSENCE of `audio_device_alsa_linux.cc:618 device index is out of range`
//   - FAIL classes:
//     - Pulse-disabled warning present → build-czar's args.gn fix didn't apply
//     - ALSA out-of-range error present → libwebrtc fell back to ALSA
//       (Pulse not compiled OR Pulse server not reachable at runtime)
//
// This script does NOT send wire traffic — M5.5 R0 is a startup-log assertion
// that doesn't require the answerer side. Just boot the worker, wait for it
// to reach steady state (PC construction complete), then external orchestrator
// kubectl-logs greps the cb-chromium output.
//
// In-pod role: confirm wire path is alive (cb-chromium CDP up), then exit.
// External role (orchestrator): kubectl logs cb-chromium + grep for absence
// of the two warning signatures.

const CDP_URL = process.env.CDP_URL || "http://127.0.0.1:9222/json/version";
const CDP_TIMEOUT_MS = parseInt(process.env.CDP_TIMEOUT_MS || "60000", 10);
const POST_READY_WAIT_MS = parseInt(process.env.POST_READY_WAIT_MS || "5000", 10);

function log(level, msg, extra) {
  const line = { ts: new Date().toISOString(), level, msg, ...(extra || {}) };
  console.log(JSON.stringify(line));
}

async function main() {
  log("info", "m5.5-r0 pulse-config test start", {
    cdp_url: CDP_URL,
    cdp_timeout_ms: CDP_TIMEOUT_MS,
    post_ready_wait_ms: POST_READY_WAIT_MS,
  });

  // ───── Phase 1: wait for cb-chromium CDP to come up ─────
  const start = Date.now();
  let cdpUp = false;
  while (Date.now() - start < CDP_TIMEOUT_MS) {
    try {
      const r = await fetch(CDP_URL);
      if (r.ok) {
        const v = await r.json();
        log("ok", "CDP up", { browser: v.Browser, version: v["V8-Version"] });
        cdpUp = true;
        break;
      }
    } catch {}
    await new Promise((r) => setTimeout(r, 1000));
  }

  if (!cdpUp) {
    log("err", "VERDICT: CDP TIMEOUT — worker didn't reach steady state", {});
    process.exit(2);  // exit 2 = worker boot regression, NOT M5.5 R0 defect
  }

  // ───── Phase 2: wait additional time for PCF construction + ADM init ─────
  // The CV2-69 architecture gates SDP offer creation on WS connect, but the
  // PCF (and thus the ADM construction in cb_audio_device_module.cc) happens
  // during embedder bootstrap before any signaling. By the time CDP is up,
  // the ADM construction has already happened — but give a small buffer
  // for any deferred libwebrtc init paths.
  log("info", "CDP up — waiting for PCF/ADM steady state", { wait_ms: POST_READY_WAIT_MS });
  await new Promise((r) => setTimeout(r, POST_READY_WAIT_MS));

  // ───── Phase 3: signal external verdict-derivation ─────
  log("ok", "WORKER STEADY STATE REACHED; verdict derived externally from cb-chromium logs", {
    grep_for_pass: 'ABSENCE of "PulseAudio is disabled using build flag" + ABSENCE of "audio_device_alsa_linux.cc:618 device index is out of range"',
    grep_for_fail_pulse_disabled: 'PulseAudio is disabled using build flag',
    grep_for_fail_alsa_fallback: 'audio_device_alsa_linux.cc:618 device index is out of range',
  });
  process.exit(0);
}

main().catch((e) => {
  log("err", "main threw", { err: String(e), stack: e?.stack });
  process.exit(1);
});

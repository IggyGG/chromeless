#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
//
// CV2 M5 R1 per-event verdict harness (CV2-78 verification).
//
// Verifies the Aura cursor-routing gate is OPEN — i.e. CbHeadlessScreen's
// IsWindowUnderCursor / GetCursorScreenPoint overrides replace the upstream
// display::ScreenBase NOTIMPLEMENTED_LOG_ONCE stubs and the renderer-driven
// cursor-style change now reaches CbCursorClient::SetCursor.
//
// Stimulus / observable (per CV2-78 commit f4062a9 "Verification harness"
// docstring, capture/build-integration/cb_headless_screen.{h,cc}):
//
//   1. Open a CDP session against the cb-chromium worker (port 9222).
//   2. Navigate to `data:text/html,<a href="#">link</a>` — the link gets the
//      UA default `cursor: pointer` style.
//   3. Dispatch Input.dispatchMouseEvent { type: "mouseMoved", x: 10, y: 10 }
//      so the cursor lands over the link's bounding box.
//   4. Wait up to 3 s for cb-chromium to:
//      (a) update aura's last-known cursor position,
//      (b) ask display::Screen::IsWindowUnderCursor (now returns true via
//          CbHeadlessScreen vs. false via ScreenBase stub pre-fix),
//      (c) proceed into CursorClient routing → CbCursorClient::SetCursor
//          fires with new_type=2 (kHand).
//
// Per-event verdict (derived externally — orchestrator greps cb-chromium
// stderr; this script only fires the stimulus and exits cleanly).
//
// Wire-axis split (single-variable discipline, matches phase-a-m4-r1-input-
// dispatch.mjs convention):
//   - This script handles the STIMULUS side (CDP attach + nav + mouseMoved).
//   - The kubectl-logs grep happens OUTSIDE (orchestrator).
//   - Script exit 0 iff the CDP stimulus succeeded; the M5 R1 verdict is
//     derived from the cb-chromium stderr grep, not from this exit code.
//
// PASS signature (cb-chromium stderr, post-stimulus, within ~2 s):
//   [INFO:cb_cursor_client.cc:NN] CbCursorClient::SetCursor new_type=2
//   (kHand corresponds to ui::mojom::CursorType::kHand = 2 in ui/base/cursor)
//
// FAIL classes:
//   - NOTIMPLEMENTED for display::ScreenBase::IsWindowUnderCursor still
//     PRESENT → CbHeadlessScreen subclass not installed (Screen registration
//     regression — route to build-czar).
//   - Above absent BUT no SetCursor LOG → Aura routes past the screen gate
//     but doesn't reach CbCursorClient → downstream wiring regression.
//   - Worker crash post-stimulus → thread-discipline bug at CbCursorClient
//     boundary (route to build-czar).
//
// Environment:
//   CDP_HOST                    — default localhost
//   CDP_PORT                    — default 9222
//   CDP_CONNECT_TIMEOUT_MS      — default 60000 (cb-chromium boot lag)
//   POST_STIMULUS_WAIT_MS       — default 3000 (PostTask + LOG margin)
//   STIMULUS_TARGET_URL         — default data:text/html,<a href="#">link</a>
//                                  (kept inline so the test is self-contained;
//                                  no external network reach needed)

import CDP from "chrome-remote-interface";

const CDP_HOST = process.env.CDP_HOST || "localhost";
const CDP_PORT = parseInt(process.env.CDP_PORT || "9222", 10);
const CDP_CONNECT_TIMEOUT_MS = parseInt(process.env.CDP_CONNECT_TIMEOUT_MS || "60000", 10);
const POST_STIMULUS_WAIT_MS = parseInt(process.env.POST_STIMULUS_WAIT_MS || "3000", 10);
const STIMULUS_TARGET_URL = process.env.STIMULUS_TARGET_URL
  || 'data:text/html,<html><body style="margin:0"><a href="#" style="display:inline-block;padding:10px 20px;font-size:24px">link</a></body></html>';

function log(level, msg, extra) {
  const line = { ts: new Date().toISOString(), level, msg, ...(extra || {}) };
  console.log(JSON.stringify(line));
}

async function waitForCdpReady() {
  const deadline = Date.now() + CDP_CONNECT_TIMEOUT_MS;
  let lastErr;
  while (Date.now() < deadline) {
    try {
      // CDP.List is the cheap /json/list probe — proves the HTTP endpoint is
      // alive AND there's at least one target page we can attach to.
      const targets = await CDP.List({ host: CDP_HOST, port: CDP_PORT });
      if (targets && targets.length > 0) {
        log("ok", "cb-chromium CDP up", { targets: targets.length });
        return targets;
      }
      lastErr = new Error("CDP up but no targets");
    } catch (e) {
      lastErr = e;
    }
    await new Promise((r) => setTimeout(r, 500));
  }
  throw new Error(`CDP not ready after ${CDP_CONNECT_TIMEOUT_MS}ms: ${lastErr}`);
}

async function main() {
  log("info", "m5-r1 per-event cursor-routing test start", {
    cdp: `${CDP_HOST}:${CDP_PORT}`,
    cdp_connect_timeout_ms: CDP_CONNECT_TIMEOUT_MS,
    post_stimulus_wait_ms: POST_STIMULUS_WAIT_MS,
    stimulus_url_preview: STIMULUS_TARGET_URL.slice(0, 80),
  });

  // ───── Phase 1: wait for CDP up ─────
  const t0 = Date.now();
  await waitForCdpReady();
  log("info", "cdp ready", { wait_ms: Date.now() - t0 });

  // ───── Phase 2: attach to first available target ─────
  let client;
  try {
    client = await CDP({ host: CDP_HOST, port: CDP_PORT });
    log("ok", "cdp client attached");
  } catch (e) {
    log("err", "VERDICT: CDP ATTACH FAIL", { err: String(e) });
    process.exit(2);
  }

  const { Page, Runtime, Input, Target } = client;

  try {
    await Page.enable();
    await Runtime.enable();
    log("ok", "Page + Runtime domains enabled");

    // ───── Phase 3: navigate to link-target page ─────
    const navStart = Date.now();
    await Page.navigate({ url: STIMULUS_TARGET_URL });
    await Page.loadEventFired();
    log("ok", "navigation complete", { nav_ms: Date.now() - navStart });

    // Briefly settle the renderer (layout/style pass).
    await new Promise((r) => setTimeout(r, 200));

    // Sanity-check: the link is visible at (10,10)? Query its bounding box
    // via JS, log it. This isn't a verdict assertion (orchestrator greps
    // cb-chromium logs), but it surfaces "link wasn't where we thought"
    // FAIL classes early.
    try {
      const rectResult = await Runtime.evaluate({
        expression: "(() => { const a = document.querySelector('a'); if (!a) return null; const r = a.getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height }; })()",
        returnByValue: true,
      });
      log("info", "link bounding box", { rect: rectResult.result?.value });
    } catch (e) {
      log("warn", "could not query link rect (non-fatal)", { err: String(e) });
    }

    // ───── Phase 4: STIMULUS — mouseMoved over the link ─────
    // CbCursorClient::SetCursor with new_type=2 should fire on cb-chromium
    // stderr within ~2 s of this dispatch.
    const stimulusTs = Date.now();
    log("info", "STIMULUS: dispatching Input.dispatchMouseEvent mouseMoved x=10 y=10", {
      stimulus_ts_ms: stimulusTs,
    });
    await Input.dispatchMouseEvent({
      type: "mouseMoved",
      x: 10,
      y: 10,
      button: "none",
      buttons: 0,
      modifiers: 0,
      // pointerType omitted -> defaults to "mouse"
    });
    log("ok", "stimulus dispatched", { latency_ms: Date.now() - stimulusTs });

    // ───── Phase 5: wait for cb-chromium PostTask + LOG to land ─────
    await new Promise((r) => setTimeout(r, POST_STIMULUS_WAIT_MS));

    log("ok", "WIRE VERDICT: STIMULUS COMPLETE; verdict derived externally from cb-chromium stderr", {
      grep_for_pass:
        "CbCursorClient::SetCursor.*new_type=2",
      grep_for_screen_gate_still_closed:
        "NOTIMPLEMENTED.*display::ScreenBase::IsWindowUnderCursor",
      grep_for_screen_gate_partial:
        "NOTIMPLEMENTED.*display::ScreenBase::GetCursorScreenPoint",
      stimulus_ts_ms: stimulusTs,
      wait_window_ms: POST_STIMULUS_WAIT_MS,
    });
  } catch (e) {
    log("err", "VERDICT: STIMULUS FAIL", { err: String(e), stack: e?.stack });
    try { await client.close(); } catch {}
    process.exit(3);
  }

  try { await client.close(); } catch {}
  process.exit(0);
}

main().catch((e) => {
  log("err", "main threw", { err: String(e), stack: e?.stack });
  process.exit(1);
});

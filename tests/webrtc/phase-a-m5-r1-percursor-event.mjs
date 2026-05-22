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
//   - NOTIMPLEMENTED for display::ScreenBase::GetCursorScreenPoint still
//     PRESENT → CV2-78 ring-2 stub not overridden.
//   - NOTIMPLEMENTED for display::ScreenBase::GetDisplayNearestWindow still
//     PRESENT → CV2-88 ring-8 override regressed (added Wave 2.5 audit
//     cleanup — was missing from Wave 1 era harness; covers d7bc3c0
//     stub-replacement family).
//   - Above absent BUT no SetCursor LOG → Aura routes past the screen gate
//     but doesn't reach CbCursorClient → downstream wiring regression.
//   - Worker crash post-stimulus → thread-discipline bug at CbCursorClient
//     boundary (route to build-czar).
//
// HALT classes (image-level regression, not CV2-78/88 defect):
//   H3   renderer-DOM not laid out (link element absent OR zero-area
//        bounding box) → CV2-89 packaging regression (SwANGLE/Vulkan ICD
//        JSON missing) OR deeper-ring renderer-DOM gate. Pre-CV2-89, this
//        was the Wave 1 Axis 2 gate; on rv7+ this should produce link
//        with non-zero bounding box. The renderer-DOM probe IS now
//        load-bearing (added Wave 2.5 audit cleanup — was non-fatal log
//        in Wave 1 era). Mirrors phase-a-m4-typed-dispatch.mjs HALT H3
//        diagnostic shape.
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
// The HTML payload of a `data:text/html,` URL MUST be percent-encoded: the
// raw `#` in `href="#"` is otherwise parsed as the URL fragment delimiter,
// truncating the document at `<a href="#` — the renderer then lays out an
// empty body and the link-rect probe (Phase 3) reports H3 with no link.
// encodeURIComponent escapes `#` `<` `>` `"` and spaces so the whole payload
// stays inside the data: URL. (Wave 2 rv8 verification finding.)
const STIMULUS_TARGET_HTML =
  '<html><body style="margin:0"><a href="#" style="display:inline-block;padding:10px 20px;font-size:24px">link</a></body></html>';
const STIMULUS_TARGET_URL = process.env.STIMULUS_TARGET_URL
  || ("data:text/html," + encodeURIComponent(STIMULUS_TARGET_HTML));

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
  // `local: true` forces chrome-remote-interface to use its bundled protocol
  // descriptor instead of GET /json/protocol against cb-chromium. cb-chromium
  // does not wire up the DevTools protocol resource; the default fetch hits
  // CHECK-FATAL at devtools_http_handler.cc:724 (SIGABRT on the worker).
  // See CV2-78 verification re-fire — pre-existing latent path, not a Wave 1
  // regression. Production-side fix (ContentClient + protocol resource) is
  // deferred to a follow-up ticket.
  let client;
  try {
    client = await CDP({ host: CDP_HOST, port: CDP_PORT, local: true });
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

    // HALT H3 check: renderer-DOM laid out? Query link bounding box via JS.
    // Pre-CV2-89, the renderer-DOM was blocked by SwANGLE/Vulkan ICD JSON
    // missing → link was either absent OR present-but-zero-area → stimulus
    // mouseMoved at (10,10) couldn't trigger cursor-style change → SetCursor
    // would never fire even with CV2-78 + CV2-88 wired correctly.
    //
    // On rv7+ (CV2-89 packaging fix bundled), the renderer DOES layout, so
    // the link should be present with non-zero area. This check distinguishes
    // HALT H3 (renderer-DOM regression) from FAIL F2 (renderer-DOM present
    // but cursor-routing wiring regressed) — same shape as M4 typed-dispatch
    // harness's ensureRendererPresent() (phase-a-m4-typed-dispatch.mjs:256+).
    //
    // Promoted from "non-fatal log" to "load-bearing HALT" as part of Wave 2.5
    // audit cleanup (was non-fatal in Wave 1 era when Axis 2 was the sole
    // gate and we couldn't distinguish; rv7's CV2-89 bundling enables the
    // tighter diagnostic).
    let domProbe;
    try {
      const rectResult = await Runtime.evaluate({
        expression: "(() => { const a = document.querySelector('a'); if (!a) return null; const r = a.getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height }; })()",
        returnByValue: true,
      });
      domProbe = rectResult.result?.value;
      log("info", "link bounding box probe", { rect: domProbe });
    } catch (e) {
      log("err", "VERDICT: HALT H3 — renderer-DOM probe threw", { err: String(e) });
      try { await client.close(); } catch {}
      process.exit(5);
    }
    if (!domProbe || domProbe.w === 0 || domProbe.h === 0) {
      log("err", "VERDICT: HALT H3 — renderer-DOM not laid out", {
        rect: domProbe,
        reason: domProbe ? "link present but zero-area bounding box" : "link element absent",
        likely_cause: "CV2-89 packaging regression (SwANGLE/Vulkan ICD JSON missing) OR deeper renderer-DOM gate",
      });
      try { await client.close(); } catch {}
      process.exit(5);
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
      grep_for_screen_gate_still_closed_ring_1:
        "NOTIMPLEMENTED.*display::ScreenBase::IsWindowUnderCursor",
      grep_for_screen_gate_partial_ring_2:
        "NOTIMPLEMENTED.*display::ScreenBase::GetCursorScreenPoint",
      grep_for_screen_gate_ring_8:
        "NOTIMPLEMENTED.*display::ScreenBase::GetDisplayNearestWindow",
      stimulus_ts_ms: stimulusTs,
      wait_window_ms: POST_STIMULUS_WAIT_MS,
      halt_classes_pre_stimulus: {
        H3: "renderer-DOM not laid out (link absent or zero-area) — exit 5 pre-stimulus",
      },
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

#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
//
// CV2 M5 R1 renderer-DOM probe harness (CV2-78 axis diagnosis).
//
// Sibling of phase-a-m5-r1-percursor-event.mjs. That harness fires the
// cursor stimulus and lets the verdict be derived from cb-chromium stderr.
// On the rv6.b re-fire the stimulus side reached `STIMULUS COMPLETE` but:
//   - `link bounding box` came back `rect: null` (renderer found no <a> OR
//     <a> had no layout), and
//   - cb-chromium stderr showed `display::ScreenBase::GetDisplayNearestWindow`
//     NOTIMPLEMENTED at boot + GPU/viz init errors.
//
// Two axes could explain the absent `CbCursorClient::SetCursor` log:
//
//   Axis 1 (Screen-routing gate)  — Aura short-circuits at
//     display::ScreenBase::GetDisplayNearestWindow before reaching
//     IsWindowUnderCursor, so cursor routing never enters CursorClient.
//
//   Axis 2 (Renderer-DOM gate)    — renderer never produced a laid-out
//     document; there is no element under the cursor to derive a cursor
//     style from, so no style-change ever propagates back to the browser
//     process even if Aura routing were intact.
//
// This probe is the cheap diagnostic that disambiguates them.
// It DOES NOT fire a cursor stimulus. It only inspects the renderer state
// after navigation + Page.loadEventFired + a 2 s settle. The cursor-stimulus
// harness stays intact for the eventual re-verification.
//
// Verdict mapping (the orchestrator interprets — this script only reports):
//
//   * rect has real width/height AND screenshot succeeds with non-trivial
//     payload AND body innerHTML non-empty
//       → renderer is working. The cursor gate is NOT in the renderer.
//         Path A (Screen-routing override) is the right call.
//
//   * innerHTML present (DOM parsed) BUT rect=null AND/OR screenshot fails
//       → renderer parsed but never composed; the cursor pipeline can't
//         observe a style change either way. Axis 2 is co-load-bearing.
//
//   * innerHTML empty/short AND rect=null AND screenshot fails AND
//     DOM.getDocument returns no/empty tree
//       → renderer is broken end-to-end. Axis 2 is the sole gate;
//         CV2-78 verification is blocked on a different fix than CV2-78.
//
// Environment:
//   CDP_HOST                  — default localhost
//   CDP_PORT                  — default 9222
//   CDP_CONNECT_TIMEOUT_MS    — default 60000
//   POST_LOAD_SETTLE_MS       — default 2000  (renderer layout settle)
//   STIMULUS_TARGET_URL       — default same data: URL as the M5 R1 harness,
//                               kept identical so the same DOM is probed.

import CDP from "chrome-remote-interface";

const CDP_HOST = process.env.CDP_HOST || "localhost";
const CDP_PORT = parseInt(process.env.CDP_PORT || "9222", 10);
const CDP_CONNECT_TIMEOUT_MS = parseInt(process.env.CDP_CONNECT_TIMEOUT_MS || "60000", 10);
const POST_LOAD_SETTLE_MS = parseInt(process.env.POST_LOAD_SETTLE_MS || "2000", 10);
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

// Per-probe deadline. cb-chromium boot is flaky (GPU/viz process exits at init,
// dbus errors, OOM score adjust fails). A blocking CDP call can hang the whole
// probe even though every individual probe is meant to be cheap & isolated.
const PER_PROBE_TIMEOUT_MS = parseInt(process.env.PER_PROBE_TIMEOUT_MS || "8000", 10);

function withTimeout(promise, ms, label) {
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error(`probe '${label}' timed out after ${ms}ms`)), ms);
    promise.then(
      (v) => { clearTimeout(t); resolve(v); },
      (e) => { clearTimeout(t); reject(e); },
    );
  });
}

// Evaluate a JS expression in the page and return { ok, value, err }.
// Wraps Runtime.evaluate so a single probe failure doesn't abort the run —
// every probe contributes to the diagnostic regardless of the others' state.
async function evalSafe(Runtime, expression, label) {
  try {
    const r = await withTimeout(
      Runtime.evaluate({ expression, returnByValue: true }),
      PER_PROBE_TIMEOUT_MS,
      label,
    );
    if (r.exceptionDetails) {
      return { ok: false, err: `exception: ${JSON.stringify(r.exceptionDetails)}` };
    }
    return { ok: true, value: r.result?.value };
  } catch (e) {
    log("warn", `probe '${label}' Runtime.evaluate threw/timed out`, { err: String(e) });
    return { ok: false, err: String(e) };
  }
}

async function main() {
  log("info", "m5-r1 renderer-DOM probe start", {
    cdp: `${CDP_HOST}:${CDP_PORT}`,
    cdp_connect_timeout_ms: CDP_CONNECT_TIMEOUT_MS,
    post_load_settle_ms: POST_LOAD_SETTLE_MS,
    stimulus_url_preview: STIMULUS_TARGET_URL.slice(0, 80),
  });

  // ───── Phase 1: wait for CDP up ─────
  const t0 = Date.now();
  await waitForCdpReady();
  log("info", "cdp ready", { wait_ms: Date.now() - t0 });

  // ───── Phase 2: attach (local: true — see CV2-78 commit 27b2a79) ─────
  let client;
  try {
    client = await CDP({ host: CDP_HOST, port: CDP_PORT, local: true });
    log("ok", "cdp client attached");
  } catch (e) {
    log("err", "VERDICT: CDP ATTACH FAIL", { err: String(e) });
    process.exit(2);
  }

  const { Page, Runtime, DOM } = client;

  // ───── Phase 3: navigate + wait for load + settle ─────
  // Note: we intentionally do NOT call DOM.enable() — on the rv6 image the
  // cb-chromium worker's CDP handler appears to deadlock subsequent
  // Runtime.evaluate calls when DOM domain is enabled. DOM.getDocument is
  // documented as not requiring DOM.enable() on the page target.
  try {
    await Page.enable();
    await Runtime.enable();
    log("ok", "Page + Runtime domains enabled");

    const navStart = Date.now();
    await Page.navigate({ url: STIMULUS_TARGET_URL });
    await Page.loadEventFired();
    log("ok", "navigation + loadEventFired", { nav_ms: Date.now() - navStart });

    // Extra settle beyond the cursor-stimulus harness's 200ms — give the
    // renderer a real layout/composite window before we probe.
    await new Promise((r) => setTimeout(r, POST_LOAD_SETTLE_MS));
    log("info", "post-load settle complete", { settle_ms: POST_LOAD_SETTLE_MS });
  } catch (e) {
    log("err", "VERDICT: NAV/SETTLE FAIL", { err: String(e), stack: e?.stack });
    try { await client.close(); } catch {}
    process.exit(3);
  }

  // ───── Phase 4: structured DOM probes ─────
  // Each probe is isolated; we collect into a single results object so the
  // orchestrator gets one structured emission at the end.
  const results = {};

  // 4.1 document.readyState — proves the renderer at least reached a state.
  results.readyState = await evalSafe(
    Runtime,
    "document.readyState",
    "readyState",
  );

  // 4.2 body.innerHTML length + first 200 chars (truncated to keep the log
  // compact). innerHTML.length=0 + readyState=complete is a strong signal
  // that the document parsed empty (renderer never received content).
  results.bodyInnerHtml = await evalSafe(
    Runtime,
    "(() => { const b = document.body; if (!b) return { present: false, length: 0, excerpt: null }; const h = b.innerHTML || ''; return { present: true, length: h.length, excerpt: h.slice(0, 200) }; })()",
    "bodyInnerHtml",
  );

  // 4.3 link <a> outerHTML — proves the parsed-DOM saw our target node.
  results.linkOuterHtml = await evalSafe(
    Runtime,
    "(() => { const a = document.querySelector('a'); if (!a) return null; return a.outerHTML; })()",
    "linkOuterHtml",
  );

  // 4.4 link getBoundingClientRect — THE signal. {x,y,w,h} with w>0 / h>0
  // means layout ran. null OR w=0/h=0 means the element exists in DOM but
  // never got laid out (the rv6.b symptom).
  results.linkRect = await evalSafe(
    Runtime,
    "(() => { const a = document.querySelector('a'); if (!a) return null; const r = a.getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height, top: r.top, right: r.right, bottom: r.bottom, left: r.left }; })()",
    "linkRect",
  );

  // 4.5 computed cursor style — if layout didn't run, computed style may
  // still resolve (style is calc'd at element resolution, layout is a
  // separate phase). 'pointer' here proves the cursor-derivation source-of-
  // truth is healthy; an absent / null result is an additional renderer signal.
  results.linkCursorStyle = await evalSafe(
    Runtime,
    "(() => { const a = document.querySelector('a'); if (!a) return null; return window.getComputedStyle(a).cursor; })()",
    "linkCursorStyle",
  );

  // 4.6 document dimensions — body offsetWidth/Height. 0/0 with content is
  // another renderer-never-laid-out signal.
  results.documentDims = await evalSafe(
    Runtime,
    "(() => ({ bodyOW: document.body?.offsetWidth ?? null, bodyOH: document.body?.offsetHeight ?? null, docOW: document.documentElement?.offsetWidth ?? null, docOH: document.documentElement?.offsetHeight ?? null, innerW: window.innerWidth, innerH: window.innerHeight }))()",
    "documentDims",
  );

  // 4.7 Page.captureScreenshot — does the compositor produce pixels at all?
  // If renderer is alive but the GPU-process / viz is broken, this fails
  // with a CDP error (recorded but non-fatal). Wrap with timeout — a stuck
  // viz process can hang the request forever.
  try {
    const t = Date.now();
    const shot = await withTimeout(
      Page.captureScreenshot({ format: "png" }),
      PER_PROBE_TIMEOUT_MS,
      "captureScreenshot",
    );
    const len = shot.data?.length || 0;
    results.screenshot = {
      ok: true,
      base64_length: len,
      capture_ms: Date.now() - t,
      // Tiny hash to compare across re-fires without dumping the full image.
      base64_prefix: (shot.data || "").slice(0, 32),
    };
    log("ok", "Page.captureScreenshot succeeded", { base64_length: len });
  } catch (e) {
    results.screenshot = { ok: false, err: String(e) };
    log("warn", "Page.captureScreenshot failed (non-fatal — recorded)", { err: String(e) });
  }

  // 4.8 DOM.getDocument — DevTools-level DOM tree, distinct from JS-side
  // queries above. If JS-side innerHTML lies (e.g. due to renderer state
  // tearing), DOM.getDocument may disagree and surface it.
  try {
    const t = Date.now();
    const doc = await withTimeout(
      DOM.getDocument({ depth: 2, pierce: false }),
      PER_PROBE_TIMEOUT_MS,
      "DOM.getDocument",
    );
    const root = doc.root;
    results.domGetDocument = {
      ok: true,
      capture_ms: Date.now() - t,
      nodeName: root?.nodeName,
      childCount: root?.children?.length ?? 0,
      // First level child node names give a quick structural pulse.
      firstLevelChildren: (root?.children || []).map((c) => c.nodeName),
    };
    log("ok", "DOM.getDocument succeeded", {
      childCount: results.domGetDocument.childCount,
    });
  } catch (e) {
    results.domGetDocument = { ok: false, err: String(e) };
    log("warn", "DOM.getDocument failed (non-fatal — recorded)", { err: String(e) });
  }

  // ───── Phase 5: structured emission ─────
  log("ok", "PROBE COMPLETE — structured results follow", { results });

  try { await client.close(); } catch {}
  process.exit(0);
}

main().catch((e) => {
  log("err", "main threw", { err: String(e), stack: e?.stack });
  process.exit(1);
});

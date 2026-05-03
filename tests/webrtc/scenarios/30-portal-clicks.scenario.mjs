// 30 — portal-encoder mouse clicks (DOM event → portal encoder → WS)
//
// Exercises the FULL portal-side wire path:
//
//   CDP Input.dispatchMouseEvent (test driver, via primary CDP session)
//     → DOM mousedown / mouseup on the test-client canvas
//     → triform_input_encoder::web::from_mouse_event   ◀── PRODUCTION CODE PATH
//     → JSON `{"type":"mouse","action":"down|up","x":…,"y":…,
//                "button":…,"modifiers":…}`
//     → WebSocket → stub WS server in the runner
//     → assertions on what arrived
//
// A failure here that doesn't reproduce in scenario 01 is in the
// portal's encoder layer — wrong field name, wrong modifier
// bit-mapping, wrong coordinate translation. That's the regression
// class the encoder lift is designed to make impossible.
//
// Coordinate system note. The encoder's `web::translate_coords`
// scales canvas-CSS-pixels to canvas-buffer-pixels via
// `width/height` vs `clientWidth/clientHeight`. The fixture sizes
// the canvas 1:1 (canvas.width === canvas.clientWidth), so the
// dispatched CDP coordinates land in the encoded JSON byte-for-byte.
// If this test starts producing scaled coordinates, it means the
// fixture's canvas sizing drifted (look at index.html).

import { runPortalScenario, sleep, MOD } from "./_lib_portal.mjs";
import process from "node:process";

// Test points — comfortably inside the 600×400 canvas + the 720×480
// emulated viewport (canvas is centered with padding so absolute page
// coords need an offset; test driver uses `s.user.click(x, y)` which
// dispatches in viewport coords, and the page receives those mapped
// to canvas-CSS-px via the layout — see the click delivery comment).
const POINTS = {
  tl: { x: 100, y: 100 },
  tr: { x: 500, y: 100 },
  bl: { x: 100, y: 300 },
  br: { x: 500, y: 300 },
  c:  { x: 300, y: 200 },
};

export const scenario = {
  name: "30-portal-clicks",

  async run(s) {
    // Sanity: the page reached `ws_state=open` (verified in setup),
    // and the stub got at least one connection.
    const cb0 = await s.readCbtest();
    s.assert(
      "test client connected to stub WS",
      () => cb0 && cb0.ws_state === "open",
      { expected: "ws_state=open", actual: cb0?.ws_state },
    );
    s.resetWire();

    // ── 1. Single primary click at the canvas centre ──────────────
    await s.user.click(POINTS.c.x, POINTS.c.y);
    // CDP dispatchMouseEvent fires mousePressed + mouseReleased; the
    // page emits one encoded "down" + one "up". (No browser-internal
    // "click" composite event is forwarded — only the raw DOM events
    // the encoder listens to.)
    let wire = await s.waitForWireCount(2);
    const downC = wire.find(
      (m) => m.type === "mouse" && m.action === "down",
    );
    const upC = wire.find(
      (m) => m.type === "mouse" && m.action === "up",
    );
    s.assert(
      "centre click: encoder produced one down + one up",
      () =>
        wire.filter((m) => m.type === "mouse" && m.action === "down").length === 1 &&
        wire.filter((m) => m.type === "mouse" && m.action === "up").length === 1,
      { expected: "1 down + 1 up", actual: wire.map((m) => m.action) },
    );
    s.assert(
      "centre click: down carries button=0 modifiers=0",
      () => downC && downC.button === 0 && downC.modifiers === 0,
      { expected: "button=0 modifiers=0", actual: downC },
    );
    s.assert(
      "centre click: up matches down's coordinates",
      () => upC && downC && upC.x === downC.x && upC.y === downC.y,
      { expected: "matching x/y", actual: { down: downC, up: upC } },
    );

    // ── 2. Shift-click in a different quadrant ────────────────────
    s.resetWire();
    await s.user.click(POINTS.tr.x, POINTS.tr.y, { shift: true });
    wire = await s.waitForWireCount(2);
    const downShift = wire.find(
      (m) => m.type === "mouse" && m.action === "down",
    );
    s.assert(
      "shift-click: modifiers carry the shift bit (=8)",
      () =>
        downShift &&
        (downShift.modifiers & MOD.Shift) === MOD.Shift &&
        downShift.modifiers === MOD.Shift, // exactly Shift, no stray bits
      {
        expected: `modifiers=${MOD.Shift}`,
        actual: downShift?.modifiers,
      },
    );

    // ── 3. Right-click ────────────────────────────────────────────
    s.resetWire();
    await s.user.click(POINTS.br.x, POINTS.br.y, { button: "right" });
    wire = await s.waitForWireCount(2);
    const downRight = wire.find(
      (m) => m.type === "mouse" && m.action === "down",
    );
    // DOM `MouseEvent.button` for right-click is 2.
    s.assert(
      "right-click: encoder reports button=2",
      () => downRight && downRight.button === 2,
      { expected: "button=2", actual: downRight?.button },
    );

    // ── 4. Wire format: `type` field is exactly "mouse" ───────────
    // Pinning this prevents a refactor from accidentally renaming the
    // variant tag (which would silently fall through to physics's
    // ClientMessage default and disappear). Same reason scenario 21
    // pins the "key" tag.
    const allMouse = s.receivedWire().filter((m) => m.type === "mouse");
    s.assert(
      "wire shape: every mouse message has type='mouse' literal",
      () =>
        allMouse.length > 0 &&
        allMouse.every((m) => m.type === "mouse" && typeof m.action === "string"),
      { expected: "all type=mouse", actual: allMouse.map((m) => m.type) },
    );

    // ── 5. Cross-check the encoder's __cbtest diagnostic ──────────
    // The page records every encoded message in `window.__cbtest.sent`
    // (cumulative — never reset). After 3 click operations × 2 events
    // each (down + up), we expect 6 entries. Stub-side counts are
    // cleared between sections via `resetWire()` so they're not the
    // right comparison surface here; using cbtest's cumulative tally
    // pins the encoder fired exactly 6 times across the run.
    const cb = await s.readCbtest();
    s.assert(
      "page-side cbtest recorded all 6 encoder firings",
      () => cb && cb.sent && cb.sent.length === 6,
      { expected: "cbtest.sent.length === 6", actual: cb?.sent?.length },
    );
    s.assert(
      "page-side mouse counter matches",
      () => cb && cb.counts && cb.counts.mouse === 6,
      { expected: "counts.mouse === 6", actual: cb?.counts?.mouse },
    );
  },
};

if (import.meta.url === `file://${process.argv[1]}`) {
  runPortalScenario(scenario);
}

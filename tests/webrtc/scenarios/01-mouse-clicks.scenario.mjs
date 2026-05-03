// 01 — mouse clicks
//
// Drives single-clicks, double-click, right-click, modifier-held click
// against the 9-button click-zone grid and asserts each landed on the
// correct button with the correct event payload.
//
// Why this matters:
//   * Single-click vs double-click discrimination (chromium needs
//     clickCount=1 then clickCount=2 within the dblclick interval —
//     get it wrong and you fire two singles instead of one double).
//   * Modifiers must accompany BOTH mousedown and mouseup; if you only
//     pass them on one, page handlers that key off MouseEvent.shiftKey
//     get the wrong answer.
//   * auxclick (middle/right) is the path browsers use to open
//     middle-click-new-tab and context menus.
//
// Recording:
//   webrtc-01-mouse-clicks.webm shows each click as a fading ripple
//   and a "was-clicked" badge on the target button. Marker timeline:
//   "single-tl" → "double-mc" → "right-tr" → "shift-bc".

import { runScenario, sleep } from "./_lib.mjs";

// Click-grid coordinates inside the 1280×720 viewport. The grid is at
// CSS top:56 left:16 width:280 height:280 with 6px gaps and 3×3 cells.
// Each cell ≈ ((280 - 12) / 3) ≈ 89.3 px wide; centre-of-cell
// approximations below.
const GRID = {
  tl: { x:  62, y: 102 }, tc: { x: 156, y: 102 }, tr: { x: 250, y: 102 },
  ml: { x:  62, y: 196 }, mc: { x: 156, y: 196 }, mr: { x: 250, y: 196 },
  bl: { x:  62, y: 290 }, bc: { x: 156, y: 290 }, br: { x: 250, y: 290 },
};

export const scenario = {
  name: "01-mouse-clicks",
  page: "fixture-input-mirror.html",

  async run(s) {
    s.marker("single-tl");
    await s.user.click(GRID.tl.x, GRID.tl.y);
    await sleep(200);

    s.marker("double-mc");
    await s.user.doubleClick(GRID.mc.x, GRID.mc.y);
    await sleep(200);

    s.marker("right-tr");
    await s.user.click(GRID.tr.x, GRID.tr.y, { button: "right" });
    await sleep(200);

    s.marker("shift-bc");
    await s.user.click(GRID.bc.x, GRID.bc.y, { shift: true });
    await sleep(400);

    const events = await s.readEvents();

    const clicks      = events.filter((e) => e.type === "click");
    const dblclicks   = events.filter((e) => e.type === "dblclick");
    const auxclicks   = events.filter((e) => e.type === "auxclick");
    const contextmenu = events.filter((e) => e.type === "contextmenu");

    s.assert("got TL single click",
      () => clicks.some((e) => e.zone === "tl" && e.detail === 1),
      { expected: "click on tl with detail=1", actual: clicks.map((e) => e.zone) });

    s.assert("got MC double click (one dblclick + two clicks)",
      () => dblclicks.some((e) => e.zone === "mc")
         && clicks.filter((e) => e.zone === "mc").length === 2,
      { expected: "1 dblclick + 2 clicks on mc",
        actual: { dbl: dblclicks.map((e) => e.zone),
                  click: clicks.filter((e) => e.zone === "mc") } });

    s.assert("got TR right-click (auxclick + contextmenu)",
      () => contextmenu.some((e) => e.zone === "tr")
         && auxclicks.some((e) => e.zone === "tr" && e.button === 2),
      { expected: "contextmenu + auxclick on tr",
        actual: { contextmenu: contextmenu.map((e) => e.zone),
                  aux: auxclicks } });

    s.assert("got BC shift-click with shiftKey set on the click event",
      () => clicks.some((e) => e.zone === "bc" && e.shift === true),
      { expected: "click on bc with shift=true",
        actual: clicks.filter((e) => e.zone === "bc") });
  },
};

if (import.meta.url === `file://${process.argv[1]}`) runScenario(scenario);

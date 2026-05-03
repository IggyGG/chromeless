// 03 — HTML5 drag and drop
//
// Drags the dndSource element to the dndTarget zone and asserts the
// full HTML5 drag event sequence fires:
//   source: dragstart → drag (sampled) → dragend
//   target: dragenter → dragover (sampled) → drop
//
// Why this matters (the trickiest input scenario, by some margin):
//
//   * Chromium's hit detector won't promote a mousedown+mousemove
//     into a drag unless the cursor moves > ~5 px during mousedown.
//     Issuing one big mouseMove from source to target after pressing
//     skips the drag detection entirely — the page sees it as a
//     misclick. We have to ramp the path with several mouseMoves.
//
//   * dataTransfer must be populated in the page's dragstart handler
//     for drop to receive a payload. The browser calls the source
//     handler synchronously while CDP holds the mousedown — so the
//     dragstart firing here proves the synchronous path landed.
//
//   * dragenter/dragleave/drop on the target only fire if the target
//     calls preventDefault() in dragover (the fixture page does).
//     Without preventDefault, the cursor flips to "no-drop" and
//     drop never fires.
//
// Recording:
//   webrtc-03-drag-and-drop.webm shows the source highlighted, the
//   target lighting up green during drag-over, and the drop landing
//   with the source's data payload visible in the target.

import { runScenario, sleep } from "./_lib.mjs";

// dnd region: top:360 left:16 width:460 height:220, padding:12, gap:12
//   left half (source):  16+12 .. 16+12+(460-12-12-12)/2 = 28..234
//   right half (target): 246 .. 452
//   y mid: 360 + 110 = 470
const SOURCE = { x: 130, y: 470 };
const TARGET = { x: 350, y: 470 };

export const scenario = {
  name: "03-drag-and-drop",
  page: "fixture-input-mirror.html",

  async run(s) {
    // Hover source first to settle hit-test target.
    await s.user.mouseMove(SOURCE.x, SOURCE.y);
    await sleep(120);
    await s.runtimeEval(`window.__events.length = 0; null`);

    s.marker("drag-start");
    await s.user.drag(SOURCE.x, SOURCE.y, TARGET.x, TARGET.y, {
      steps: 16,        // chromium needs several intermediate moves
      settleMs: 18,     // ~1 frame between moves; total drag ~300ms
    });
    s.marker("drag-end");

    // Give the drop handler a beat to update the DOM.
    await sleep(250);

    const events = await s.readEvents();

    const dragstart  = events.filter((e) => e.type === "dragstart");
    const drags      = events.filter((e) => e.type === "drag");
    const dragend    = events.filter((e) => e.type === "dragend");
    const dragenter  = events.filter((e) => e.type === "dragenter");
    const dragover   = events.filter((e) => e.type === "dragover");
    const drops      = events.filter((e) => e.type === "drop");

    s.assert("dragstart fired once on source",
      () => dragstart.length === 1 && dragstart[0].source === "A",
      { expected: "exactly 1 dragstart with source A", actual: dragstart });

    s.assert("at least one drag event fired during the drag",
      () => drags.length >= 1,
      { expected: ">=1 drag", actual: drags.length });

    s.assert("dragenter fired on target",
      () => dragenter.length >= 1,
      { expected: ">=1 dragenter", actual: dragenter.length });

    s.assert("dragover fired on target (preventDefault path engaged)",
      () => dragover.length >= 1,
      { expected: ">=1 dragover", actual: dragover.length });

    s.assert("drop fired with the source's data payload",
      () => drops.length === 1 && drops[0].data === "data:source-A",
      { expected: "1 drop with data 'data:source-A'", actual: drops });

    s.assert("dragend fired exactly once on source",
      () => dragend.length === 1,
      { expected: "1 dragend", actual: dragend });

    // DOM-level confirmation: the target's textContent should reflect
    // the drop. Reading state from the page rather than just events
    // catches the case where event handlers fired but the
    // application-level effect (DOM mutation) didn't land.
    const dropText = await s.runtimeEval(
      `document.getElementById("dndTarget").textContent`,
    );
    s.assert("target DOM was updated by drop handler",
      () => dropText && dropText.includes("data:source-A"),
      { expected: "target text contains 'data:source-A'", actual: dropText });
  },
};

if (import.meta.url === `file://${process.argv[1]}`) runScenario(scenario);

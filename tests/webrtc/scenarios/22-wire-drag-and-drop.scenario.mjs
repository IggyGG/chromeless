// 22 — wire-path HTML5 drag-and-drop (DataChannel → bridge → CDP)
//
// The trickiest gesture, exercised end-to-end through the
// production wire path. We dispatch:
//
//   1× mouse_button {action: "down", x:src, y:src}
//   16× mouse_move along the path src → dst
//   1× mouse_button {action: "up", x:dst, y:dst}
//
// The bridge must:
//   * translate mouse_button {down} into Input.dispatchMouseEvent
//     {type: mousePressed} with buttons=1
//   * translate the 16 mouse_move envelopes into mouseMoved events
//     with buttons=1 (so chromium's drag detector engages)
//   * translate mouse_button {up} into mouseReleased
//
// And the page must observe the full HTML5 drag sequence (dragstart,
// drag, dragenter, dragover, drop, dragend) with the correct payload
// in dataTransfer.
//
// A failure here that scenario 03 (CDP-direct drag) passes means the
// bridge's mouse_button-during-drag handling drops the buttons=1
// state — a real production bug class.

import { runScenario, sleep } from "./_lib.mjs";

const SOURCE = { x: 130, y: 470 };
const TARGET = { x: 350, y: 470 };

export const scenario = {
  name: "22-wire-drag-and-drop",
  page: "fixture-input-mirror.html",
  wire: { url: "ws://127.0.0.1:9100/input" },

  async run(s) {
    const wire = await s.setupWire();

    // Settle hover on source.
    await wire.mouseMove(SOURCE.x, SOURCE.y);
    await sleep(150);
    await s.runtimeEval(`window.__events.length = 0; null`);

    s.marker("wire-drag-start");
    await wire.drag(SOURCE.x, SOURCE.y, TARGET.x, TARGET.y, {
      steps: 16, settleMs: 18,
    });
    s.marker("wire-drag-end");
    await sleep(300);

    const events = await s.readEvents();
    const dragstart = events.filter((e) => e.type === "dragstart");
    const drags     = events.filter((e) => e.type === "drag");
    const dragend   = events.filter((e) => e.type === "dragend");
    const dragenter = events.filter((e) => e.type === "dragenter");
    const dragover  = events.filter((e) => e.type === "dragover");
    const drops     = events.filter((e) => e.type === "drop");
    const relay = await s.readWireRelay();

    s.assert("[wire] relay forwarded the mouse_button + mouse_move sequence",
      () => relay && relay.ws_forwarded >= 17 && relay.ws_dropped === 0,
      { expected: ">=17 forwarded (1 down + 16 moves + 1 up), 0 dropped",
        actual: relay });

    // Bridge held-button-during-drag regression test: the v1
    // mouse_move envelope shape is just `{x,y}` — no buttons field.
    // The bridge tracks held buttons from mouse_button down/up
    // envelopes (heldButtons state) and applies the current bitmask
    // to mouse_move CDP dispatch, so chromium's drag detector keeps
    // the gesture alive: dragstart→drag→dragenter→dragover→drop→
    // dragend all fire correctly.
    s.assert("[wire] dragstart fired on source — buttons survived through drag",
      () => dragstart.length === 1 && dragstart[0].source === "A",
      { expected: "1 dragstart with source A", actual: dragstart });

    s.assert("[wire] drag events fired during move sequence",
      () => drags.length >= 1,
      { expected: ">=1 drag", actual: drags.length });

    s.assert("[wire] dragenter fired on target",
      () => dragenter.length >= 1, { actual: dragenter.length });

    s.assert("[wire] dragover engaged target's preventDefault",
      () => dragover.length >= 1, { actual: dragover.length });

    s.assert("[wire] drop fired with source's data payload",
      () => drops.length === 1 && drops[0].data === "data:source-A",
      { expected: "1 drop with data:source-A", actual: drops });

    s.assert("[wire] dragend fired on source",
      () => dragend.length === 1,
      { expected: "1 dragend", actual: dragend });

    const dropText = await s.runtimeEval(
      `document.getElementById("dndTarget").textContent`,
    );
    s.assert("[wire] target DOM updated by drop handler",
      () => dropText && dropText.includes("data:source-A"),
      { expected: "target text contains 'data:source-A'", actual: dropText });
  },
};

if (import.meta.url === `file://${process.argv[1]}`) runScenario(scenario);

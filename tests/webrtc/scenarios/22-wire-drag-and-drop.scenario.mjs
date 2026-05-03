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

    // KNOWN BRIDGE BUG #3: drag breaks because mouse_move during
    // mousedown loses the held-button state.
    //
    // capture/input-bridge/main.go:528 sends every mouse_move event
    // to CDP as `button: "none", modifiers: 0`. There's no `buttons`
    // field on the wire (the v1 protocol's mouse_move shape is just
    // `{x, y}`) and the bridge has no state machine that infers
    // "button is held because the last mouse_button was a 'down'
    // without a matching 'up'." So during a drag, chromium sees:
    //
    //   mouseDown  buttons=1  (correct)
    //   mouseMove  buttons=0  (WRONG — break)
    //   mouseMove  buttons=0  ...
    //   mouseUp    buttons=0  (chromium thinks nothing was held)
    //
    // Chromium's drag detector aborts when it sees an unbuttoned
    // mouseMove during what should be a drag, so dragstart/drag/drop
    // never fire. The page-level drag never happens.
    //
    // Fix: bridge maintains pointer-button state across messages —
    // track which buttons were last `mouse_button` down without a
    // matching up, and OR them into `buttons` on mouse_move CDP
    // dispatch. (Same state machine pattern as the modifier fix.)
    //
    // ALL six drag assertions below fail until the bridge is patched.
    // The recording shows the cursor moving from source to target
    // but no drop highlighting — visible evidence of the bug.
    s.assert("[wire] [BRIDGE-BUG #3] dragstart fired on source — buttons=1 survived",
      () => dragstart.length === 1 && dragstart[0].source === "A",
      { expected: "1 dragstart — but bridge mouse_move drops buttons state",
        actual: dragstart });

    s.assert("[wire] [BRIDGE-BUG #3] drag events fired during move sequence",
      () => drags.length >= 1,
      { expected: ">=1 drag — but bridge mouse_move drops buttons state",
        actual: drags.length });

    s.assert("[wire] [BRIDGE-BUG #3] dragenter fired on target",
      () => dragenter.length >= 1,
      { expected: ">=1 dragenter — but bridge mouse_move drops buttons state",
        actual: dragenter.length });

    s.assert("[wire] [BRIDGE-BUG #3] dragover engaged target's preventDefault",
      () => dragover.length >= 1,
      { expected: ">=1 dragover — but bridge mouse_move drops buttons state",
        actual: dragover.length });

    s.assert("[wire] [BRIDGE-BUG #3] drop fired with source's data payload",
      () => drops.length === 1 && drops[0].data === "data:source-A",
      { expected: "1 drop with data:source-A — but bridge mouse_move drops buttons state",
        actual: drops });

    s.assert("[wire] [BRIDGE-BUG #3] dragend fired on source",
      () => dragend.length === 1,
      { expected: "1 dragend — but bridge mouse_move drops buttons state",
        actual: dragend });

    const dropText = await s.runtimeEval(
      `document.getElementById("dndTarget").textContent`,
    );
    s.assert("[wire] [BRIDGE-BUG #3] target DOM updated by drop handler",
      () => dropText && dropText.includes("data:source-A"),
      { expected: "target text contains 'data:source-A' — but drop never fires",
        actual: dropText });
  },
};

if (import.meta.url === `file://${process.argv[1]}`) runScenario(scenario);

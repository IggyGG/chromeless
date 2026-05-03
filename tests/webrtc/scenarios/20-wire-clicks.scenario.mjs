// 20 — wire-path mouse clicks (DataChannel → input-bridge → CDP)
//
// Exercises the FULL production wire path end-to-end:
//
//   harness.dc.send(envelope)
//     → page.dc.onmessage
//     → page.InputRelay.forward()
//     → ws://127.0.0.1:9100/input
//     → input-bridge sidecar
//     → CDP Input.dispatchMouseEvent
//     → renderer
//     → page event listener
//     → window.__events
//
// The CDP-direct scenarios (01–12) cover page-level behaviour — what
// the page receives, regardless of how it was dispatched. THIS
// scenario covers the wire-format roundtrip: the v1 envelope shape,
// the page-side InputRelay, the input-bridge translation table.
//
// A failure here that isn't reproduced by scenario 01 means the bug
// is in the wire path (envelope schema mismatch, relay forwarding,
// bridge translation), not in the page or in chromium's input
// handling.
//
// Recording:
//   webrtc-20-wire-clicks.webm shows the click ripples landing on
//   each target button just like the CDP-direct version, plus the
//   page-side relay diagnostic in window.__cbtest.relay reflects
//   the ws_forwarded count climbing.

import { runScenario, sleep } from "./_lib.mjs";

const GRID = {
  tl: { x:  62, y: 102 }, tc: { x: 156, y: 102 }, tr: { x: 250, y: 102 },
  ml: { x:  62, y: 196 }, mc: { x: 156, y: 196 }, mr: { x: 250, y: 196 },
  bl: { x:  62, y: 290 }, bc: { x: 156, y: 290 }, br: { x: 250, y: 290 },
};

export const scenario = {
  name: "20-wire-clicks",
  page: "fixture-input-mirror.html",

  // Enable wire mode: the fixture page creates an "input"
  // RTCDataChannel and forwards messages to ws://127.0.0.1:9100/input
  // (the input-bridge sidecar's default listen address).
  wire: { url: "ws://127.0.0.1:9100/input" },

  async run(s) {
    const wire = await s.setupWire();
    await s.runtimeEval(`window.__events.length = 0; null`);

    s.marker("wire-single-tl");
    await wire.click(GRID.tl.x, GRID.tl.y);
    await sleep(150);

    s.marker("wire-double-mc");
    await wire.doubleClick(GRID.mc.x, GRID.mc.y);
    await sleep(200);

    s.marker("wire-right-tr");
    await wire.click(GRID.tr.x, GRID.tr.y, { button: "right" });
    await sleep(150);

    s.marker("wire-shift-bc");
    await wire.click(GRID.bc.x, GRID.bc.y, { shift: true });
    await sleep(300);

    const events = await s.readEvents();
    const clicks = events.filter((e) => e.type === "click");
    const dblclicks = events.filter((e) => e.type === "dblclick");
    const contextmenu = events.filter((e) => e.type === "contextmenu");
    const auxclicks = events.filter((e) => e.type === "auxclick");
    const relay = await s.readWireRelay();

    s.assert("[wire] page-side relay forwarded messages to bridge",
      () => relay && relay.ws_forwarded > 0 && relay.ws_dropped === 0,
      { expected: "ws_forwarded>0 ws_dropped=0", actual: relay });

    s.assert("[wire] TL single click landed via the bridge",
      () => clicks.some((e) => e.zone === "tl" && e.detail === 1),
      { expected: "click on tl with detail=1",
        actual: clicks.map((e) => e.zone) });

    // Bridge clickCount-ramp regression test: the bridge must elevate
    // clickCount on rapid successive same-button clicks within
    // ~500 ms / ~5 px so chromium's renderer fires dblclick. Was bug
    // before the ramp state-machine landed (main.go: clickCount field
    // on dispatcher's input state).
    s.assert("[wire] MC double click produced 1 dblclick + 2 clicks (clickCount ramp)",
      () => dblclicks.some((e) => e.zone === "mc")
         && clicks.filter((e) => e.zone === "mc").length === 2,
      { expected: "1 dblclick + 2 clicks on mc",
        actual: { dblclicks: dblclicks.map((e) => e.zone),
                  clicks: clicks.filter((e) => e.zone === "mc") } });

    s.assert("[wire] TR right-click (button=2) produced contextmenu + auxclick",
      () => contextmenu.some((e) => e.zone === "tr")
         && auxclicks.some((e) => e.zone === "tr" && e.button === 2),
      { expected: "contextmenu + auxclick on tr",
        actual: { contextmenu: contextmenu.map((e) => e.zone),
                  aux: auxclicks } });

    // Bridge modifier-state regression test: the wire shape brackets
    // a shift-click as `key_down Shift / mouse_button down / up /
    // key_up Shift`. The bridge tracks Shift/Ctrl/Alt/Meta key_down/up
    // envelopes in heldMods state and applies the current bitmask to
    // mouse events — so the page's click event has shiftKey=true.
    s.assert("[wire] shift-modifier on click reached the page (held-mod state)",
      () => clicks.some((e) => e.zone === "bc" && e.shift === true),
      { expected: "click on bc with shift=true",
        actual: clicks.filter((e) => e.zone === "bc") });

    // Sanity: every click envelope we sent produced a corresponding
    // mousedown + mouseup at the target coords. Confirms the bridge's
    // mouse_button → CDP Input.dispatchMouseEvent translation.
    const mouseDowns = events.filter((e) => e.type === "mousedown");
    const mouseUps   = events.filter((e) => e.type === "mouseup");
    s.assert("[wire] every click envelope produced mousedown + mouseup at the target",
      () => {
        // 1 single + 2 of double + 1 right + 1 shift = 5 down + 5 up
        return mouseDowns.length >= 5 && mouseUps.length >= 5;
      },
      { expected: ">=5 each",
        actual: { downs: mouseDowns.length, ups: mouseUps.length } });
  },
};

if (import.meta.url === `file://${process.argv[1]}`) runScenario(scenario);

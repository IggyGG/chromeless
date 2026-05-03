// 10 — concurrent CDP cursor fight (agent + user)
//
// Production reality: an LLM agent driving the browser via physics
// CdpSession AND a human user driving via input-bridge / DataChannel
// are TWO CDP attachers on the same target. Each can dispatch
// `Input.dispatchMouseEvent` independently, with no coordination.
// What does chromium do?
//
// THE ANSWER (pinned by this scenario):
//
//   * Chromium SERIALISES input from concurrent CDP sessions at the
//     renderer's input-router. There's a single pointer state per
//     RenderFrame, so two simultaneously-issued mouseDowns from
//     different sessions don't both register as their own clicks —
//     they interleave at the wire and can be miscategorised
//     (e.g. session-A's mouseDown + session-B's mouseUp at different
//     coordinates becomes a *drag*, not a click).
//
//   * Modifier bits are CARRIED PER-EVENT, not stored in any
//     session-level pointer state. So a sequential agent-then-user
//     pattern keeps modifiers isolated even though pointer state
//     is shared.
//
//   * SEQUENTIAL alternation works perfectly — both sessions can
//     dispatch in turn and each event lands on the intended target.
//     This is what the production "agent and user collaborate" path
//     should look like (input-bridge sequences user inputs, agent
//     dispatches between them).
//
//   * CONCURRENT dispatch (Promise.all) produces chaotic event logs:
//     coordinates from both paths land, but the synthesised
//     click/dragstart events depend on the wire-arrival order. Some
//     dispatches never produce their click event because the pointer
//     state was disturbed mid-transaction by the other session.
//
// This scenario asserts the production-relevant invariants:
//   * Sequential alternation: every dispatched click lands.
//   * Modifier isolation across sequential sessions.
//   * Concurrent dispatch: both sessions' mousemove coordinates
//     reach the page (proves they aren't dropped wholesale).
//   * Concurrent click: documents (NOT asserts) that some clicks
//     get miscategorised as drags — by recording the dragstart
//     events that fire instead.
//
// Recording:
//   webrtc-10-concurrent-cursor-fight.webm shows BOTH the clean
//   sequential pattern (clicks landing alternately on TL and BR)
//   AND the concurrent-fight section (rapid Promise.all dispatches
//   that produce drag-shaped event logs). Markers separate each
//   sub-phase so a reviewer can scrub to the interesting bits.

import { runScenario, sleep } from "./_lib.mjs";

const GRID = {
  tl: { x:  62, y: 102 },
  tc: { x: 156, y: 102 },
  tr: { x: 250, y: 102 },
  ml: { x:  62, y: 196 },
  mc: { x: 156, y: 196 },
  mr: { x: 250, y: 196 },
  bl: { x:  62, y: 290 },
  bc: { x: 156, y: 290 },
  br: { x: 250, y: 290 },
};

export const scenario = {
  name: "10-concurrent-cursor-fight",
  page: "fixture-input-mirror.html",

  async run(s) {
    const agent = await s.openSecondaryCdpSession("agent");
    await s.runtimeEval(`window.__events.length = 0; null`);

    // ─── Phase 1: SEQUENTIAL alternation ────────────────────────────
    // User → Agent → User → Agent → ... 5 rounds. Sleep between each
    // dispatch ensures chromium's pointer state settles between
    // events. This is the production-friendly pattern.
    s.marker("sequential-alternation");
    for (let i = 0; i < 5; i++) {
      await s.user.click(GRID.tl.x, GRID.tl.y);
      await sleep(40);
      await agent.click(GRID.br.x, GRID.br.y);
      await sleep(40);
    }

    const seqEvents = await s.readEvents();
    const seqClicks = seqEvents.filter((e) => e.type === "click");

    s.assert(
      "[sequential] both TL (user) and BR (agent) received their 5 clicks",
      () => {
        const tl = seqClicks.filter((e) => e.zone === "tl").length;
        const br = seqClicks.filter((e) => e.zone === "br").length;
        return tl === 5 && br === 5;
      },
      { expected: "tl=5 br=5",
        actual: { tl: seqClicks.filter((e) => e.zone === "tl").length,
                  br: seqClicks.filter((e) => e.zone === "br").length } },
    );

    // ─── Phase 2: SEQUENTIAL modifier divergence ────────────────────
    // User shift-clicks TR, then agent plain-clicks BL. Modifier bits
    // travel per-event, so neither session leaks state to the other.
    await s.runtimeEval(`window.__events.length = 0; null`);
    s.marker("sequential-modifier-divergence");
    await s.user.click(GRID.tr.x, GRID.tr.y, { shift: true });
    await sleep(80);
    await agent.click(GRID.bl.x, GRID.bl.y);
    await sleep(120);

    const modEvents = await s.readEvents();
    const tr = modEvents.find((e) => e.type === "click" && e.zone === "tr");
    const bl = modEvents.find((e) => e.type === "click" && e.zone === "bl");

    s.assert("[sequential] modifier bits don't leak across sessions",
      () => tr && tr.shift === true && bl && bl.shift === false,
      { expected: "tr.shift=true (user), bl.shift=false (agent)",
        actual: { tr, bl } });

    // ─── Phase 3: SEQUENTIAL hover traversal from agent path ────────
    // Park user cursor inside core, then have agent walk a path
    // outer → mid → inner → core → inner → mid → outer. With nested
    // DOM the hover events fire correctly even though the user's
    // "logical cursor" never moved.
    await s.runtimeEval(`window.__events.length = 0; null`);
    s.marker("sequential-hover-traverse");
    await s.user.mouseMove(1100, 196); // park in core
    await sleep(120);
    for (const x of [880, 952, 984, 1016, 1100, 1016, 984, 952, 880]) {
      await agent.mouseMove(x, 196);
      await sleep(60);
    }

    const hoverEvents = await s.readEvents();
    const enters = hoverEvents
      .filter((e) => e.type === "mouseenter").map((e) => e.zone);

    s.assert(
      "[sequential] agent hover traversal entered each nested zone",
      // Going inwards: outer → mid → inner → core. We don't check
      // an EXACT match because the user's parked-in-core cursor
      // means we may see an early "core" enter from the user's move.
      // We just check each zone shows up at least once.
      () => ["outer", "mid", "inner", "core"].every((z) => enters.includes(z)),
      { expected: "every zone in enters",
        actual: enters });

    // ─── Phase 4: CONCURRENT dispatch (Promise.all) — DOCUMENT chaos
    // Same TL/BR alternation but with Promise.all so the two CDP
    // sessions race at the wire layer. Production-relevant finding:
    // chromium serialises at the renderer input-router, so concurrent
    // mouseDowns can interleave into drag-shaped sequences.
    await s.runtimeEval(`window.__events.length = 0; null`);
    s.marker("concurrent-promise-all");
    for (let i = 0; i < 5; i++) {
      await Promise.all([
        s.user.click(GRID.tl.x, GRID.tl.y),
        agent.click(GRID.br.x, GRID.br.y),
      ]);
      await sleep(40);
    }

    const conEvents = await s.readEvents();
    const conClicks   = conEvents.filter((e) => e.type === "click");
    const conDragstrt = conEvents.filter((e) => e.type === "dragstart");
    const conMouseDn  = conEvents.filter((e) => e.type === "mousedown");
    const conMouseUp  = conEvents.filter((e) => e.type === "mouseup");

    // CRITICAL invariant: BOTH sessions' inputs must reach chromium —
    // if one were dropped wholesale, that'd be a serious regression.
    // We check via mousedown/mouseup counts (which fire regardless
    // of how chromium classifies the resulting gesture).
    s.assert(
      "[concurrent] both sessions' raw inputs reach chromium (mousedown count >= 10)",
      () => conMouseDn.length >= 10,
      { expected: ">=10 mousedowns from 5×2 dispatches",
        actual: conMouseDn.length });

    s.assert(
      "[concurrent] both TL and BR coordinates appear among the dispatched mousedowns",
      () => {
        const seenTL = conMouseDn.some(
          (e) => Math.abs(e.x - GRID.tl.x) <= 2 && Math.abs(e.y - GRID.tl.y) <= 2,
        );
        const seenBR = conMouseDn.some(
          (e) => Math.abs(e.x - GRID.br.x) <= 2 && Math.abs(e.y - GRID.br.y) <= 2,
        );
        return seenTL && seenBR;
      },
      { expected: "both TL and BR coords in mousedown list",
        actual: conMouseDn.map((e) => `(${e.x},${e.y})`) });

    // The chaotic finding — pinned, not asserted-against.
    // We RECORD it as an informational assertion so the report shows
    // the ratio. A regression that *changed* the serialisation
    // semantics (e.g. chromium added per-session pointer state) would
    // show up as a sudden spike in conClicks back to 10.
    const chaosLine = `concurrent dispatches: ` +
      `mousedowns=${conMouseDn.length} ` +
      `mouseups=${conMouseUp.length} ` +
      `clicks=${conClicks.length} ` +
      `dragstarts=${conDragstrt.length}`;
    s.log = s.log || (() => {});
    s.assert(`[concurrent] CHAOS DOCUMENTED — ${chaosLine}`,
      () => true, // informational; never fails
      { expected: "informational",
        actual: chaosLine });
  },
};

if (import.meta.url === `file://${process.argv[1]}`) runScenario(scenario);

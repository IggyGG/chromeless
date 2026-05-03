// 02 — hover traverse (nested zones)
//
// Walks the cursor from outside the hover-zones panel through outer →
// mid → inner → core, then back out, asserting that mouseenter +
// mouseleave fire EXACTLY once per nested zone in the right order.
//
// Why this matters:
//   * mouseenter/leave don't bubble; mouseover/out do. Production
//     hover state machines (tooltips, dropdowns) depend on this
//     non-bubbling pair, and getting the dispatch order wrong
//     produces flicker — tooltips re-anchoring on every internal
//     mousemove.
//   * Re-entering an ancestor while hovering a descendant must NOT
//     re-fire mouseenter on the ancestor (the cursor never left it).
//     This catches a common chromium bug class where dispatch hits
//     a wrong target due to compositor-thread hit testing.
//
// Recording:
//   webrtc-02-hover-traverse.webm shows the cursor entering each
//   concentric box, the :hover CSS lighting up, and the ticker rolling
//   the mouseenter/mouseleave events.

import { runScenario, sleep } from "./_lib.mjs";

// hover-zones panel: top:56 right:16, width:320 height:280 → so its
// CSS-rect is left=944 top=56 in a 1280-wide viewport.
//   outer: full panel        (944..1264, 56..336)
//   mid:   inset 32          (976..1232, 88..304)
//   inner: inset 64          (1008..1200, 120..272)
//   core:  inset 96          (1040..1168, 152..240)
const PT = {
  outsideLeft: { x: 880, y: 196 }, // just left of outer
  outerEdge:   { x: 952, y: 196 }, // inside outer, outside mid
  midEdge:     { x: 984, y: 196 }, // inside mid, outside inner
  innerEdge:   { x: 1016, y: 196 }, // inside inner, outside core
  core:        { x: 1100, y: 196 }, // inside core
};

export const scenario = {
  name: "02-hover-traverse",
  page: "fixture-input-mirror.html",

  async run(s) {
    // Start outside.
    await s.user.mouseMove(PT.outsideLeft.x, PT.outsideLeft.y);
    await sleep(150);
    await s.runtimeEval(`window.__events.length = 0; null`);

    s.marker("enter-outer");
    await s.user.mouseMove(PT.outerEdge.x, PT.outerEdge.y);
    await sleep(120);

    s.marker("enter-mid");
    await s.user.mouseMove(PT.midEdge.x, PT.midEdge.y);
    await sleep(120);

    s.marker("enter-inner");
    await s.user.mouseMove(PT.innerEdge.x, PT.innerEdge.y);
    await sleep(120);

    s.marker("enter-core");
    await s.user.mouseMove(PT.core.x, PT.core.y);
    await sleep(120);

    // Walk back out the way we came.
    s.marker("leave-core");
    await s.user.mouseMove(PT.innerEdge.x, PT.innerEdge.y);
    await sleep(120);

    s.marker("leave-inner");
    await s.user.mouseMove(PT.midEdge.x, PT.midEdge.y);
    await sleep(120);

    s.marker("leave-mid");
    await s.user.mouseMove(PT.outerEdge.x, PT.outerEdge.y);
    await sleep(120);

    s.marker("leave-outer");
    await s.user.mouseMove(PT.outsideLeft.x, PT.outsideLeft.y);
    await sleep(200);

    const events = await s.readEvents();
    const enters = events.filter((e) => e.type === "mouseenter").map((e) => e.zone);
    const leaves = events.filter((e) => e.type === "mouseleave").map((e) => e.zone);

    s.assert("entered each zone exactly once in nesting order",
      () => JSON.stringify(enters) === JSON.stringify(["outer", "mid", "inner", "core"]),
      { expected: ["outer","mid","inner","core"], actual: enters });

    s.assert("left each zone exactly once in reverse order",
      () => JSON.stringify(leaves) === JSON.stringify(["core", "inner", "mid", "outer"]),
      { expected: ["core","inner","mid","outer"], actual: leaves });

    s.assert("never re-entered an ancestor while hovering a descendant",
      () => {
        // After "outer" enter, no further "outer" enter until the
        // matching leave. Same for mid/inner.
        const seq = events
          .filter((e) => e.type === "mouseenter" || e.type === "mouseleave")
          .map((e) => `${e.type}:${e.zone}`);
        const last = {};
        for (const tok of seq) {
          const [type, zone] = tok.split(":");
          if (type === "mouseenter" && last[zone] === "in") return false;
          last[zone] = type === "mouseenter" ? "in" : "out";
        }
        return true;
      });
  },
};

if (import.meta.url === `file://${process.argv[1]}`) runScenario(scenario);

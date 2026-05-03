// 11 — agent Page.navigate during user typing
//
// User is typing into a field. Mid-stream, the agent issues a
// Page.navigate to a fresh URL. What happens?
//
// Production reality: the LLM agent may decide a goto is needed while
// the human user is still entering text in a form. The current
// chromeless behaviour is "navigate wins, in-flight typing is lost"
// — but that contract isn't tested anywhere. This scenario pins the
// behaviour:
//
//   * The user's keystrokes BEFORE navigate must be visible in
//     window.__events captured prior to the navigation.
//   * After navigate, window.__events resets (new document).
//   * The user's keystrokes AFTER navigate land on the new document's
//     focusable elements — proving the input dispatch keeps working
//     across the navigation boundary, not just the first document.
//
// Trickiest part: chrome's CDP queue may swallow keyDown events sent
// while a navigation is mid-flight, because the renderer is being
// torn down. The harness inserts a small dwell after navigate and
// re-checks focus before continuing.

import { runScenario, sleep } from "./_lib.mjs";

const TEXT1_FOCUS = { x: 700, y: 412 };

export const scenario = {
  name: "11-agent-navigate-during-typing",
  page: "fixture-input-mirror.html",

  async run(s) {
    const agent = await s.openSecondaryCdpSession("agent");

    // Phase 1: focus + start typing
    s.marker("user-focus-text1");
    await s.user.click(TEXT1_FOCUS.x, TEXT1_FOCUS.y);
    await sleep(150);
    await s.runtimeEval(`window.__events.length = 0; null`);

    s.marker("user-types-pre-nav");
    await s.user.type("before-", { intervalMs: 16 });
    await sleep(80);

    // Snapshot what was typed before nav.
    const preNavValue = await s.runtimeEval(
      `document.getElementById("text1").value`,
    );

    // Phase 2: agent navigates while user is "thinking". Same-origin
    // navigation to a different query string keeps us on the same
    // fixture page (so we can re-find #text1) but force a real
    // document swap.
    s.marker("agent-navigate");
    const navUrl = await s.runtimeEval(
      `location.origin + location.pathname + "?phase=post-nav"`,
    );
    await agent.session.Page.navigate({ url: navUrl });

    // Wait for the page to come back up. The fixture's bridge sets
    // window.__cbtest.ready=true once the streamer offer is sent;
    // for a re-navigation we don't re-establish WebRTC, so we just
    // poll for window.__events to exist (proves the new document's
    // script ran).
    const navSettleDeadline = Date.now() + 8000;
    while (Date.now() < navSettleDeadline) {
      const has = await s.runtimeEval(
        `typeof window.__events === "object" ? "yes" : "no"`,
      ).catch(() => "err");
      if (has === "yes") break;
      await sleep(200);
    }
    await sleep(300); // small dwell for layout

    // Phase 3: re-focus text1 on the new document and continue typing.
    s.marker("user-types-post-nav");
    await s.user.click(TEXT1_FOCUS.x, TEXT1_FOCUS.y);
    await sleep(150);
    await s.user.type("after", { intervalMs: 16 });
    await sleep(150);

    const postNavValue = await s.runtimeEval(
      `document.getElementById("text1").value`,
    );
    const postNavUrl = await s.runtimeEval(`location.search`);

    s.assert("pre-nav typing landed in text1 (visible before navigate)",
      () => preNavValue === "before-",
      { expected: "before-", actual: preNavValue });

    s.assert("agent's navigate landed (location.search changed)",
      () => postNavUrl === "?phase=post-nav",
      { expected: "?phase=post-nav", actual: postNavUrl });

    s.assert("user's post-nav typing landed in the new document's text1",
      () => postNavValue === "after",
      { expected: "after", actual: postNavValue });

    // The whole point of this scenario is to prove the new document
    // received fresh keyboard events. Read the new document's event
    // log — it should NOT contain the pre-nav keystrokes (those went
    // to the discarded document).
    const newEvents = await s.readEvents();
    const postNavKeydowns = newEvents
      .filter((e) => e.type === "keydown" && e.target === "text1");

    s.assert("new document's event log only contains post-nav keystrokes",
      () => {
        // Count printable letters typed in "after" = 5 keydowns.
        // The keylog could include the click+focus events too, which
        // we ignore by filtering on target=text1 + key length 1.
        const printable = postNavKeydowns.filter((e) => e.key.length === 1);
        return printable.length === 5;
      },
      { expected: "5 printable keydowns on text1 ('after')",
        actual: postNavKeydowns.map((e) => e.key) });
  },
};

if (import.meta.url === `file://${process.argv[1]}`) runScenario(scenario);

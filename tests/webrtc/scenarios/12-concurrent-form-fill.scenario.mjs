// 12 — concurrent form fill conflict (agent + user typing into same field)
//
// User is typing into text1 char-by-char. Concurrently, the agent
// uses Input.insertText (the IME-batched fast-path) to drop a chunk
// of text into the same field. What does the page see?
//
// Production reality: an LLM agent calling
// `BrowserOpsExecutor::fill(selector, value)` ultimately funnels into
// Input.insertText (or a select-all + insertText combo). If the user
// is concurrently typing, the two paths interleave at the renderer's
// IME-handling layer.
//
// Key questions this scenario pins:
//   * Does Input.insertText fire keydown/keyup events? (chromium docs
//     say no — it's the IME path; this scenario asserts that and so
//     a regression that started firing keys would be caught.)
//   * If the agent inserts mid-stream, do user keys after the insert
//     append to the agent's text or replace it? (chromium behaviour:
//     append — caret follows insertion.)
//   * What's the final field value when both finish? Last writer wins
//     OR concatenation? Pin it.

import { runScenario, sleep } from "./_lib.mjs";

const TEXT1_FOCUS = { x: 700, y: 412 };

export const scenario = {
  name: "12-concurrent-form-fill",
  page: "fixture-input-mirror.html",

  async run(s) {
    const agent = await s.openSecondaryCdpSession("agent");

    s.marker("user-focus-text1");
    await s.user.click(TEXT1_FOCUS.x, TEXT1_FOCUS.y);
    await sleep(120);
    await s.runtimeEval(`window.__events.length = 0; null`);

    s.marker("user-types-prefix");
    await s.user.type("u-", { intervalMs: 18 });
    await sleep(50);

    // Agent batches a chunk via insertText — analogous to what
    // BrowserOpsExecutor::fill does internally.
    s.marker("agent-inserts");
    await agent.session.Input.insertText({ text: "AGENT-" });

    // User continues typing — should append to whatever the field
    // currently contains.
    s.marker("user-types-suffix");
    await s.user.type("u-end", { intervalMs: 18 });
    await sleep(150);

    const finalValue = await s.runtimeEval(
      `document.getElementById("text1").value`,
    );

    const events = await s.readEvents();
    const keydowns  = events.filter((e) => e.type === "keydown" && e.target === "text1");
    const inputEvts = events.filter((e) => e.type === "input"   && e.target === "text1");

    s.assert("final field value reflects user-prefix + agent-insert + user-suffix",
      () => finalValue === "u-AGENT-u-end",
      { expected: "u-AGENT-u-end", actual: finalValue });

    s.assert("Input.insertText did NOT fire keydown events",
      () => {
        // Count printable keydowns (the user's "u-" + "u-end" = 6 chars,
        // but the keydowns include modifier-style events for the dash).
        // We expect ZERO keydowns for the agent's "AGENT-" insertion.
        // Easiest check: the count of keydowns matches the user's
        // typed character count exactly.
        const userTypedChars = "u-u-end".length; // 7
        return keydowns.length === userTypedChars;
      },
      { expected: "7 keydowns (user typed); 0 from insertText",
        actual: { keydowns: keydowns.length,
                  keys: keydowns.map((e) => e.key) } });

    s.assert("input events fired for both paths (user keys + agent insert)",
      () => {
        // The page's `input` event listener fires regardless of source
        // — this is the listener a React-style controlled component
        // would attach. So this catches BOTH user keystrokes AND
        // agent insertText.
        return inputEvts.length >= 8; // 7 user chars + 1 batched insert
      },
      { expected: ">=8 input events", actual: inputEvts.length });

    s.assert("input event sequence shows interleaving (agent insert appears mid-stream)",
      () => {
        const values = inputEvts.map((e) => e.value);
        // We expect some prefix like ["u","u-"] then "u-AGENT-" then
        // "u-AGENT-u" then "u-AGENT-u-" etc. The key signature: at
        // some point the value jumps by more than one char (the agent
        // batched insert).
        for (let i = 1; i < values.length; i++) {
          if (values[i].length - values[i - 1].length > 1) return true;
        }
        return false;
      },
      { expected: "value-length jump > 1 in input sequence",
        actual: inputEvts.map((e) => e.value) });
  },
};

if (import.meta.url === `file://${process.argv[1]}`) runScenario(scenario);

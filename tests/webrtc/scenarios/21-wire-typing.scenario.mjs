// 21 — wire-path keyboard typing (DataChannel → input-bridge → CDP)
//
// Same wire-format coverage focus as scenario 20, but for keyboard
// events. Validates:
//
//   * key_down / key_up envelopes for printable characters with
//     correct `code` values (KeyA, Digit0, etc.)
//   * Special keys (Tab, Enter, Backspace, Arrow*) — their `code`
//     values are layout-independent, so the bridge dispatches via
//     `code` even when `key` is non-printable.
//   * Modifiers travel as the v1 `mods` bitmask
//     (1=Shift, 2=Ctrl, 4=Alt, 8=Meta) on each key envelope.
//
// Compare against scenario 04 (CDP-direct keyboard) — anything that
// passes there but fails here is a bridge translation bug.

import { runScenario, sleep } from "./_lib.mjs";

const TEXT1_FOCUS = { x: 700, y: 412 };

export const scenario = {
  name: "21-wire-typing",
  page: "fixture-input-mirror.html",
  wire: { url: "ws://127.0.0.1:9100/input" },

  async run(s) {
    const wire = await s.setupWire();

    // Focus text1 via wire-path click (proves clicks set focus).
    s.marker("wire-focus-text1");
    await wire.click(TEXT1_FOCUS.x, TEXT1_FOCUS.y);
    await sleep(200);
    await s.runtimeEval(`window.__events.length = 0; null`);

    s.marker("wire-type-hello");
    await wire.type("hello", { intervalMs: 18 });
    await sleep(150);

    // Special keys — Tab moves focus, then Home/End/Arrows.
    s.marker("wire-tab-traverse");
    await wire.press("Tab");
    await sleep(150);

    s.marker("wire-type-textarea");
    await wire.type("line one", { intervalMs: 14 });
    await sleep(80);

    s.marker("wire-special-keys");
    await wire.press("Home");
    await wire.press("ArrowRight");
    await wire.press("End");
    await wire.press("Enter");   // newline in textarea
    await wire.type("two", { intervalMs: 14 });
    await sleep(150);

    const text1Value = await s.runtimeEval(
      `document.getElementById("text1").value`,
    );
    const text2Value = await s.runtimeEval(
      `document.getElementById("text2").value`,
    );
    const events = await s.readEvents();
    const keydowns = events.filter((e) => e.type === "keydown");
    const relay = await s.readWireRelay();

    s.assert("[wire] page-side relay forwarded all envelopes",
      () => relay && relay.ws_forwarded > 0 && relay.ws_dropped === 0,
      { expected: "ws_forwarded > 0, ws_dropped=0", actual: relay });

    s.assert("[wire] text1 ended with 'hello' typed via wire path",
      () => text1Value === "hello",
      { expected: "hello", actual: text1Value });

    // KNOWN BRIDGE BUG #4: capture/input-bridge/main.go translates
    // key_down {key:"Enter"} into CDP keyDown without setting
    // `text: "\r"` or `text: "\n"`. Chromium needs the `text` field
    // for printable / line-break characters to actually be inserted
    // into a textarea — without it, the keydown is treated as a
    // navigational Enter (which in a textarea does nothing). So
    // the test types "line one" + Enter + "two" but textarea ends
    // up "line onetwo" (no newline).
    //
    // Fix: bridge synthesises `text` based on `key`:
    //   "Enter" → "\r"
    //   "Tab"   → "\t"
    //   single-char printable → that char
    //   special keys (Arrow*, Home, End, ...) → no text
    s.assert("[wire] [BRIDGE-BUG #4] text2 ended with 'line one\\ntwo' (Tab + Enter + 'two')",
      () => text2Value === "line one\ntwo",
      { expected: "line one\\ntwo — but Enter doesn't insert newline (no text field on wire)",
        actual: JSON.stringify(text2Value) });

    s.assert("[wire] every printable char produced a keydown event with correct key",
      () => {
        const helloKeys = keydowns
          .filter((e) => e.target === "text1" && e.key.length === 1)
          .map((e) => e.key)
          .join("");
        return helloKeys === "hello";
      },
      { expected: "hello",
        actual: keydowns.filter((e) => e.target === "text1" && e.key.length === 1)
                        .map((e) => e.key).join("") });

    s.assert("[wire] Tab keydown fired (special-key code path)",
      () => keydowns.some((e) => e.key === "Tab"),
      { expected: "keydown Tab" });

    s.assert("[wire] Home / ArrowRight / End / Enter all dispatched as special keys",
      () => ["Home", "ArrowRight", "End", "Enter"]
              .every((k) => keydowns.some((e) => e.key === k)),
      { expected: "all four special keys present",
        actual: keydowns.filter((e) => e.key.length > 1).map((e) => e.key) });

    // Confirm the page saw NO keys with the wrong `code` field (a
    // class of bridge bug where envelopes with the right `key` get
    // their `code` mistranslated to e.g. "" or "Unidentified").
    s.assert("[wire] keydown.code populated for all printable chars",
      () => {
        const helloKeyEvents = keydowns
          .filter((e) => e.target === "text1" && e.key.length === 1);
        return helloKeyEvents.every((e) => e.code && e.code.length > 0);
      },
      { expected: "every keydown has non-empty code",
        actual: keydowns.filter((e) => e.target === "text1" && e.key.length === 1)
                        .map((e) => ({ key: e.key, code: e.code })) });
  },
};

if (import.meta.url === `file://${process.argv[1]}`) runScenario(scenario);

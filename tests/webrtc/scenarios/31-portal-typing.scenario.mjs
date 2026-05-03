// 31 — portal-encoder keyboard typing (DOM key event → portal encoder → WS)
//
// Companion to scenario 30. Where 30 covers mouse-event encoding,
// this scenario covers the key-event encoder:
//
//   CDP Input.dispatchKeyEvent (test driver, primary CDP session)
//     → DOM keydown / keyup on focused canvas
//     → triform_input_encoder::web::from_key_event   ◀── PRODUCTION CODE PATH
//     → JSON `{"type":"key","action":"down|up","key":…,"code":…,
//                "keyCode":…,"location":…,"modifiers":…,"text":…}`
//     → WebSocket → stub WS server in the runner
//     → assertions
//
// Two regression classes pinned here that the encoder lift fixed:
//
//   1. **Non-ASCII printable text** — the original portal encoder
//      gated `text` emission on `key.len() == 1` (BYTE length).
//      A printable like "é" (1 char / 2 bytes) sent empty text to
//      physics, which then dispatched a CDP keydown without
//      `text:"é"` — chromium's renderer never typed the character.
//      The encoder now uses `chars().count() == 1`. We verify by
//      typing a non-ASCII printable and asserting `text` is set.
//
//   2. **Keyup must carry empty text** — CDP `Input.dispatchKeyEvent`
//      semantics: only keydown inserts text; emitting `text` on
//      keyup causes double-typing. Pinned by asserting keyup's
//      `text` is "" even when keydown had it.

import { runPortalScenario, sleep, MOD } from "./_lib_portal.mjs";
import process from "node:process";

// CDP key dispatch helper — the InputBag's `keyDown(key, mods)` /
// `keyUp(key, mods)` take a string key + a mods bag like
// `{ shift: true }`, not a descriptor object. The bag's
// `keyDescriptor()` resolves Enter/Tab/etc to KEY_MAP entries and
// single ASCII / BMP chars get a synthesised descriptor.
async function pressKey(s, key, mods = {}) {
  await s.user.keyDown(key, mods);
  await s.user.keyUp(key, mods);
}

export const scenario = {
  name: "31-portal-typing",

  async run(s) {
    s.resetWire();

    // Focus the canvas so keydown / keyup route to the encoder's
    // listeners. CDP key dispatch goes to whatever element has focus
    // in the page; without this the page-level listener never fires.
    await s.focusTarget();

    // ── 1. ASCII printable: "a" ───────────────────────────────────
    await pressKey(s, "a");
    let wire = await s.waitForWireCount(2);
    const downA = wire.find((m) => m.type === "key" && m.action === "down");
    const upA = wire.find((m) => m.type === "key" && m.action === "up");

    s.assert(
      "ASCII 'a' down: key=a code=KeyA keyCode=65 text=a",
      () =>
        downA &&
        downA.key === "a" &&
        downA.code === "KeyA" &&
        downA.keyCode === 65 &&
        downA.text === "a",
      { expected: "key=a code=KeyA keyCode=65 text=a", actual: downA },
    );

    s.assert(
      "ASCII 'a' up: text MUST be empty (CDP semantics)",
      () => upA && upA.text === "",
      {
        expected: "text=''",
        actual: upA?.text,
        why: "Keyup carrying text causes double-typing in CDP renderer",
      },
    );

    // ── 2. Non-ASCII printable: "é" ───────────────────────────────
    // Pinning the `.chars().count() == 1` fix. Old code with
    // `.len() == 1` (byte length) would have produced text=""
    // because "é" is 2 bytes UTF-8.
    s.resetWire();
    await pressKey(s, "é");
    wire = await s.waitForWireCount(2);
    const downE = wire.find((m) => m.type === "key" && m.action === "down");

    s.assert(
      "non-ASCII 'é' down: text carries the multi-byte character (chars-count fix)",
      () => downE && downE.text === "é" && downE.key === "é",
      {
        expected: "text='é'",
        actual: downE,
        why: "Encoder must use chars().count() not len() to keep multi-byte printables",
      },
    );

    // ── 3. Special key: Enter ─────────────────────────────────────
    // Special keys (Enter, Tab, Escape) are NOT printable — text must
    // be empty even on keydown. The `len()==1` bug never affected
    // these (their `key` strings are multi-char), but pinning the
    // shape prevents a regression where someone "fixes" the encoder
    // to emit text on every keydown.
    s.resetWire();
    await pressKey(s, "Enter");
    wire = await s.waitForWireCount(2);
    const downEnter = wire.find((m) => m.type === "key" && m.action === "down");

    s.assert(
      "special key Enter: text MUST be empty even on keydown",
      () => downEnter && downEnter.text === "" && downEnter.key === "Enter",
      { expected: "text='' key='Enter'", actual: downEnter },
    );

    // ── 4. Modifier-bearing key: Shift+a ──────────────────────────
    // The encoder reads modifier state via `kb_modifier_flags(&e)`,
    // which packs Alt=1, Ctrl=2, Meta=4, Shift=8. Pinning the bit
    // values prevents a CDP modifier-bit reshuffle from silently
    // breaking shift-click / ctrl-click / cmd-click semantics.
    s.resetWire();
    await pressKey(s, "A", { shift: true });
    wire = await s.waitForWireCount(2);
    const downShifted = wire.find((m) => m.type === "key" && m.action === "down");

    s.assert(
      "Shift+A: encoder reports modifiers=8 (Shift bit)",
      () =>
        downShifted &&
        downShifted.modifiers === MOD.Shift &&
        (downShifted.modifiers & MOD.Shift) === MOD.Shift,
      {
        expected: `modifiers=${MOD.Shift}`,
        actual: downShifted?.modifiers,
      },
    );

    // ── 5. Wire-tag pin: every key message has type='key' ─────────
    const allKey = s.receivedWire().filter((m) => m.type === "key");
    s.assert(
      "wire shape: every key message has type='key' and a string action",
      () =>
        allKey.length > 0 &&
        allKey.every(
          (m) =>
            m.type === "key" &&
            (m.action === "down" || m.action === "up") &&
            typeof m.key === "string" &&
            typeof m.code === "string",
        ),
      { expected: "shape ok", actual: allKey.length },
    );

    // ── 6. Page-side cumulative tally ─────────────────────────────
    // 4 key presses × 2 events (down + up) = 8 encoder firings. The
    // stub-side count is cleared per section, so cbtest's cumulative
    // sent array is the right comparison surface.
    const cb = await s.readCbtest();
    s.assert(
      "page-side cbtest recorded all 8 encoder firings",
      () => cb && cb.sent && cb.sent.length === 8,
      { expected: "cbtest.sent.length === 8", actual: cb?.sent?.length },
    );
    s.assert(
      "page-side key counter matches",
      () => cb && cb.counts && cb.counts.key === 8,
      { expected: "counts.key === 8", actual: cb?.counts?.key },
    );
  },
};

if (import.meta.url === `file://${process.argv[1]}`) {
  runPortalScenario(scenario);
}

# tests/webrtc/scenarios — input-dispatch test framework

Scenario-based input tests that exercise CDP `Input.dispatch*` paths
against an instrumented page, record each scenario to its own webm,
and emit an HTML index for human review.

```
scenarios/
├── _lib.mjs                  scenario runner foundation
├── fixture-input-mirror.html instrumented page (window.__events log)
├── runner.mjs                discovers + runs all *.scenario.mjs
├── 01-mouse-clicks.scenario.mjs           single / double / right / shift-click
├── 02-hover-traverse.scenario.mjs         nested-DOM mouseenter / mouseleave
├── 03-drag-and-drop.scenario.mjs          HTML5 D&D (the trickiest path)
├── 04-keyboard-typing.scenario.mjs        typing + modifiers + special keys
├── 05-wheel-scroll.scenario.mjs           vertical / horizontal / shift+wheel
├── 10-concurrent-cursor-fight.scenario.mjs    sequential + concurrent CDP
├── 11-agent-navigate-during-typing.scenario.mjs  agent Page.navigate mid-typing
└── 12-concurrent-form-fill.scenario.mjs   user typing + agent insertText race
```

## Running

```bash
# Local: against a chromium with --remote-debugging-port=9222.
TEST_ARTIFACTS_DIR=/tmp/cb-input-test \
  node scenarios/runner.mjs

# Single scenario (each file is independently runnable):
node scenarios/01-mouse-clicks.scenario.mjs

# Subset by name:
node scenarios/runner.mjs --only=concurrent
node scenarios/runner.mjs --skip=keyboard --fail-fast

# Override target:
node scenarios/runner.mjs --cb-url=http://chromium.cluster.local:9222
```

After a run, open `${TEST_ARTIFACTS_DIR}/index.html` to scrub through
each recording with its assertion table inline.

## Per-scenario artifacts

Each scenario emits three files into the artifacts dir:

| File | Contents |
|---|---|
| `webrtc-<scenario>.webm` | VP9 recording of the scenario's run, captured via `canvas.captureStream(30)` |
| `events-<scenario>.json` | `window.__events` array — every input event the page received |
| `summary-<scenario>.json` | metadata: assertions (with pass/fail), markers, frame count, duration |

The runner also writes a top-level `manifest.json` and `index.html`.

## Production findings encoded as assertions

These tests pin behaviours that surface only when you run real CDP
input dispatch against real chromium — and that production code paths
depend on but rarely test:

* **Concurrent CDP serialisation** (scenario 10) — chromium has a
  single pointer state per RenderFrame. Two CDP sessions issuing
  `Input.dispatchMouseEvent` via `Promise.all()` interleave at the
  wire and produce drag-shaped event sequences, not clicks. Pinned:
  10 mousedowns + 10 mouseups but 0 synthesised clicks under
  concurrent dispatch. Sequential alternation works perfectly.

* **Headless browser-chrome shortcut bypass** (scenario 04) — Cmd+A /
  Ctrl+A keydown events fire correctly with the right modifier bits,
  but the browser-chrome layer that translates the shortcut into an
  actual `selectAll` command does NOT run in headless mode. Pages
  that rely on the keydown event work; pages that rely on the
  command router don't.

* **Input.insertText bypasses keyboard event dispatch** (scenario 12)
  — the IME-batched insert path (used by `BrowserOpsExecutor::fill`
  in production) does NOT fire `keydown` events; it only fires the
  `input` event. Pages that listen on `keydown` to validate input
  miss agent-driven fills entirely.

* **Sibling vs nested hover stacking** (scenario 02 + fixture) —
  mouseenter/mouseleave fire on the topmost element under cursor.
  In nested DOM, that's the deepest ancestor. In sibling stacking,
  that's whichever sibling has the highest z-index at that point.
  Production hover state machines almost always assume nested
  semantics — getting this wrong produces tooltip flicker and
  dropdown re-positioning bugs.

* **Agent navigation kills in-flight user input** (scenario 11) —
  when the agent fires `Page.navigate` mid-typing, the renderer
  tears down. The user's pre-nav keystrokes are committed in the
  outgoing document; post-nav keystrokes need a re-focus on the
  new document. The CDP `Input.dispatchKeyEvent` path keeps working
  across the navigation boundary.

## Adding a new scenario

A scenario is one `.scenario.mjs` file exporting:

```js
import { runScenario, sleep } from "./_lib.mjs";

export const scenario = {
  name: "13-my-test",
  page: "fixture-input-mirror.html",   // or your own HTML

  async run(s) {
    s.marker("phase-1");
    await s.user.click(100, 200);
    await sleep(120);

    // Optionally open an "agent" CDP session for concurrency tests.
    const agent = await s.openSecondaryCdpSession("agent");
    await agent.click(300, 400);

    const events = await s.readEvents();
    s.assert("got my expected event",
      () => events.some((e) => e.type === "click" && e.zone === "tl"),
      { expected: "click on tl", actual: events.filter((e) => e.type === "click") });
  },
};

if (import.meta.url === `file://${process.argv[1]}`) runScenario(scenario);
```

The harness runs each scenario in isolation: fresh BrowserContext,
fresh Target, fresh recording. No state leaks between scenarios.

## Input dispatch helpers (`s.user.*`, `agent.*`)

| Method | Maps to CDP |
|---|---|
| `mouseMove(x, y, mods?)` | `Input.dispatchMouseEvent {type: "mouseMoved"}` |
| `mouseDown(x, y, opts?)` | `Input.dispatchMouseEvent {type: "mousePressed"}` |
| `mouseUp(x, y, opts?)` | `Input.dispatchMouseEvent {type: "mouseReleased"}` |
| `click(x, y, opts?)` | mouseDown + mouseUp |
| `doubleClick(x, y, opts?)` | click + click with clickCount=2 |
| `drag(x1, y1, x2, y2, opts?)` | mouseDown + N×mouseMove + mouseUp (correctly ramped for HTML5 D&D detection) |
| `wheel(x, y, dx, dy, mods?)` | `Input.dispatchMouseEvent {type: "mouseWheel"}` |
| `keyDown(key, mods?)` / `keyUp` / `press(key, mods?)` | `Input.dispatchKeyEvent` |
| `type(text, opts?)` | per-char keyDown + keyUp |
| `insertText(text)` | `Input.insertText` (IME path; no keyboard events) |

Modifier bag: `{ alt, ctrl, meta, shift }` → CDP modifier bitfield.
Button bag: `{ button: "left"|"middle"|"right", clickCount: 1|2|3 }`.

## Wire-path scenarios (production roundtrip)

Scenarios `20-22` exercise the **full production wire path**:

```
harness.dc.send(envelope)
  → page.dc.onmessage
  → page.InputRelay.forward()
  → ws://127.0.0.1:9100/input
  → input-bridge sidecar
  → CDP Input.dispatchMouseEvent / dispatchKeyEvent
  → renderer
  → page event listener → window.__events
```

Local prerequisites:
```bash
# Build input-bridge (Go binary):
cd /private/tmp/chromeless/capture/input-bridge && go build -o /tmp/input-bridge .

# Start chromium with remote debugging:
chromium --headless=new --remote-debugging-port=9222 \
         --remote-allow-origins=* --window-size=1280,720 about:blank &

# Start input-bridge AFTER chromium is up:
/tmp/input-bridge --source ws --cdp-url http://127.0.0.1:9222 &

# Run wire-only scenarios:
TEST_ARTIFACTS_DIR=/tmp/cb-wire-test \
  node scenarios/runner.mjs --only=wire
```

### Bridge bugs surfaced by the wire tests

The wire scenarios surface **four genuine production bugs** in
`capture/input-bridge/main.go` — these aren't test errors, they're
invariants the bridge must hold and currently doesn't:

| Bug | Location | Symptom | Fix |
|---|---|---|---|
| **#1 clickCount** | `main.go:554` | `mouse_button` always emits `clickCount: 1` → no dblclick events | Track time-of-last-mousedown, ramp clickCount on rapid successive clicks (~500ms threshold) |
| **#2 modifiers** | `main.go:555` | `mouse_button` always emits `modifiers: 0` → shift-click loses modifier on the wire | Maintain held-key state machine across `key_down`/`key_up` envelopes; OR-mask into `modifiers` on mouse events |
| **#3 buttons during drag** | `main.go:528` | `mouse_move` always emits `button: "none", buttons: 0` → drag detector aborts → no dragstart/drag/drop fires | Track pointer-button state across messages; OR last-down-without-up into `buttons` on mouse_move CDP dispatch |
| **#4 Enter newline** | `main.go` keyboard path | `key_down {key:"Enter"}` dispatched without `text: "\r"` → textarea doesn't receive newline | Synthesise `text` from `key`: Enter→\r, Tab→\t, printable single char→itself |

A green run of `--only=wire` against a patched bridge means all four
fixes landed. Ship those fixes upstream in the bridge crate —
**this scenario set is the regression suite** that proves the
production wire path holds under real chromium dispatch.

## Cluster integration

The CDP-direct scenarios (01–12) run via the existing
`cb-webrtc-validation.yaml` Job — no input-bridge needed.

The wire scenarios (20+) run via `cb-webrtc-wire-bridge-validation.yaml`
— same shape but with an additional `input-bridge` sidecar
container alongside cb-chromium and the test-driver.

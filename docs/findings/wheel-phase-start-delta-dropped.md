# The first wheel event of every scroll gesture is discarded

**Status:** confirmed against a live deployment. Fix is in `capture/` (C++),
which cannot be compiled in this repo — see "Fixing it" below.

**Impact:** slow or deliberate scrolling does nothing at all. Fast continuous
scrolling works after the first event. A user who nudges the wheel once, pauses,
and nudges again never scrolls.

## What happens

`client/src/input.ts` runs a wheel phase machine (protocol v1.1). The first
non-zero wheel event after a quiet period is sent as `phase: "start"`, and it
**carries a real delta**:

```jsonc
{ "dx": 0, "dy": 300, "mode": 0, "delta_mode": "pixel",
  "phase": "start", "momentum": false, "x": 612, "y": 401 }
```

`capture/build-integration/cb_input_dispatch_mouse.cc` throws that delta away:

```cpp
if (*phase == "start") {
  blink_phase = blink::WebMouseWheelEvent::kPhaseBegan;
  wheel_in_gesture_ = true;
  // ... The same envelope's dx/dy (if
  // non-zero) would be lost here, but in practice phase=start
  // envelopes carry dx=dy=0 — the next envelope (phase=changed
  // or phase omitted) carries the first real delta.
  synth_zero_delta = true;
}
```

The comment states the assumption plainly, and the assumption is wrong. It is
also contradicted by this repo's own spec, `docs/protocols/input-channel.md`:

> **`start`** — first **non-zero** wheel event after a quiet period

## Why it looks intermittent

The gesture returns to idle after `DEFAULT_WHEEL_END_DELAY_MS = 150` ms of
silence (`client/src/input.ts`). So:

| scrolling style | wheel spacing | what the server sees | result |
| --- | --- | --- | --- |
| fast, continuous | < 150 ms | `start` (dropped), then `changed`… | works, minus the first event |
| slow or deliberate | > 150 ms | `start`, `start`, `start`, … | **nothing scrolls, ever** |

That is why this survived: anyone flicking a trackpad sees scrolling work.

## How it was found

`tests/interactive/` dispatches real DOM events at the client and reads the
result from the WORKER's own DevTools. Four wheel events at 350 ms spacing left
`window.scrollY` at 0. The same page scrolled correctly when a wheel was
injected directly through the worker's CDP (`scrollY: 400`), which isolates the
fault to the chromeless input path rather than to Blink or the page.

## Fixing it

In `cb_input_dispatch_mouse.cc`, `phase == "start"` should dispatch the
envelope's actual delta rather than forcing zero. If the compositor genuinely
needs a zero-delta `kPhaseBegan` before its first real delta, then emit **two**
events — the synthetic Begin and then the carried delta as `kPhaseChanged` —
instead of dropping the user's input.

Not fixed here: `capture/` is an out-of-tree Chromium embedder needing a 4–8 h
build (see CLAUDE.md), so any change would be unverified. This is written down
rather than guessed at.

A regression test belongs in `tests/interactive/run.py`, which already covers
it — the check is `wheel scrolls the remote page down`, currently failing by
design until the embedder is fixed.

## Related

The same file's `phase == "end"` branch also forces a zero delta. That one is
correct: `emitWheelEnd()` in `input.ts` genuinely sends `dx: 0, dy: 0`.

---

## Three related gaps found in the same run

Each was isolated the same way: inject the interaction directly at the worker
over CDP. If it works there, Blink and the page are fine and the fault is in
the chromeless input path.

| interaction | at the worker (CDP) | through the input channel | verdict |
| --- | --- | --- | --- |
| click | ✓ | ✓ | works |
| typing, backspace, Ctrl+A | ✓ | ✓ | works |
| **wheel / scroll** | ✓ `scrollY: 400` | ✗ `scrollY: 0` | **the bug above** |
| **hover → `mouseover`** | ✓ fired | ✗ never fires | needs investigation |
| **Tab then Space (checkbox)** | ✓ focus moves, box toggles | ✗ box stays unchecked | needs investigation |

The hover case is worth a look on its own: `mouse_move` envelopes clearly do
arrive, because clicking works and the client sends a `mousemove` before every
`mousedown`. So the coordinates land — but a bare move with no button appears
not to produce a `mouseover` on the remote page.

Tab-then-Space may be the same root cause as the wheel: both are non-printing
keys, and printable characters demonstrably work. Worth checking whether
`key_down` for `Tab`/`Space` reaches `cb_input_dispatch_keyboard.cc` with the
`code` the dispatcher expects.

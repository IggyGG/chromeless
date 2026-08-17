# The first wheel event of every scroll gesture is discarded

**Status:** FIXED and VERIFIED WORKING against a live deployment (image
`cr7727-224c19413e24`, 2026-08-17):

```
PASS  wheel scrolls the remote page down   scrollY 0 -> 1280
PASS  wheel scrolls back up                scrollY 1280 -> 0
```

**It took TWO fixes, and the first one uncovered the second.** Discarding the
start-delta (below) was one bug; a sign inversion between the protocol and
Blink was another, older one that the first bug had been hiding. With every
start-delta thrown away, slow scrolling produced no motion at all — so there
was never a direction to be wrong about. Only once the delta survived did the
page move, upward, from `scrollY 0`, which looks exactly like nothing
happening. See "The sign inversion" below.

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

## The sign inversion (the second bug)

Deploying the delta fix and reading what the page actually received:

```
events at page: [{dy:0}, {dy:-320}, {dy:0}, {dy:-320}, {dy:0}, {dy:-320}]
```

Two events per gesture, exactly as designed — the synthetic zero-delta Begin,
then the real delta that used to be dropped. But the test sent `+320`.

- `docs/protocols/input-channel.md`: `dy` matches `WheelEvent.deltaY`, so
  **positive = down**. `client/src/input.ts` forwards `e.deltaY` verbatim.
- `blink::WebMouseWheelEvent::delta_y`: **positive moves the CONTENT down**,
  i.e. scrolls up. The opposite.

Measured on the deployed worker with a 2766px page: `dy=+320` left `scrollY`
at 0; `dy=-320` scrolled to 960. `cb_input_dispatch_mouse.cc` now negates
(`kProtocolToBlinkSign`), applied to `delta_x/y` and `wheel_ticks_x/y` alike —
they describe the same gesture, and a mismatch would tell chromium's smoothing
the opposite of the motion.

**The lesson worth keeping:** a fix that makes a check go from failing to
failing is not necessarily a fix that did nothing. Reading the events the page
received — rather than only the pass/fail — is what separated "the delta is
still being dropped" from "the delta arrives, backwards".

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
| **hover → `mouseover`** | ✓ fired | ✓ fired | **not a bug** — see below |
| **Tab then Space (checkbox)** | ✓ focus moves, box toggles | ✗ neither happens | [`keyboard-dom-code-never-set.md`](./keyboard-dom-code-never-set.md) |

**The hover case was my own test bug**, recorded here because the shape of the
mistake is worth remembering: the check moved the pointer to `#hot` while the
two preceding click assertions had already left it there, and `mouseover` fires
on ENTERING an element. A correct product reported as broken by a test that
never moved the mouse. It passes once the pointer starts somewhere else.

**Tab and Space turned out to be a real and separate defect**, not the same
root cause as the wheel: `dom_code`/`dom_key` are never populated, so Blink
cannot run the default action. Isolated by controlled experiment and written up
in [`keyboard-dom-code-never-set.md`](./keyboard-dom-code-never-set.md).

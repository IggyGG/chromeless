# Non-printing keys do nothing: `dom_code`/`dom_key` are never set

**Status:** FIXED and COMPILED (build lane e654ed6, 2026-08-14, image
`cr7727-e654ed644219`) — including the parts that could not be checked without
a Chromium tree: `ui::KeycodeConverter` resolves, the new
`//ui/events:dom_keycode_converter` dep is correct, and the `dom_key`
conversion compiles. Behaviour not yet re-verified against a deployment;
re-run `tests/interactive/` on that image. The checks are `tab moves remote
focus` and `space toggles the focused remote checkbox`.

**Impact:** Tab does not move focus. Space does not activate a focused
checkbox, button, or link. Arrow keys, Home/End, PageUp/PageDown and Escape are
all dispatched the same way and are very likely affected identically (not
individually confirmed). Typing printable characters works, which is what has
kept this hidden — the keyboard *looks* fine until someone tabs between fields.

For a browser you drive entirely through a video stream, this means keyboard
navigation and accessibility are unavailable.

## Root cause

`capture/build-integration/cb_input_dispatch_keyboard.cc` builds a
`NativeWebKeyboardEvent` from a `ScancodeMapping`:

```cpp
native.windows_key_code = scancode.windows_key_code;
native.native_key_code  = scancode.windows_key_code;
native.dom_code         = scancode.dom_code;   // always 0
native.dom_key          = scancode.dom_key;    // always 0
```

`MapCodeToScancode` sets `windows_key_code` and returns, leaving `dom_code` and
`dom_key` at their zero-initialised defaults for **every** key:

```cpp
if (code == "Enter")     { out->windows_key_code = 0x0D; return true; }
if (code == "Tab")       { out->windows_key_code = 0x09; return true; }
if (code == "Backspace") { out->windows_key_code = 0x08; return true; }
if (code == "Space")     { out->windows_key_code = 0x20; return true; }
```

Two branches assign `out->dom_code = 0` explicitly (the letter and digit
ranges), which reads as deliberate; the rest simply never touch it. Either way
the field is 0 on the wire.

Blink's focus and default-action handling keys off `dom_code`/`dom_key`, not
`windows_key_code`. With them zero, the renderer receives a keydown it cannot
attribute to a physical key, and the default action — move focus, activate the
focused control — never runs. Character insertion still works because it goes
through the separate `kChar` text-synthesis path further down the same
function, which carries the text explicitly.

## The experiment

Both variants below dispatch the *same* `windowsVirtualKeyCode` at the same
worker, to the same page, over CDP. The only difference is the `code`/`key`
params — which are precisely what populate `dom_code`/`dom_key`. Run against
the deployed worker:

```
Tab   WITH code/key  -> focus=box     ← moved
Tab   WITHOUT        -> focus=a       ← did not move
Space WITH code/key  -> checked=True  ← toggled
Space WITHOUT        -> checked=False ← did not toggle
```

"WITHOUT" is what the embedder currently sends. That is the whole defect,
reproduced in four lines with one variable changed.

The same page also confirms the layers underneath are fine: injected *with*
those fields, both keys behave correctly, so Blink, the page, and the focus
model are all working. Compare with the input-channel path, where
`tests/interactive/run.py` reports:

```
FAIL  tab moves remote focus                     activeElement='target'
FAIL  space toggles the focused remote checkbox  checked=False
```

## Fixing it

Populate `dom_code` and `dom_key` in `MapCodeToScancode`. Chromium already has
the tables — `ui::KeycodeConverter::CodeStringToDomCode()` maps the protocol's
`code` string (which is the DOM `KeyboardEvent.code`, exactly what that
function expects) and `ui::KeycodeConverter::KeyStringToDomKey()` maps `key`.
That is strictly better than hand-extending the table, since the protocol
already carries both strings and this file is currently re-deriving a mapping
Chromium maintains.

Verify against in-tree precedent before using either (CLAUDE.md: this tree pins
`refs/branch-heads/7727`, and API drift across rolls has bitten this repo
repeatedly).

While there: `SynthesizedTextFor` covers Enter/Tab/Backspace for textarea text
insertion, which is correct and should stay. It is orthogonal to this — text
insertion and default-action handling are different paths.

Not attempted here: `capture/` needs a 4–8 h Chromium build, so any change
would ship unverified.

## Testing it

`tests/interactive/run.py` covers both keys, deliberately as **separate**
checks:

- `tab moves remote focus`
- `space toggles the focused remote checkbox`

They were one combined check at first, which reported "Tab+Space is broken"
without saying which key — and they are independent behaviours that could
regress separately. Both currently fail by design until the embedder is fixed.

## Related

- [`wheel-phase-start-delta-dropped.md`](./wheel-phase-start-delta-dropped.md)
  — the other input-path defect, found in the same run by the same isolation
  method: inject the interaction directly at the worker over CDP; if it works
  there, the fault is in the chromeless input path rather than in Blink.
- `docs/protocols/input-channel.md` — the wire contract. It specifies `code`
  and `key` on every key envelope, and the client sends both; they are simply
  dropped on the way to Blink.

# findings — confirmed defects, no fix landed

Each file here documents a defect **reproduced against a live deployment**,
with the file and line that would fix it. They are open tasks, not notes.

CLAUDE.md's rule applies to this directory in particular: *a TODO that names
its own verification step is a defect nobody has run yet.* These name theirs.

All three below are now FIXED and COMPILED (build lane e654ed6, image
`cr7727-e654ed644219`). They stay here until their behaviour is re-verified
against a deployment running that image — compiling is not working, and this
repo's unit tests do not exercise any of these paths.

| finding | fixed in | still needs |
| --- | --- | --- |
| [`wheel-phase-start-delta-dropped.md`](./wheel-phase-start-delta-dropped.md) | `cb_input_dispatch_mouse.cc` | `tests/interactive/` scroll checks on the new image |
| [`keyboard-dom-code-never-set.md`](./keyboard-dom-code-never-set.md) | `cb_input_dispatch_keyboard.cc` + BUILD.gn | `tests/interactive/` tab/space checks on the new image |
| [`one-session-per-worker-process.md`](./one-session-per-worker-process.md) | `cloud_browser_browser_main_parts.{h,cc}` | connect, reload, confirm video returns |

## Adding one

Write it up when a defect is confirmed but cannot be fixed in the same change —
almost always because it lives in `capture/`, which this repo cannot compile.
Include:

- **the symptom as a user sees it**, not just the mechanism. These get found by
  someone searching for what they are looking at.
- **the evidence**: log lines, counters, the two commands that separate this
  cause from the obvious wrong one.
- **why it looks like something else.** Both findings here cost a day because
  they impersonated a different failure — one looked like a NAT problem, one
  looked like a trackpad quirk.
- **the fix**, concretely enough to act on: file, symbol, and what to change.

Delete the file when the fix lands.

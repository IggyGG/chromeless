# findings — confirmed defects, no fix landed

Each file here documents a defect **reproduced against a live deployment**,
with the file and line that would fix it. They are open tasks, not notes.

CLAUDE.md's rule applies to this directory in particular: *a TODO that names
its own verification step is a defect nobody has run yet.* These name theirs.

## Resolved, kept for the citations

Three findings below are **fixed and verified working** on a deployed image
(`tests/interactive/`: wheel down/up, Tab, Space, and the re-arm scenarios in
`tests/local/`). Their README contract says "delete the file when the fix lands";
they stay because code and tests cite them by path
(`cb_input_dispatch_keyboard.cc`, `cloud_browser_browser_main_parts.cc`,
`signaling/server.go`, `tests/interactive/harness.py`) and each records a
diagnosis that took a day and impersonated a different failure. Each carries a
RESOLVED banner at the top. Do not treat them as open work.

| finding | fixed in | verified by |
| --- | --- | --- |
| [`wheel-phase-start-delta-dropped.md`](./wheel-phase-start-delta-dropped.md) | `cb_input_dispatch_mouse.cc` (delta and sign) | `tests/interactive/` scroll suite, image `cr7727-224c19413e24` |
| [`keyboard-dom-code-never-set.md`](./keyboard-dom-code-never-set.md) | `cb_input_dispatch_keyboard.cc` + BUILD.gn | `tests/interactive/` tab/space checks, image `cr7727-e654ed644219` |
| [`one-session-per-worker-process.md`](./one-session-per-worker-process.md) | broker replay rules; then the re-armable driver (`RearmSession`) superseded exit-on-close | `tests/local/rearm-scenarios.spec.ts`: pid unchanged across viewers, image `cr7727-8d2ce2e66288` |

## Open

| finding | where the fix is | what it costs today |
| --- | --- | --- |
| [`worker-signaling-no-redial.md`](./worker-signaling-no-redial.md) | **FIXED 2026-09-08** — kept for the diagnosis | (was: every broker redeploy restarted the browser ~70 s later) |
| [`file-upload-chunk-vanishes.md`](./file-upload-chunk-vanishes.md) | **FIXED + VERIFIED 2026-09-08** (`f9deaaa`) — a use-after-move computed `ok` from a moved-from string, so every SUCCESSFUL upload reported failure. `--only uploads` 9/9 | (was: `<input type=file>` ended in "no file selected" while the file sat correct on disk) |
| [`viewport-resize-freezes-beginframe-and-crashes-gpu.md`](./viewport-resize-freezes-beginframe-and-crashes-gpu.md) | `cb_viewport_controller.cc` `Apply` must bracket the resize for `CbBeginFrameDriver`; the watchdog must not re-issue into a rebinding controller | **partly fixed 2026-09-08**: the GPU abort is gone, the freeze remains — two attempts documented. Mitigated by `CHROMELESS_VIEWPORT_FOLLOW=0` |
| [`audio-dies-after-first-rearm.md`](./audio-dies-after-first-rearm.md) | **three ADM-level fixes tried and refuted** — the next probe is libwebrtc's audio send stream, not the ADM. Read the finding first | audio for the first viewer of a process only; silent for everyone after |

Not code defects, but the same shape — measured, with a named owner:

| finding | owner | what it blocks |
| --- | --- | --- |
| [`build-node-cpu-reservations.md`](./build-node-cpu-reservations.md) | `triform-builder`, `triform-wtf` | the chromeless build lane cannot schedule on t7/t8 |

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

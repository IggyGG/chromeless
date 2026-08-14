# findings — confirmed defects, no fix landed

Each file here documents a defect **reproduced against a live deployment**,
with the file and line that would fix it. They are open tasks, not notes.

CLAUDE.md's rule applies to this directory in particular: *a TODO that names
its own verification step is a defect nobody has run yet.* These name theirs.

| finding | where the fix goes | why it is still open |
| --- | --- | --- |
| [`wheel-phase-start-delta-dropped.md`](./wheel-phase-start-delta-dropped.md) | `capture/build-integration/cb_input_dispatch_mouse.cc` | C++; needs a 4–8 h Chromium build |
| [`one-session-per-worker-process.md`](./one-session-per-worker-process.md) | `capture/signaling/cb_offerer_driver.cc` + `cloud_browser_browser_main_parts.cc` | C++; the broker half IS fixed and landed |

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

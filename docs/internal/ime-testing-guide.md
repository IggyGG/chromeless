# IME testing guide (T88)

> **Audience:** `qa-tester` running manual IME validation against an
> assembled cloud-browser stack, plus any `platform-dev` reproducing
> a reported bug.
>
> **Companion:** [`docs/protocols/input-channel.md`](../protocols/input-channel.md)
> §`composition_*` — the wire format being validated here.

This document covers the manual test procedures we run to validate
the IME composition pipeline end-to-end. The automated tests in
`client/src/input.test.ts` and `capture/input-bridge/main_test.go`
cover the wire format and the bridge's CDP translation. They do
**not** cover what actually happens when the local OS's IME is in
the loop — that's what this guide is for.

The DoD is a real human typing into a real cloud Chromium tab and
seeing the right characters land:

1. macOS Pinyin: typing `ni hao` produces `你好`.
2. macOS Hangul: typing `dks dud` produces `안녕`.
3. macOS Hiragana: typing `kon nichi ha` produces `こんにちは`.
4. macOS Vietnamese (Telex): typing `tieesng vieejt` produces
   `tiếng việt`.
5. Linux/X11 ibus-pinyin: same as case 1.
6. Linux/X11 dead-key Latin: typing `Compose ' e` (or
   `<dead_acute>e` on layouts that have it) produces `é`.
7. Windows MS-IME Japanese: same as case 3.

---

## Setup matrix

You don't need to run every row before every change — pick the
relevant ones based on what you touched:

| Code that changed              | Suggested manual rows |
| ------------------------------ | --------------------- |
| client/src/input.ts (DOM listeners) | rows 1, 2, 6 |
| capture/input-bridge/main.go (CDP dispatch) | rows 1, 4 |
| docs/protocols/input-channel.md | nothing — doc only |
| Anything touching the SDP / data channel layout | full matrix |

The rows below are ordered by setup difficulty (easiest first).

---

## Common preflight

Before any of these tests:

1. The cloud-browser stack is up — `docker compose -f infra/compose.yaml up`
   (T8) or a `kind`-deployed K8s session via T71.
2. The client is open at `http://localhost:3000` with an active
   peer connection (status pill = "connected"); the input data
   channel pill reads "open".
3. A page that accepts text input is loaded inside the cloud
   browser tab. Use **`https://www.google.com/search`** as the
   default victim — the search box is a plain text input that
   exhibits IME behaviour reliably across platforms.

If any of the three fail, fix that before running the IME tests —
otherwise you'll be debugging the wrong layer.

---

## Row 1 — macOS Pinyin → Chinese

**Setup (one-time):**

1. macOS → System Settings → Keyboard → Text Input → Input Sources
   → Edit → `+` → "Pinyin – Simplified". The variant labelled
   *"ABC"* is **not** Pinyin — pick the one with the 中 icon.
2. Confirm the input-source switcher (Ctrl-Space by default) lists
   the new entry.

**Steps:**

1. Activate Pinyin (Ctrl-Space until the menu-bar icon shows 拼).
2. Click into the cloud-browser search box.
3. Type `ni`. Expected: an underlined `ni` appears with a candidate
   bar above it offering 你 / 妮 / 拟 / …
4. Type `hao`. Expected: composing string becomes `ni hao`; the
   candidate bar updates to 你好 / 拟好 / 妮好 …
5. Press **Space** (or **1**) to commit the first candidate.
   Expected: `你好` lands in the search box; the underline + bar
   disappear.
6. Press **Esc** during a fresh `ni` composition without committing.
   Expected: the underlined text vanishes; nothing lands in the box.

**What's being verified:**

- `composition_start` fires on first keystroke after activation.
- `composition_update` fires per added Pinyin syllable.
- `composition_end` fires on commit; `data` field has the Chinese
  text not the Pinyin spelling (per the protocol).
- `composition_cancel` fires on Esc; bridge dispatches the empty-
  text `imeSetComposition` that clears Chromium's compose UI.
- No `key_down` envelopes for `n`, `i`, `h`, `a`, `o`, `Space`,
  `Escape` show up in the input-bridge log during the composition
  (per the "no key_* during composition" rule).

**How to inspect the wire:** with the streamer page debug panel
open, the input-bridge log tail (`docker compose logs -f
input-bridge`) shows each dispatched CDP method. Pinyin should
produce `Input.imeSetComposition` per syllable and a single
`Input.insertText` on commit.

---

## Row 2 — macOS Hangul → Korean

**Setup:**

1. macOS → System Settings → Keyboard → Input Sources → `+` →
   "Korean" → "Korean - 2-Set Korean".

**Steps:**

1. Switch to Hangul (Ctrl-Space).
2. Type `dks`. Expected: `안` appears (composing — underlined).
   Hangul composition is jamo-by-jamo: `d` → ㅇ, `k` → ㅏ, `s` → ㄴ.
3. Type `dud`. Expected: `안녕` — the previous syllable `안`
   commits as you start the next, then `녕` is composing.
4. Press Space. Expected: ` ` lands; `녕` finalises.

**What's being verified:**

- Korean's "compose-then-flush-on-next-character" pattern produces
  multiple `composition_end` events as syllable boundaries cross.
- `composition_update` carries the per-jamo intermediate state.
- The full sequence `안녕` lands character-perfect.

---

## Row 3 — macOS Hiragana → Japanese

**Setup:**

1. macOS → System Settings → Keyboard → Input Sources → `+` →
   "Japanese" → "Hiragana".

**Steps:**

1. Switch to Hiragana.
2. Type `konnichiha`. Expected: `こんにちは` underlined as
   composing string.
3. Press Space. Expected: candidate bar offers 今日は / こんにちは /
   etc. Press Enter or 1 to commit `こんにちは`.

**What's being verified:**

- Multi-syllable Japanese flows through as one composition (until
  Space triggers candidate selection).
- The candidate-list mechanism doesn't break the composition — the
  protocol leaves `candidate_list` empty in v1.1 and the test
  passes regardless.

---

## Row 4 — Vietnamese Telex (dead-key-style)

**Setup:**

1. macOS → Input Sources → `+` → "Vietnamese" → "Telex".

**Steps:**

1. Switch to Vietnamese Telex.
2. Type `tieesng`. Expected: as you type `s` after `tieê`, the
   `ê` gets a high tone and the composing string transitions
   through `tie` → `tiê` → `tiês` → `tiếng` (sample timing varies).
3. Type ` `, then `vieejt`. Expected: `việt` appears.

**What's being verified:**

- `replacementStart` / `replacementEnd` in CDP imeSetComposition
  are exercised — Telex re-writes prior characters as accents
  arrive, which is the dead-key path through CDP. v1.1 sets these
  to `0` always (no replacement); if Vietnamese feels broken in
  the cloud Chromium, that's the field to revisit.

---

## Row 5 — Linux/X11 ibus-pinyin

**Setup:**

1. `sudo apt install ibus-pinyin && ibus-setup`. Add
   "Chinese — Pinyin" to the input methods list.
2. Re-login or run `ibus restart`.
3. The activation key is `Super-Space` by default.

**Steps:** identical to Row 1.

---

## Row 6 — X11 dead-key Latin (`Compose '`)

**Setup:**

1. Configure a Compose key — `setxkbmap -option compose:caps` (or
   pick another modifier you don't use).
2. Confirm with `xev` that the key fires with keysym `Multi_key`.

**Steps:**

1. Activate the Compose-key target (most plain text inputs work).
2. Press Compose, then `'`, then `e`. Expected: `é` lands.
3. Press Compose, then `~`, then `n`. Expected: `ñ` lands.

**What's being verified:**

- The dead-key combo produces a `compositionstart` /
  `compositionupdate` / `compositionend` cycle in the *local*
  browser's DOM API, which we then forward to the cloud Chromium
  unchanged.
- Chromium's CDP `Input.insertText` accepts the composed character
  intact; no re-composition happens server-side.

If the composed character lands as `'e` literally, the local
browser isn't producing a CompositionEvent — that's a local-OS-
config issue, not a cloud-browser bug.

---

## Row 7 — Windows MS-IME Japanese

**Setup:**

1. Settings → Time & language → Language & region → "Add a
   language" → "日本語". After install, the IME shows in the
   notification area.
2. Switch to Hiragana mode (`Caps Lock` on most layouts).

**Steps:** same as Row 3.

---

## What can go wrong (and how to triage)

| Symptom | Likely cause | First thing to look at |
| ------- | ------------ | ---------------------- |
| Pinyin types literal `ni hao` instead of 你好 | `key_*` events leaking through during composition | Check input-bridge log: did `Input.dispatchKeyEvent` fire for `KeyN`/`KeyI` between `imeSetComposition` calls? If yes, the key-suppression heuristic in `client/src/input.ts` (`isComposingKey`) is missing a case — likely `keyCode 229` not propagating, or `isComposing` not being set on the local browser. |
| Hangul commits one syllable too late | `composition_end` arriving as another `composition_start` | The two events aren't deduped. Confirm the wire trace shows a `composition_end` between syllables — if instead it's a long single `composition_update` chain, the local browser is treating the whole gesture as one composition and you need to relax the per-syllable expectation. |
| Vietnamese `tieesng` lands as `tieesng` literal | Telex isn't activated on the LOCAL machine | This is a local-OS problem. The cloud-browser only forwards what the local browser produces; if the local browser doesn't see Telex composing, neither do we. |
| Esc during composition leaves stale underline in cloud Chromium | `composition_cancel` not flowing | Check input-bridge log for `composition_cancel` envelope arrival, then confirm a follow-up `Input.imeSetComposition` with empty `text` was dispatched. If the envelope arrived but the empty-imeSetComposition is missing, regression in `capture/input-bridge/main.go` `case "composition_cancel"`. |
| All IME inputs work but selection within composing string is wrong | `selection_start` / `selection_end` not propagating | This is the cosmetic case where the *cloud-side* caret isn't quite where the user expects. Acceptable for v1.1 — file as a polish task. |

---

## Recording for follow-up

When a manual run fails, capture:

1. Local OS + IME version (`uname -a` / `system_profiler SPSoftwareDataType`).
2. Local browser + version (in particular, Chromium 147+ vs Firefox).
3. Wire trace from the input-bridge log (`docker compose logs
   --since=30s input-bridge`).
4. The expected vs observed text in the cloud Chromium tab.

File the ticket with `[T88-followup]` in the title so the IME
work cluster stays linked.

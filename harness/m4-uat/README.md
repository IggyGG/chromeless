# M4 functional UAT harness (CV2-49 / R9)

> **Status:** DRAFT — Phase 1, M4 R9. Native dispatch is itself drafted
> (M4 R1..R8 + R10); this harness wires those drafts into an
> end-to-end functional acceptance run.
>
> **Owner:** chromeless-v2 (cv2/m4-r9-uat-harness).
> **Parent:** [Plane CV2-49](https://plane.triform.cloud), Module M4.
>
> **Position on the gate-blocking schedule:** runs at module-position
> **M4**, after the M3 signaling client + DataChannel host are
> functional (without M3 there's no `input` channel to write to).

## What this harness is

A functional acceptance test for the **native input dispatch chain**
of M4. Drives one envelope per representative input surface end-to-
end through the **native** code path (M4 R1..R8 + R10) while the
legacy `input-bridge` sidecar is **still present in the image**, and
checks that the DOM in the WebRTC-captured WebContents observed each
event.

The harness is functional, not parity-comparative. The brief that
spawned this draft mentioned "two paths, observable equivalence" —
the Plane spec narrows that to *drive native path end-to-end with
input-bridge STILL in the image (M7 deletes it; the M0
absence-assertion stays NYI until post-M7)*. So we:

1. Send one envelope per M4 surface over the native DataChannel.
2. Read `document.lastInputEvent` back via CDP `Runtime.evaluate`
   on the captured WebContents.
3. Compare against the per-surface expected DOM shape.
4. Assert the `input-bridge` binary IS still in the image manifest
   (negative-of-M7 — fails red if M7 lands without the M0 assertion
   flipping at the same time).

We deliberately do **not** drive the sidecar path. The M0 R4
assertion already proves the sidecar will be gone post-M7; this
harness proves the native path is functional **while** the sidecar
is still there. The two facts together are what M7 needs to delete
the sidecar.

## Architecture

```
                ┌─────────────────────────────────────────┐
                │  harness/m4-uat/run_uat.sh              │
                │  (bash orchestrator — boots container,  │
                │   serves fixture page, runs uat.mjs)    │
                └──────────────────┬──────────────────────┘
                                   │
                                   ▼
                ┌─────────────────────────────────────────┐
                │  uat.mjs (Node driver, --scenario=NAME) │
                │                                          │
                │   1. open WebRTC connection to native   │
                │      peer via M3 signaling client       │
                │   2. resolve target WebContents via     │
                │      CDP (M4 R2 surface — but harness   │
                │      probes from outside via CDP, the   │
                │      resolver in R2 is the in-process   │
                │      lookup the native side uses)       │
                │   3. for each scenario:                  │
                │       - send envelope on input DC        │
                │       - wait for ack-timeout or RAF      │
                │       - Runtime.evaluate(                │
                │           document.lastInputEvent)       │
                │       - compare with expected            │
                │   4. probe image manifest:               │
                │       - assert /opt/cloud-browser/      │
                │         input-bridge IS PRESENT          │
                │   5. emit one JSON object on stdout      │
                └──────────────────┬──────────────────────┘
                                   │
                                   ▼
                ┌─────────────────────────────────────────┐
                │  scenarios/*.mjs                         │
                │    mouse.mjs      (R3 + wheel)           │
                │    keyboard.mjs   (R4)                   │
                │    ime.mjs        (R5)                   │
                │    touch.mjs      (R6)                   │
                │    drag.mjs       (R7)                   │
                │    clipboard.mjs  (R8)                   │
                │    pointer-leave  (R10)                  │
                └─────────────────────────────────────────┘
```

`fixtures/uat-page.html` is the page the native Chromium loads. It
attaches one event listener per DOM input event class and stores a
canonical snapshot on `document.lastInputEvent`. The harness reads
that snapshot through CDP `Runtime.evaluate` and compares it
against the per-scenario expected shape in `fixtures/expected.json`.

The CDP path here is **CDP-over-the-host's-DevTools-port**, not
the in-process CDP that the native input dispatcher uses to inject.
The harness lives outside the container and talks to the same
`/json/version`-shaped CDP surface the M0 latency / input-latency
rigs already use. This keeps the harness honest: if the native
dispatcher's UI-thread hop succeeds but the resulting event never
reaches the DOM, the harness will see it.

## Running

```bash
# Container-mode: boots a chromeless:ci container, runs the gate.
./harness/m4-uat/run_uat.sh --image=chromeless:ci

# Against an already-running container (skip boot, faster iteration):
./harness/m4-uat/run_uat.sh --skip-boot --cdp=http://localhost:9222

# Single scenario:
./harness/m4-uat/run_uat.sh --scenario=mouse

# Strict mode — every scenario must PASS:
./harness/m4-uat/run_uat.sh --strict
```

## Scenarios

| ID            | M4 R# | Envelope types covered                                     | DOM event expected            |
|---------------|-------|------------------------------------------------------------|-------------------------------|
| `mouse`       | R3    | `mouse_move`, `mouse_button`, `mouse_wheel`                | `mousemove`, `mousedown`/`mouseup`, `wheel` |
| `keyboard`    | R4    | `key_down`, `key_up` (with mods bitmask)                   | `keydown`, `keyup`            |
| `ime`         | R5    | `composition_start`, `composition_update`, `composition_end`, `composition_cancel` | `compositionstart`, `compositionupdate`, `compositionend` (+ Escape detection) |
| `touch`       | R6    | `touch_start`, `touch_move`, `touch_end`, `touch_cancel`   | `touchstart`, `touchmove`, `touchend`, `touchcancel` |
| `drag`        | R7    | `drag_start`, `drag_over`, `drop`, `drag_end`              | `dragenter`, `dragover`, `drop`, `dragend` |
| `clipboard`   | R8    | `clipboard_copy_request`                                   | `copy` (synthesised via Ctrl+C) |
| `pointer-leave` | R10 | `mouse_leave` (v1.1, R10)                                  | implicit; verified via R10 last-pointer snapshot accessor |

The full v1 envelope schema is canonical at
[`docs/protocols/input-channel.md`](../../docs/protocols/input-channel.md).
`scenarios/*.mjs` import the schema-aware envelope builder from
`lib/envelope.mjs` rather than hand-rolling JSON.

## Output

`uat.mjs` emits ONE JSON object on stdout, schema-versioned for
test-locked CI consumption:

```jsonc
{
  "schema": "m4-uat",
  "schemaVersion": 1,
  "mode": "scaffold|strict",
  "imageTag": "chromeless:ci",
  "cdp": "http://localhost:9222",
  "gate": { "verdict": "PASS|FAIL", "exitCode": 0 },
  "scenarios": [
    { "id": "mouse", "status": "PASS|FAIL|SKIPPED|NYI",
      "sent": [ /* envelopes */ ],
      "observed": [ /* document.lastInputEvent snapshots */ ],
      "evidence": { /* diff or pass-marker */ },
      "durationMs": 142 }
  ],
  "bridgePresence": {
    "status": "PASS|FAIL",
    "evidence": { "inputBridgePresent": true, "cursorWatcherPresent": true,
                  "probedPaths": [...] }
  }
}
```

Stderr carries a human-readable progress table; CI hard-keys on stdout.

## Failure modes the harness catches

1. **Native DataChannel never opened.** M3 R5 host not registered;
   M4 R1 sink never instantiated. Scenarios all SKIP with reason
   `datachannel-not-open`, gate FAILS.
2. **Envelope decoded but UI-thread hop dropped.** R1's PostTask
   didn't fire. DOM event absent; observable `lastInputEvent` is the
   previous one. Scenario FAILS with a stale-snapshot evidence
   block.
3. **WebContents resolver picked the wrong target.** R2 resolver
   returned the wrong RWHV. The event lands in a different page;
   `document.lastInputEvent` on the captured page is never updated.
4. **Per-type injection wired but wrong coordinate space.** R3 mouse
   coordinates land on the wrong pixel; DOM `mousemove` arrives but
   `target` is the body, not the test rect.
5. **IME composition forwards key events.** R5 violates the
   "no key_* during composition" suppression rule; harness sees a
   `keydown` on a key that should have been suppressed.
6. **R10 last-pointer snapshot stale.** After `mouse_leave`, the
   harness queries the same `document.lastInputEvent` accessor and
   sees a stale in-widget snapshot.
7. **input-bridge already deleted (M7 leaked).** Bridge-presence
   probe FAILS. The harness is the canary that says "M7 shipped
   without the M0 absence-assertion flipping at the same time".

## Cross-references

- M0 R4 — [bridge-absence assertion](../../verification/assertions/bridges.mjs).
  Negation of *that* is what this harness asserts.
- M0 R6 — [native-peer connectivity](../../verification/assertions/native-peer.mjs).
  Prerequisite for the harness to open a DataChannel at all.
- M3 R5 — DataChannel host + consumer-bind API.
- M4 R1..R8, R10 — the dispatcher under test.

## TODOs

- `TODO(M4-R9-cpp-seam)` — the small C++ seam that exports
  `cb_input_dispatch_test_marker` for the rare case where DOM
  observability is insufficient. See `capture/build-integration/
  cb_input_dispatch_test_marker.h` (drafted alongside this harness).
- `TODO(M4-R9-cdp-host)` — wire the CDP host argument once M3 R5's
  consumer-bind API exposes a stable port for the harness; the
  current shape assumes `--cdp=http://localhost:9222`.
- `TODO(M4-R9-strict-after-m7)` — when M7 lands, the bridge-presence
  assertion FLIPS to bridge-absence; this harness retires (its
  scenarios fold into M0 R3..R6 or get re-homed under M7).

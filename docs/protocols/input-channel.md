# Input channel protocol (v1)

Wire format for input events sent from the **browser client** to the
**cloud-browser server** over the WebRTC `input` data channel created by
T14. The server-side dispatcher that consumes this stream is T22
(platform-dev).

> **Status:** v1 (current minor: **v1.1**, additive — see
> [Versioning](#versioning)). Backwards-incompatible changes bump the
> `v` field on the wire to `2`.
>
> **Changelog**
> - **v1.1** — Added `drag_start`, `drag_over`, `drag_end`, `drop` event
>   types for drag-and-drop (T45/T46). Added `touch_start`, `touch_move`,
>   `touch_end`, `touch_cancel` for mobile-client touch input (T56).
>   Extended `mouse_wheel` with optional `delta_mode`, `phase`, and
>   `momentum` fields for scroll-inertia fidelity (T62). Extended
>   `composition_start` / `composition_update` with optional
>   `selection_start` / `selection_end` / `rect` / `candidate_list`
>   fields, and added `composition_cancel` for Escape / focus-loss
>   IME aborts (T88). Documented the "no key_* during composition"
>   suppression rule explicitly. Wire `v` stays at `1`; v1 clients/
>   servers ignore unknown types and unknown optional fields per the
>   original spec.

---

## Transport

- **Channel:** `RTCDataChannel` named `"input"`, ordered, reliable
  (default `{ ordered: true }`). Created by the client during
  `RTCPeerConnection` setup.
- **Direction:** client → server only. The server does not send messages
  on this channel; out-of-band events (e.g. clipboard reflect, cursor
  updates) live on separate channels (T-future).
- **Encoding:** UTF-8 JSON, one message per `RTCDataChannel.send` call.
  No framing on top of WebRTC — each `message` event on the receiver is
  exactly one envelope.

## Envelope

```jsonc
{
  "v": 1,                        // protocol version, integer
  "type": "mouse_move",          // discriminant, see below
  "t": 1730290000123,            // client timestamp, ms since epoch (Date.now())
  "seq": 4217,                   // monotonic, per-channel sequence number
  "data": { /* type-specific */ }
}
```

| field  | type    | required | notes                                              |
|--------|---------|----------|----------------------------------------------------|
| `v`    | integer | yes      | must equal `1` for this spec                       |
| `type` | string  | yes      | one of the values in [Event types](#event-types)   |
| `t`    | integer | yes      | client wall-clock time (`Date.now()`), ms          |
| `seq`  | integer | yes      | strictly increasing; resets when channel reopens   |
| `data` | object  | yes      | per-type schema; may be `{}` if no payload         |

The server MUST validate `v` and reject (close channel with reason) if
`v` is not in the supported set. The server MAY ignore unknown `type`
values and continue processing, logging a warning. `seq` is informational
for the server; it exists primarily for client-side coalescing
diagnostics and for the latency harness to correlate events.

`t` uses wall-clock time so it can be reconciled against the server's
NTP-synced clock for input-latency measurement. The harness aligns
`t` to server-receipt timestamps (T11, platform-dev).

---

## Event types

All `data` field offsets are integer pixels in the **video element's
content space** unless otherwise noted (i.e. after CSS `object-fit` has
been undone). The client is responsible for mapping page coordinates
back to source coordinates; the server treats them as authoritative.

### `mouse_move`

```jsonc
{ "x": 612, "y": 401 }
```

Coalescable — see [Backpressure](#backpressure-and-coalescing).

### `mouse_button`

```jsonc
{ "button": 0, "action": "down", "x": 612, "y": 401 }
```

| field    | type   | values                                     |
|----------|--------|--------------------------------------------|
| `button` | int    | `0` left, `1` middle, `2` right, `3` back, `4` forward |
| `action` | string | `"down"` \| `"up"`                         |
| `x`,`y`  | int    | content-space coords at the time of click  |

Click is _not_ a separate event — it is `down` followed by `up`.
Double-click is two `down`/`up` pairs; the server applies its own
double-click timing rules.

### `mouse_wheel`

```jsonc
{ "dx": 0, "dy": -120, "mode": 0,
  "delta_mode": "pixel", "phase": "start", "momentum": false,
  "x": 612, "y": 401 }
```

| field         | type    | values                                                          |
|---------------|---------|-----------------------------------------------------------------|
| `dx`          | int     | horizontal delta                                                |
| `dy`          | int     | vertical delta (negative = up, matching `WheelEvent.deltaY`) — see [the sign warning](#implementer-notes-two-traps-this-wheel-spec-has-already-sprung) |
| `mode`        | int     | `0` pixel, `1` line, `2` page (`WheelEvent.deltaMode`)          |
| `delta_mode`  | string? | `"pixel"` \| `"line"` \| `"page"` — string alias for `mode`. (v1.1) Servers SHOULD prefer this over `mode` when both are present. |
| `phase`       | string? | `"start"` \| `"changed"` \| `"end"` \| `null`. (v1.1) Servers without inertia handling MAY ignore. See [Wheel phase machine](#wheel-phase-machine). |
| `momentum`    | bool?   | `true` for OS-generated momentum frames (after the user lifted their finger), `false` otherwise. (v1.1) Heuristic on the client. |
| `x`,`y`       | int     | content-space coords (cursor position when wheel fired)         |

#### Wheel phase machine

A trackpad scroll on macOS / a smooth-scroll device produces a
sequence shaped like:

```
   user finger   :  ─────╲
                          ╲
                           ╲ ─ ─ ─ ─ ─ ─ ─ (deltas decay during
                            ╲               momentum after lift)
                             ╲ _ _ _
   wire phase    :  start changed changed changed end
   momentum      :  false  false   true     true   false
```

- **`start`** — first non-zero wheel event after a quiet period
  (gesture begin). The client also resets the momentum heuristic
  state at this boundary.
- **`changed`** — subsequent non-zero events in the same gesture.
- **`end`** — synthesized final event with `dx=dy=0`, emitted ~150
  ms after the last non-zero event. Lets the server flush its
  momentum-decay state.
- `null` (or omitted) — a v1.0 client that doesn't track phase. The
  server treats these as `"changed"`.

`mouse_wheel` events MAY be coalesced **within the same phase** (e.g.
two `phase=changed` deltas in one rAF tick can sum into one envelope).
Coalescing across phase boundaries is forbidden — `start` and `end`
are semantic boundary markers.

### Implementer notes: two traps this wheel spec has already sprung

Both were found against a live deployment in 2026-08 and each cost a day
because it impersonated a different failure. Written up in full in
`docs/findings/wheel-phase-start-delta-dropped.md` (resolved); the durable half
lives here.

**The sign is INVERTED relative to blink.** `dy` follows the DOM
(`WheelEvent.deltaY`): **positive means scrolling DOWN**. But
`blink::WebMouseWheelEvent::delta_y` is the opposite — positive moves the
CONTENT down, i.e. scrolls up. An embedder that passes the value through
unchanged scrolls backwards. `cb_input_dispatch_mouse.cc` negates
(`kProtocolToBlinkSign`) on both `delta_*` and `wheel_ticks_*`, since they
describe the same gesture and a mismatch tells the compositor's smoothing the
opposite of the motion.

This went unnoticed for a long time because a second defect hid it: the
`phase: "start"` envelope's delta was being discarded, so slow scrolling
produced no motion at all and there was no direction to be wrong about. Fixing
the delta is what made the inversion observable — and even then it presents as
"nothing happened", because a page at `scrollY 0` cannot scroll up.

**`phase: "start"` CARRIES A REAL DELTA.** It is defined above as the "first
**non-zero** wheel event after a quiet period", and `client/src/input.ts` sends
it that way. A server that synthesises a zero-delta `kPhaseBegan` from it must
also dispatch the carried delta — as a follow-on `kPhaseChanged`, if the
compositor needs the Begin to be zero. Dropping it means the first event of
every gesture is lost, which is invisible when scrolling fast (the next event
arrives within the 150 ms gesture window) and total when scrolling slowly (every
event is a `start`, so nothing ever moves).

### Debugging input that "does not work"

One step separates a product defect from a test defect, and it is cheap:

> **Inject the same interaction directly at the worker over CDP**
> (`Input.dispatchMouseEvent` / `Input.dispatchKeyEvent`). If it works there,
> Blink and the page are fine and the fault is in the chromeless input path. If
> it fails there too, the fault is in the page or the test.

That step found the wheel and keyboard defects above, and cleared three
suspected bugs that turned out to be test errors. It also isolates root causes
by bisection: dispatching the same `windowsVirtualKeyCode` **with and without**
the `code`/`key` params — the two that populate `dom_code`/`dom_key` — is the
entire difference between Tab moving focus and Tab doing nothing.

### `key_down` / `key_up`

```jsonc
{ "code": "KeyA", "key": "a", "mods": 0 }
```

| field  | type   | notes                                                     |
|--------|--------|-----------------------------------------------------------|
| `code` | string | `KeyboardEvent.code` — physical key identifier            |
| `key`  | string | `KeyboardEvent.key` — logical character (post-modifiers)  |
| `mods` | int    | bitmask: `1` Shift, `2` Ctrl, `4` Alt, `8` Meta           |

The server uses `code` for layout-independent dispatch (e.g. when the
remote keyboard layout differs) and falls back to `key` for printable
characters that have no mapping.

**Non-printing keys need `dom_code`/`dom_key`, not just `windows_key_code`.**
Blink's focus traversal and default-action handling read the DOM fields. With
them zero, Tab does not move focus and Space does not activate a focused
control, while typing keeps working — because characters are inserted by the
separate `kChar` text-synthesis path. That asymmetry is what hid the defect:
the keyboard looks fine until someone tabs between fields. Convert with
`ui::KeycodeConverter::CodeStringToDomCode()` / `KeyStringToDomKey()`, which
take exactly the `code`/`key` strings this protocol already carries
(`docs/findings/keyboard-dom-code-never-set.md`, resolved).

### `composition_start` / `composition_update` / `composition_end` / `composition_cancel`

For IME composition (Pinyin, Hangul, Hiragana, Vietnamese tonal
marks, dead-key combos for accented Latin). The full sequence is:

```
composition_start  →  0..N composition_update  →  composition_end
                                                  composition_cancel  (alternative — Esc / focus loss)
```

The original v1.0 spec had only `data` on each envelope; v1.1 adds
selection boundaries (so the server's CDP `imeSetComposition` call
gets the right caret + selection within the composing string), the
caret rectangle on `composition_start` (so the server can
optionally surface a candidate-positioning hint to a future client-
side IME-candidate UI), and an explicit `composition_cancel` event
distinct from "committed an empty string". v1.0 clients that only
populate `data` continue to work — the new fields are all optional.

```jsonc
// composition_start — fired when an IME composition begins.
{ "data": "",
  "rect": { "x": 612, "y": 401, "w": 12, "h": 18 } }   // optional

// composition_update — current composing text + caret/selection within it.
{ "data": "ni hao",
  "selection_start": 6,
  "selection_end": 6,
  "candidate_list": ["你好", "拟好", "妮好"] }          // optional

// composition_end — IME committed; data is the FINAL committed string.
{ "data": "你好" }

// composition_cancel — user pressed Escape or focus moved away.
{}
```

| field             | type      | required | applies to                                        |
| ----------------- | --------- | -------- | ------------------------------------------------- |
| `data`            | string    | yes      | start (empty), update, end (committed). Mirrors `CompositionEvent.data`. |
| `selection_start` | integer   | no       | update (and optionally start). Caret position within `data`, in UTF-16 code units. Defaults to `data.length` (caret at end). |
| `selection_end`   | integer   | no       | update. End of selection within `data`. When equal to `selection_start`, the caret is collapsed. Defaults to `selection_start`. |
| `rect`            | object    | no       | start. `{x, y, w, h}` content-space bounding rect of the focused element / caret. Reserved for a future client-side candidate-positioning UI; servers MAY ignore. |
| `candidate_list`  | string[]  | no       | update. Optional list of IME candidate strings the client-side UI could surface. **Currently unobservable from a Web client** — the IME is a system component the page can't introspect — so v1.1 clients leave this empty. v2 may add a per-platform IME bridge that populates it. |

**Key suppression during composition.** While composition is in
flight, raw `key_down` / `key_up` events MUST NOT be forwarded for
keys that are part of the composition. The composing-key detection
on the client side uses the standard `KeyboardEvent.isComposing`
property (true between `compositionstart` and `compositionend`) plus
the legacy Chromium `keyCode === 229` ("composition is active")
sentinel. Servers receiving both `key_*` and `composition_*` events
for the same composition would dispatch the keys twice — once
literally and once as the composed result.

`composition_end.data` is the final committed string and should be
treated authoritatively even if intermediate `composition_update`
events were dropped. Servers SHOULD treat `composition_cancel` as
"discard any in-progress composition state" — e.g., dispatch
`Input.imeSetComposition` with empty `text` to clear the IME's
in-progress display in the cloud Chromium.

**Detecting cancel from the DOM.** There is no `compositioncancel`
DOM event in HTML. Clients detect cancel by:
1. observing a `keydown` with `key === "Escape"` while
   `isComposing === true`, OR
2. observing `compositionend` with empty `data` (no commit happened —
   user moved focus or aborted).

Either pathway emits `composition_cancel`. A `compositionend` with
non-empty `data` always emits `composition_end`.

### `clipboard_paste`

```jsonc
{ "text": "hello world" }
```

Sent when the local user pastes into the cloud browser. The server
writes this into the remote clipboard and synthesizes a paste action.
v1 supports text only; binary/HTML clipboard payloads are deferred.

### `clipboard_copy_request`

```jsonc
{}
```

Sent when the local user issues a copy gesture inside the video stage.
Asks the server to push the current remote clipboard back over a
**separate** out-of-band channel (not specified here — see future
"clipboard" channel). Included in the input envelope so that the local
copy gesture preserves activation timing.

### `drag_start` / `drag_over` / `drop` / `drag_end` (v1.1)

Drag-and-drop. Mirrors the DOM `dragstart` / `dragover` / `drop` /
`dragend` events on the client side and dispatches via CDP
`Input.dispatchDragEvent` on the server side (T22). Coordinates are in
the same content-space as mouse events.

**State machine.** Every drag begins with `drag_start`, contains zero
or more `drag_over` updates while the pointer is in the viewport, and
ends with **either** `drop` (successful) **or** `drag_end` with
`success: false` (cancelled / dropped outside). After `drop`, the
client SHOULD send `drag_end` with `success: true` so the server can
release any per-drag state. Servers MUST tolerate stray `drag_over`
events outside an active drag (treat as no-op + log).

```jsonc
// drag_start  — drag has entered the cloud-browser viewport
{ "x": 612, "y": 401,
  "types": ["text/plain", "text/uri-list"],
  "items": [
    { "kind": "string", "type": "text/plain",   "data": "hello world" },
    { "kind": "string", "type": "text/uri-list", "data": "https://example.com/" }
  ] }

// drag_over  — pointer position update while dragging
{ "x": 700, "y": 410 }

// drop  — pointer released; payload re-asserted
{ "x": 700, "y": 410,
  "types": ["text/plain", "text/uri-list"],
  "items": [ /* same items as drag_start */ ] }

// drag_end  — close out the drag
{ "success": true }
```

Per-event-type fields:

**`drag_start`** / **`drop`** carry the same payload shape:

| field   | type     | notes                                                 |
|---------|----------|-------------------------------------------------------|
| `x`,`y` | int      | content-space coords                                  |
| `types` | string[] | the MIME types the drag offers, in client order       |
| `items` | object[] | per [Item schema](#drag-item-schema). 0..N entries.   |

**`drag_over`**:

| field   | type | notes                  |
|---------|------|------------------------|
| `x`,`y` | int  | content-space coords   |

**`drag_end`**:

| field      | type    | notes                                              |
|------------|---------|----------------------------------------------------|
| `success`  | boolean | `true` after `drop`; `false` for cancel / outside  |

#### Drag item schema

| field   | type   | notes                                                                   |
|---------|--------|-------------------------------------------------------------------------|
| `kind`  | string | `"string"` or `"file"`. v1 only carries `"string"`; see below.          |
| `type`  | string | MIME type, lower-case (`"text/plain"`, `"text/uri-list"`, `"text/html"`).|
| `data`  | string | UTF-8 text payload. Required when `kind == "string"`.                   |

**v1 file-drag policy.** When the user drags a file (DOM
`DataTransferItem.kind === "file"`), the client emits the item with
`kind: "file"`, `type: <file mime>`, and **NO `data` field**. v1
servers MUST treat file items as informational and not transmit file
contents — file upload is a separate task on the v1+ roadmap. The
client SHOULD also `console.warn` so the user understands why their
file drop didn't transfer.

**Coalescing.** `drag_over` is coalescable on the same rules as
`mouse_move` (see [Backpressure](#backpressure-and-coalescing)).
`drag_start`, `drop`, and `drag_end` are NEVER dropped.

### `touch_start` / `touch_move` / `touch_end` / `touch_cancel` (v1.1)

Multi-touch input. Mirrors the DOM `TouchEvent` API on the client side
and dispatches via CDP `Input.dispatchTouchEvent` on the server side
(T22). v1 success criteria deferred touch to Phase 4 (mobile is
"best-effort" per [`docs/v1-success-criteria.md`](../v1-success-criteria.md)
B5), but landing the wire format early lets a mobile client form-
factor work end-to-end on day one.

**Identifier semantics.** Each ongoing touch carries a stable
`identifier` — the same integer on every event for the same finger,
chosen by the client and unique per active touch. After `touch_end`
or `touch_cancel` the identifier MAY be reused. The server MUST treat
`touch_move`/`touch_end`/`touch_cancel` for an identifier it has not
seen `touch_start` for as a no-op + warning.

**One envelope per finger per event.** Multi-touch gestures produce
one envelope per moving finger per DOM event, not one envelope
carrying all current touches. The server-side bridge maintains the
"active touches" snapshot needed by CDP from those single-finger
updates.

```jsonc
// touch_start  — finger 1 lands at (612, 401)
{ "identifier": 1, "x": 612, "y": 401,
  "radius_x": 12, "radius_y": 12, "force": 0.6, "twist": 0 }

// touch_move   — finger 1 slides
{ "identifier": 1, "x": 700, "y": 410,
  "radius_x": 12, "radius_y": 12, "force": 0.6, "twist": 0 }

// touch_end    — finger 1 lifts
{ "identifier": 1 }

// touch_cancel — system cancelled the touch (e.g. notification)
{ "identifier": 1 }
```

| field      | type     | required | notes                                                                |
|------------|----------|----------|----------------------------------------------------------------------|
| `identifier` | int    | yes      | per-finger; stable across the lifetime of one touch                  |
| `x`,`y`    | int      | yes for `touch_start` / `touch_move` | content-space coords           |
| `radius_x`,`radius_y` | int | optional | contact ellipse radii (px). Default 1 if omitted.            |
| `force`    | float    | optional | 0.0–1.0; mirrors `Touch.force`. Default 0 if not reported.           |
| `twist`    | int      | optional | rotation in degrees clockwise; mirrors `Touch.rotationAngle`. Default 0. |

**Coalescing.** `touch_move` is coalescable per finger: when more than
one `touch_move` for the same `identifier` is queued in a single
flush window, only the latest is kept. Other finger identifiers are
preserved independently. `touch_start`, `touch_end`, and
`touch_cancel` are NEVER dropped.

**Mapping to CDP.** The bridge maintains a map of active touches
keyed by `identifier` and on every event re-issues
`Input.dispatchTouchEvent` with the *currently active* set as
`touchPoints` (per CDP/puppeteer convention: the array reflects the
state *after* this event applies — empty when the last finger lifts).

---

## Backpressure and coalescing

WebRTC data channels expose `RTCDataChannel.bufferedAmount`. The
client's `InputChannel` implementation MUST:

1. Maintain a **send queue** for `mouse_move` events. When a new
   `mouse_move` arrives and the queue depth (or `bufferedAmount`)
   exceeds the configured threshold, **drop older `mouse_move` events**
   in favour of newer ones. Other event types are never dropped.
2. Default thresholds:
   - `coalesceThreshold` = 4 queued mouse_moves, OR
   - `bufferedAmount` > 64 KiB.
3. Always preserve **mouse_button**, **mouse_wheel**, **key_***,
   **composition_***, and **clipboard_*** events — these are
   semantically irreplaceable.
4. Flush the queue on every animation frame (`requestAnimationFrame`)
   rather than on every input event, so a flurry of `pointermove` events
   produces at most one network frame per refresh.

When dropping mouse_moves, log a warning with the dropped count for the
debug panel; do not surface to the user.

## Sequence numbers

`seq` starts at `0` when the channel transitions to `open` and
increments by 1 on every send. If the channel closes and re-opens
(reconnect), `seq` resets and the client SHOULD send a new
`mouse_move` (current cursor position) on first send so the server
state is consistent.

## Versioning

- `v` is integer, currently `1`.
- **Minor revisions (v1.x) are additive and DO NOT change `v` on the
  wire.** New event types and new optional fields are minor changes —
  v1 clients/servers must ignore what they don't understand. We track
  the additions in this document's [Changelog](#input-channel-protocol-v1)
  for human readers.
- **Major revisions (v2+) are breaking and DO bump `v`.** Removing a
  field, renaming a field, repurposing an existing type, or changing
  the meaning of an existing value are all major changes.
- Adding a new optional field to a `data` schema is a minor change
  (v1.x). The server MUST ignore unknown fields.
- Adding a new `type` is a minor change (v1.x). Servers SHOULD ignore
  unknown types and MAY log a warning. Clients MAY query a
  capabilities endpoint in the future to negotiate which types are
  supported (out of scope for v1).
- Removing / renaming / repurposing a field bumps `v` to `2`.
- The client and server negotiate the version range via the signaling
  layer, not over the data channel itself (see T22 for server-side
  negotiation).

### Why we don't put a `1.1` on the wire

Carrying a minor version on the wire would tempt receivers to gate
behaviour on it ("if minor < 1, fall back…"), which couples the
server's behaviour to the client's exact version rather than to the
shape of the message it actually sent. The unknown-type-ignored rule
keeps the protocol forward-compatible without that coupling.

## Security notes (Phase 1 deferred)

- v1 has no authentication on the input channel itself. The signaling
  layer is responsible for authorising the peer connection (Phase 3).
- v1 has no rate limiting on the server side. T22's bridge MUST add a
  per-channel input rate ceiling before this protocol is exposed beyond
  the dev environment.
- Pasted clipboard content is forwarded verbatim. The server MUST
  treat it as untrusted user input.

## Test vectors

```jsonc
// mouse move at (100, 200), 100ms after channel open
{ "v": 1, "type": "mouse_move", "t": 1730290000100, "seq": 0,
  "data": { "x": 100, "y": 200 } }

// shift-click at (100, 200)
{ "v": 1, "type": "mouse_button", "t": 1730290000150, "seq": 1,
  "data": { "button": 0, "action": "down", "x": 100, "y": 200 } }
{ "v": 1, "type": "mouse_button", "t": 1730290000160, "seq": 2,
  "data": { "button": 0, "action": "up",   "x": 100, "y": 200 } }

// Cmd+A on macOS
{ "v": 1, "type": "key_down", "t": 1730290000200, "seq": 3,
  "data": { "code": "KeyA", "key": "a", "mods": 8 } }
```

## Cross-references

- T14 — browser client + RTCDataChannel "input" creation.
- T20 — this protocol + client encoder (`client/src/input.ts`).
- T22 — platform-dev's server-side dispatcher that translates these
  events into Chromium DevTools `Input.dispatchMouseEvent` /
  `Input.dispatchKeyEvent` calls.
- T10/T11 — latency harness; uses `seq` + `t` to correlate input events
  against frame-receipt timestamps.

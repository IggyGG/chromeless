# Input channel protocol (v1)

Wire format for input events sent from the **browser client** to the
**cloud-browser server** over the WebRTC `input` data channel created by
T14. The server-side dispatcher that consumes this stream is T22
(platform-dev).

> **Status:** v1, frozen for Phase 0 / Phase 1. Backwards-incompatible
> changes bump the `v` field — see [Versioning](#versioning).

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
{ "dx": 0, "dy": -120, "mode": 0, "x": 612, "y": 401 }
```

| field   | type | values                                                          |
|---------|------|-----------------------------------------------------------------|
| `dx`    | int  | horizontal delta                                                |
| `dy`    | int  | vertical delta (negative = up, matching `WheelEvent.deltaY`)    |
| `mode`  | int  | `0` pixel, `1` line, `2` page (`WheelEvent.deltaMode`)          |
| `x`,`y` | int  | content-space coords (cursor position when wheel fired)         |

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

### `composition_start` / `composition_update` / `composition_end`

```jsonc
{ "data": "こん" }
```

For IME composition. Mirrors `CompositionEvent.data`. The full sequence
is `composition_start` → 0..N `composition_update` → `composition_end`.
`composition_end.data` is the final committed string and should be
treated authoritatively even if intermediate updates were dropped.

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
- Adding a new optional field to a `data` schema does NOT bump `v`. The
  server MUST ignore unknown fields.
- Adding a new `type` does NOT bump `v`. The server SHOULD ignore
  unknown types.
- Removing/renaming/repurposing a field bumps `v`.
- The client and server negotiate the version range via the signaling
  layer, not over the data channel itself (see T22 for server-side
  negotiation).

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

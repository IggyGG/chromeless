# Cursor channel protocol (v1)

Wire format for cursor metadata sent from the **cloud-browser server**
to the **browser client** over a dedicated `RTCDataChannel` named
`"cursor"`. The client renders a CSS/SVG cursor at the reported
coordinates over the streamed video; the streamed frame itself shows
no cursor (per the project brief: server-side framebuffer cursor adds
a frame of latency, so we render client-side from metadata).

> **Status:** v1, frozen for Phase 1. Shares the versioning policy of
> [`input-channel.md`](./input-channel.md). Backwards-incompatible
> changes bump the `v` field.

The server emitter is `capture/cursor-watcher/` (T26, platform-dev);
the client renderer is `client/src/cursor.ts`.

---

## Transport

- **Channel:** `RTCDataChannel` named `"cursor"`, ordered, reliable
  (default `{ ordered: true }`). Created either by the client during
  `RTCPeerConnection` setup, or — preferred — labelled in the SDP and
  emitted by the server-side streamer page.
- **Direction:** server → client only. The client does not send
  messages on this channel.
- **Encoding:** UTF-8 JSON, one envelope per `send`. No framing on
  top of WebRTC.

For the Phase-1 skeleton, when the WebRTC data-channel relay isn't
wired yet, the watcher writes envelopes to **stdout** (one per line)
or to a localhost WebSocket sink (`--sink ws://…`), matching the
shape of the client-side input bridge for symmetry with T22.

---

## Envelope

```jsonc
{
  "v": 1,                        // protocol version, integer
  "type": "cursor",              // discriminant; only "cursor" in v1
  "t": 1730290000123,            // server timestamp (epoch ms)
  "seq": 17,                     // monotonic, per-channel
  "data": { /* see Data */ }
}
```

| field  | type    | required | notes                                              |
|--------|---------|----------|----------------------------------------------------|
| `v`    | integer | yes      | must equal `1`                                     |
| `type` | string  | yes      | only `"cursor"` in v1; reserved for future types   |
| `t`    | integer | yes      | server wall-clock (epoch ms)                       |
| `seq`  | integer | yes      | strictly increasing within a channel               |
| `data` | object  | yes      | see [Data](#data)                                  |

The client MUST validate `v` and ignore (drop) any envelope whose
`v` is not in the supported set. Unknown `type` values MAY be
ignored — there is currently only one.

---

## Data

```jsonc
{
  "x": 612, "y": 401,
  "visible": true,
  "shape": "pointer",
  "hotspot": { "x": 0, "y": 0 },          // optional
  "custom_image_b64": "iVBORw0K…",        // optional
  "image_format": "png"                   // when custom_image_b64 present
}
```

| field              | type       | required | notes                                                                 |
| ------------------ | ---------- | -------- | --------------------------------------------------------------------- |
| `x`, `y`           | integer    | yes      | source-image-space coordinates of the cursor *hotspot*; same coordinate system as `input-channel.md` |
| `visible`          | boolean    | yes      | `false` when the page sets `cursor: none` or the pointer leaves the viewport |
| `shape`            | string     | yes      | one of [Shape names](#shape-names); `"custom"` when `custom_image_b64` is set; `"none"` when `visible` is false |
| `hotspot`          | object     | no       | `{x,y}` offset within the cursor image, in source pixels. Only meaningful for `shape: "custom"`. Defaults to `{0,0}`. |
| `custom_image_b64` | string     | no       | base64 of a PNG (`image_format: "png"`) for arbitrary `cursor: url(...)` cases. Capped at **64 KiB encoded** in v1. |
| `image_format`     | string     | when image present | `"png"` only in v1                                       |

Coordinates are **integers** in the source image's intrinsic pixel
space, matching the `x`/`y` semantics of `input-channel.md`. The
client is responsible for mapping to its own video element (T14
already does this in reverse).

### Shape names

The standard CSS cursor keywords are accepted as values for `shape`,
plus three control values:

```
default pointer text wait crosshair move not-allowed grab grabbing
ew-resize ns-resize nesw-resize nwse-resize col-resize row-resize
zoom-in zoom-out help progress vertical-text context-menu alias copy
cell all-scroll no-drop n-resize e-resize s-resize w-resize
ne-resize nw-resize se-resize sw-resize

custom    — emit a custom_image_b64 instead
none      — used when visible == false
```

Unknown CSS keywords (e.g. vendor-specific) MAY appear; the client
falls back to `"default"` rendering for any value it does not
recognise.

---

## Emission policy

- The server emits an envelope **only on change**: a delta in any of
  `(x, y, shape, visible, custom_image_b64)` from the previous
  envelope. Identical-state polls are suppressed.
- The server SHOULD coalesce `x`/`y`-only updates to ≤ 60 Hz to avoid
  flooding the data channel during fast pointer movement; the
  shape-change events bypass the throttle.
- On (re)connect the server emits one full envelope as the new client
  state baseline before any deltas.

The client treats every envelope as **absolute** state, not a diff —
so packet loss on the (unreliable) channel is recoverable as soon as
the next change envelope arrives. v1 uses the *reliable* ordered
mode, but future revisions may move to unreliable; the absolute-state
property keeps the client correct under either mode.

---

## Data-channel labelling and negotiation

For the Phase-1 demo path, the client creates the channel with
`pc.createDataChannel("cursor", { ordered: true })`, mirroring how
the `"input"` channel is created in T14. The streamer page on the
server-side picks up the matching `RTCPeerConnection.ondatachannel`
event and forwards messages from the watcher into it.

Future work (Phase 2) moves channel creation into the SDP so it can
be labelled `negotiated: true` with a known channel id; the protocol
itself does not change.

---

## Versioning

- `v` is integer, currently `1`.
- Adding a new optional field to `data` does NOT bump `v`. Receivers
  MUST ignore unknown fields.
- Adding a new shape keyword does NOT bump `v`. Receivers MUST fall
  back to `"default"` rendering for unknown values.
- Removing/renaming/repurposing a field bumps `v`.
- Negotiation is via signaling-layer config, not over the data
  channel itself (matches `input-channel.md`).

---

## Test vectors

```jsonc
// Initial baseline (server emits on (re)connect)
{ "v": 1, "type": "cursor", "t": 1730290000100, "seq": 0,
  "data": { "x": 0, "y": 0, "visible": true, "shape": "default" } }

// Pointer moves over a clickable element — shape switches to pointer
{ "v": 1, "type": "cursor", "t": 1730290000150, "seq": 1,
  "data": { "x": 240, "y": 80, "visible": true, "shape": "pointer" } }

// Page calls cursor: none on a custom canvas — visible flips off
{ "v": 1, "type": "cursor", "t": 1730290000300, "seq": 2,
  "data": { "x": 240, "y": 80, "visible": false, "shape": "none" } }

// Custom cursor (cursor: url(...))
{ "v": 1, "type": "cursor", "t": 1730290000600, "seq": 3,
  "data": { "x": 412, "y": 220, "visible": true, "shape": "custom",
            "hotspot": {"x": 8, "y": 8},
            "custom_image_b64": "iVBORw0K…",
            "image_format": "png" } }
```

---

## Cross-references

- [`input-channel.md`](./input-channel.md) — companion, client→server.
- T14 — browser client + RTCDataChannel infrastructure.
- T26 — this protocol + watcher service + client renderer.
- T22 — server-side input bridge; same code shape (per-event-type
  CDP wrapping) on the inverse direction.

# Clipboard channel protocol (v1)

Wire format for **bidirectional** clipboard synchronization between
the **browser client** and the **cloud Chromium**, carried over a
dedicated `RTCDataChannel` named `"clipboard"` (separate from
`"input"` and `"cursor"`).

> **Status:** v1, frozen for Phase 1. Shares the versioning policy of
> [`input-channel.md`](./input-channel.md) and
> [`cursor-channel.md`](./cursor-channel.md). Backwards-incompatible
> changes bump the `v` field.

The client side is `client/src/clipboard.ts` (T31, platform-dev). The
server side WAS `capture/clipboard-bridge/` (a Phase-1 sidecar for the
streamer page deleted in M7); since 2026-09 it is
`capture/build-integration/cb_clipboard_relay.{h,cc}` in the browser process,
which owns both directions directly — see [Guest implementation](#guest-implementation).

The project brief calls clipboard sync "a notable rabbit hole"; v1 is
a deliberately narrow path: **text only, user-triggered, never
silent**.

---

## Transport

- **Channel:** `RTCDataChannel` named `"clipboard"`, ordered, reliable.
- **Direction:** bidirectional. Either side may initiate.
- **Encoding:** UTF-8 JSON, one envelope per `send`.

---

## Envelope

```jsonc
{
  "v": 1,                              // protocol version
  "type": "clipboard_offer",           // discriminant; only one type in v1
  "t": 1730290000123,                  // sender timestamp, epoch ms
  "seq": 0,                            // monotonic per-channel
  "data": {
    "direction": "client->cloud",      // or "cloud->client"
    "source":    "user_action",        // see [Security model]
    "text":      "hello world"         // utf-8 string, ≤ 1 MiB
  }
}
```

| field   | type    | required | notes                                           |
|---------|---------|----------|-------------------------------------------------|
| `v`     | integer | yes      | must equal `1`                                  |
| `type`  | string  | yes      | only `"clipboard_offer"` in v1                  |
| `t`     | integer | yes      | sender wall-clock (epoch ms)                    |
| `seq`   | integer | yes      | strictly increasing within a channel            |
| `data`  | object  | yes      | see below                                       |

`data` schema:

| field       | type   | required | notes                                                                |
| ----------- | ------ | -------- | -------------------------------------------------------------------- |
| `direction` | string | yes      | `"client->cloud"` (paste from local clipboard into remote tab) or `"cloud->client"` (copy from remote tab back to local clipboard) |
| `source`    | string | yes      | `"user_action"` is the only allowed value in v1; future revisions may add e.g. `"sync"` for non-event-driven sync (out of scope for v1) |
| `text`      | string | yes      | utf-8; **≤ 1 MiB** (`text.length` ≤ 1,048,576). Larger payloads MUST be rejected before sending and MUST be ignored on receipt with a warning log. |

---

## Security model

Clipboard sync has historically been used to silently exfiltrate
secrets between browser sandboxes. v1's security stance is
deliberately conservative:

1. **No silent polling.** Neither side polls a clipboard and emits
   updates when it changes. An envelope is emitted **only** as the
   direct consequence of an explicit user-initiated event:
   - **client→cloud:** the local user fires a `paste` event inside
     the streamed-video region (or a focused element inside the
     client app, depending on the integration). The handler reads
     the local clipboard text from `ClipboardEvent.clipboardData` —
     the event-bound payload that the browser only populates for
     genuine user gestures — and sends one envelope.
   - **cloud→client:** the remote user fires a `copy` event inside
     the cloud Chromium tab. The bridge's CDP probe receives the
     copied text via a `Runtime.bindingCalled` event and forwards it.
     No `setInterval(navigator.clipboard.readText, ...)`.
2. **Text only.** No HTML, image, or arbitrary MIME type forwarding.
   Anything other than text is dropped at the source. (Phase 4 may
   add narrow images-only support behind explicit settings.)
3. **Size cap.** 1 MiB UTF-8. The cap is hard on both sides; oversize
   payloads are dropped, not truncated, so what the user sees in the
   destination clipboard is exactly what the source had — or
   nothing.
4. **Sender stamps the timestamp; receiver does not echo.** A side
   that *just* received a `clipboard_offer` for one direction does
   not immediately emit the inverse envelope when its local
   clipboard changes — that would create a loop. We rely on the
   user-action gating above (real user events do not chain).
5. **Receiver gating.**
   - The client writes the received text via
     `navigator.clipboard.writeText(text)` only when
     `document.hasFocus() === true`. If the document is not focused,
     the text is queued; the next focus event flushes it. This
     matches Chromium's own UX rule for clipboard write.
   - The cloud Chromium write is gated by CDP `Browser.grantPermissions`
     scoped to the streamer-page origin, set up once at sidecar
     startup (see `capture/clipboard-bridge/README.md`).
6. **Untrusted contents.** Both sides treat received text as
   untrusted user input. The cloud Chromium does not interpret it as
   a URL/script/etc. — it lands in the clipboard, period.

The signaling layer is responsible for authorising the peer
connection itself; v1 has no per-channel authentication on top.
Phase 3 work.

---

## Guest implementation

`CbClipboardRelay` is bound to the `"clipboard"` DataChannel as its observer
and is also a `ui::ClipboardObserver`. It enforces this spec at the only place
that can:

- **client→cloud (paste).** A valid v1 envelope is written to the guest's
  clipboard (`ui::ScopedClipboardWriter`, `kCopyPaste`) and a Ctrl+V key pair
  is synthesised against the active WebContents — the same shape the copy
  gesture uses for Ctrl+C — so the page's own `keydown`/`paste` handlers run
  exactly as for a physical keypress. Wrong direction, wrong `source`, wrong
  version, or more than 1 MiB: dropped with a warning, never truncated.
- **cloud→client (copy).** A clipboard change is forwarded **only inside a 2 s
  window armed by a viewer gesture**: the `clipboard_copy_request` envelope on
  the input channel, or a Ctrl/Cmd+C `key_down` that reached the guest. One
  change per window. A change whose text equals the relay's own last paste
  write is an echo and is dropped. This is rule 1 of the security model
  (no silent polling) made mechanical: a page calling
  `navigator.clipboard.writeText()` from a timer changes the guest clipboard
  and nothing leaves the guest.
- Text only, 1 MiB cap on read as on write; the copied text is read
  asynchronously (`ui::Clipboard::ReadText`) on the UI thread.

Rule 5's "`Browser.grantPermissions` scoped to the streamer-page origin" no
longer applies — there is no page-side writer; the browser process writes the
clipboard itself.

## Versioning

- `v` is integer, currently `1`.
- Adding a new optional field to `data` does NOT bump `v`. Receivers
  MUST ignore unknown fields.
- Adding a new `source` value (e.g., `"sync"`) does NOT bump `v`.
  Receivers MUST drop envelopes whose `source` they do not recognise
  (because the implied semantics may differ).
- Removing/renaming/repurposing a field bumps `v`.

---

## Test vectors

```jsonc
// Local user pastes "hello world" into the cloud tab
{ "v":1, "type":"clipboard_offer", "t":1730290000100, "seq":0,
  "data":{ "direction":"client->cloud", "source":"user_action",
           "text":"hello world" } }

// Remote user copies a URL inside the cloud tab; bridge forwards it
{ "v":1, "type":"clipboard_offer", "t":1730290000200, "seq":1,
  "data":{ "direction":"cloud->client", "source":"user_action",
           "text":"https://example.com/" } }

// Oversize — receiver MUST drop and warn (text length > 1 MiB)
{ "v":1, "type":"clipboard_offer", "t":1730290000300, "seq":2,
  "data":{ "direction":"client->cloud", "source":"user_action",
           "text":"<...> 2 MiB of A's <...>" } }
```

---

## Cross-references

- [`input-channel.md`](./input-channel.md) — the input channel also
  carries a v1 `clipboard_paste` event for the **client→cloud** path
  bundled into a user input gesture. The two are intentionally
  duplicative for v1: the input-channel path preserves the activation-
  timing relationship with surrounding key events, while this
  dedicated channel is the supported general-purpose path. Phase 2
  may consolidate.
- [`cursor-channel.md`](./cursor-channel.md) — sibling sidecar
  channel, similar shape.
- T14 — browser client + RTCDataChannel infrastructure.
- T22 — input-bridge sibling service; same code shape on the server.

# Control channel protocol (v1)

The guest's **ask-a-human** path, carried on a `RTCDataChannel` named
`"control"`.

> **Status:** v1. Seven kinds, all implemented as of 2026-09-10.
>
> **Companion implementations:**
> [`capture/build-integration/cb_control_channel.{h,cc}`](../../capture/build-integration/cb_control_channel.h)
> (guest) and [`client/src/control.ts`](../../client/src/control.ts) (viewer).
>
> This file was written after batch C, when the kind count went from two to
> seven. Until then the only specification was the C++ header — which is
> excellent and which a TypeScript author has no reason to read.

Everything else the guest sends is telemetry or pixels. This is the one path
where the browser **stops and waits for a person**: a page calls `confirm()`,
or clicks `<input type=file>`, or hits a bad certificate, and chromium hands
the embedder a callback it will not proceed without.

---

## Envelopes

The house shape used by the input/cursor/clipboard channels —
`{v, type, t, seq, data}` — so existing decode helpers apply unchanged.

### `ui_request` — guest → viewer, needs an answer

```jsonc
{"v":1,"type":"ui_request","t":1757462400000,"seq":7,
 "data":{"id":"12","kind":"js_dialog","deadline_ms":60000, /* ...kind fields */ }}
```

### `ui_response` — viewer → guest, answering one

```jsonc
{"v":1,"type":"ui_response","t":1757462401000,"seq":3,
 "data":{"id":"12", /* ...kind-specific answer */ }}
```

`id` correlates the two. A response whose id is unknown — late, duplicated or
invented — is dropped with a log and never trusted: the viewer is a remote
peer, and this channel gates real decisions like "proceed past a certificate
error".

### `ui_event` — guest → viewer, fire-and-forget

```jsonc
{"v":1,"type":"ui_event","t":1757462402000,"seq":9,
 "data":{"kind":"download","phase":"progress", /* ... */ }}
```

No id, no reply, no bookkeeping. By definition nothing depends on one
arriving.

---

## THE INVARIANT

**Every request resolves exactly once, and MUST NOT depend on the viewer to
do it.**

`RunJavaScriptDialog` blocks the page's JS thread. `RunFileChooser` holds a
`FileSelectListener` that chromium CHECKs on if released unresolved.
`CreateLoginDelegate`'s callback must run — or the delegate must be destroyed,
which IS the cancellation. A dropped answer is a wedged tab or a hung network
request, not a missing feature.

So on the guest side:

* if the channel is not open when the request is minted, the **default is
  applied synchronously** and nothing is sent;
* every in-flight request carries a `base::OneShotTimer`; on expiry the
  default is applied;
* `CancelAllPending()` resolves everything outstanding on channel close,
  WebContents destruction and session teardown.

And on the viewer side: a missing handler, a throwing handler and a rejected
promise all send the **safe** answer rather than nothing
(`settleWith` in control.ts).

**Every default is what the guest did before the kind existed.** Denied,
cancelled, dismissed. An unimplemented kind is never a regression and never
accidentally permissive.

---

## Requests

| kind | asks | answer | default |
| --- | --- | --- | --- |
| `js_dialog` | alert / confirm / prompt / beforeunload | `{accept, prompt_text?}` | dismissed |
| `file_chooser` | `<input type=file>` was clicked | `{accept}` — true means bytes are coming on the `files` channel | no file selected |
| `permission` | geolocation, notifications, camera… | `{granted}` | **denied** |
| `cert_error` | a TLS error on a navigation | `{proceed}` | **cancelled** |
| `login` | HTTP 401 / 407 | `{username, password}` — both or neither | cancelled → the 401 |

### `permission`

```jsonc
{"kind":"permission","id":"20","deadline_ms":90000,
 "origin":"https://example.com/","permissions":["geolocation"]}
```

`permissions` are **readable names**, already mapped from blink's
`PermissionDescriptorPtr` by the guest — the viewer never sees a raw
descriptor. The viewer answers the group as a whole; a per-permission UI
would be a better prompt and is not worth a protocol that can return the
wrong *number* of answers.

### `cert_error`

```jsonc
{"kind":"cert_error","id":"21","deadline_ms":120000,
 "url":"https://bad.example/","cert_error":-202,
 "subject":"bad.example","issuer":"Nobody","is_main_frame":true}
```

`cert_error` is a chromium net error code (negative). Map the common ones for
display and fall back to the number — see `certErrorText` in control.ts, whose
values are pinned against `net/base/net_error_list.h`.

**The guest does not send this at all when HSTS applies.** The site has
declared its certificate must be valid, so "proceed anyway" would only be
offering the user a way to be wrong.

### `login`

```jsonc
{"kind":"login","id":"22","deadline_ms":120000,
 "url":"https://intranet/","realm":"Staff","scheme":"basic",
 "is_proxy":false,"first_attempt":true}
```

`realm` is **server-supplied text shown to someone deciding whether to type a
password** — truncate it, never render it as markup.

`first_attempt` is false on a retry, i.e. the previous credentials were
rejected. Surface it: without it the second prompt is indistinguishable from
the first and the user retypes the same rejected password.

`is_proxy` matters for the same reason — proxy credentials are a different
secret, and a prompt that conflates them invites the wrong password.

Send **both** `username` and `password` or neither. The guest treats a partial
answer as cancelled.

---

## Events

| kind | fires when |
| --- | --- |
| `fullscreen_changed` | the page entered or left the Fullscreen API |
| `tab_opened` / `tab_closed` | advisory; the gateway's `/json` list is the truth |
| `download` | a download started, progressed or completed |

### `download`

```jsonc
{"kind":"download","phase":"progress","id":3,
 "filename":"report.pdf","url":"https://example.com/report.pdf",
 "mime_type":"application/pdf",
 "received_bytes":196608,"total_bytes":1234567,"state":0}
```

`phase` is `started` | `progress` | `complete`.

**`total_bytes` is -1 when the server sent no `Content-Length`.** Render that
as bytes-so-far, not a percentage: "0%" for a download that is actually
running reads as a stall.

Byte counts are **doubles**, not integers — a download can exceed 2 GiB and
`base::Value` has no 64-bit integer type.

`filename` comes from `GetTargetFilePath()`, not `GetFullPath()`. The latter
names the intermediate `.crdownload` file, which may be renamed or disappear
mid-download.

### `context_menu`

```jsonc
{"kind":"context_menu","x":412,"y":260,
 "link_url":"https://example.com/","link_text":"Example",
 "src_url":"","selection_text":"","media_type":0,"is_editable":false}
```

Coordinates are relative to the guest's RenderView origin — the same space the
client's input mapper already works in.

**Every string here is renderer-supplied.** The chromium struct is literally
named `UntrustworthyContextMenuParams`. Truncate and escape; nothing in this
payload is a capability, only a description of what was clicked.

The guest always suppresses its own menu (a headless embedder has no native
menu surface), so the viewer drawing one is the only way a menu appears. The
menu's *actions* come back as ordinary input — a click, a clipboard write, a
navigation — which is why this is an event and not a request.

---

## Versioning

- `v` is an integer, currently `1`.
- A new **kind** is a minor change. Receivers must decline an unknown kind
  explicitly rather than ignore it — the guest is blocking on it, and an
  empty-dict response makes it apply its default immediately instead of
  waiting out the full deadline.
- A new optional **field** on an existing kind is minor. Receivers must ignore
  unknown fields.

# File-upload channel protocol (v1)

Wire format for transmitting file *content* from the **browser
client** to the **cloud Chromium**, carried over a dedicated
`RTCDataChannel` named `"files"` (separate from `"input"`,
`"clipboard"`, and `"cursor"`).

> **Status:** v1, frozen for first deployment. Shares the
> versioning policy of [`input-channel.md`](./input-channel.md) and
> [`clipboard-channel.md`](./clipboard-channel.md).
>
> **Companion implementation:** [`client/src/file-upload.ts`](../../client/src/file-upload.ts)
> + [`capture/build-integration/cb_file_upload_receiver.{h,cc}`](../../capture/build-integration/cb_file_upload_receiver.h).
>
> **The server side moved (2026-09).** This protocol was originally
> terminated by `capture/file-bridge/`, a Go sidecar reached over a
> localhost WebSocket, with `CbFileUploadRelay` forwarding DC frames to it.
> That relay was constructed with `url="off"` and therefore dropped every
> frame — file upload was inert on the wire for the whole time the sidecar
> existed, and the sidecar itself is deployed by nothing today
> (`infra/k8s/cloud-browser-session.yaml` still names it; the compose stack
> and the standalone image do not).
>
> `CbFileUploadReceiver` now terminates the protocol **in the browser
> process**. The wire format below is unchanged; what changed is who reads
> it and how a completed file reaches the page. Two differences follow, both
> marked inline: `target_selector` is ignored, and `attached_via` can no
> longer be `"domSetFileInputFiles"`.

The drag-drop **events** documented in `input-channel.md` v1.1
forward `DataTransfer.types` only (file content is dropped per the
v1 file-drag policy). This protocol is the path that actually moves
bytes — invoked when the client wants to deliver a file the user
selected or dropped.

---

## Transport

- **Channel:** `RTCDataChannel` named `"files"`, ordered, reliable
  (default `{ ordered: true }`). One channel may carry **multiple
  concurrent uploads**, demultiplexed by `upload_id`.
- **Direction:** primarily client → server (chunks). Server →
  client carries progress / complete / error envelopes for each
  upload.
- **Encoding:** UTF-8 JSON, one envelope per `send`. Chunk payload
  bytes are base64-encoded inside the JSON envelope (so the entire
  channel speaks one wire format).

---

## Envelopes

All envelopes share the standard outer shape:

```jsonc
{ "v": 1, "type": "...", "upload_id": "<uuid>", /* type-specific fields */ }
```

The `t` and `seq` fields used by other channels are omitted —
`upload_id` + per-envelope identifiers handle correlation.

### Client → Server

#### `file_upload_start`

```jsonc
{
  "v": 1,
  "type": "file_upload_start",
  "upload_id": "01958a01-...",
  "name": "report.pdf",
  "mime_type": "application/pdf",
  "size": 1234567,
  "sha256": "9f8e...d3",
  "target_selector": "input[type=file]"  // optional
}
```

| field            | type   | required | notes                                                      |
|------------------|--------|----------|------------------------------------------------------------|
| `upload_id`      | string | yes      | UUID v4, stable across all envelopes for this upload       |
| `name`           | string | yes      | original filename. Server sanitises before disk write       |
| `mime_type`      | string | yes      | client-asserted MIME; server may re-sniff and reject        |
| `size`           | int    | yes      | total content length in bytes; ≤ size cap (default 100 MB) |
| `sha256`         | string | yes      | hex-lowercase SHA-256 of the full content; server verifies |
| `target_selector`| string | no       | **Ignored since 2026-09.** The old sidecar used it to drive `DOM.setFileInputFiles` over CDP. The in-process receiver instead resolves the `content::FileSelectListener` that chromium itself handed to `RunFileChooser`, so the file goes to the input the page actually opened — no selector, and no way for the client to nominate a different one. Still accepted on the wire so an older client does not break. |

#### `file_upload_chunk`

```jsonc
{ "v": 1, "type": "file_upload_chunk",
  "upload_id": "01958a01-...",
  "seq": 0,
  "data": "<base64...>" }
```

| field        | type   | notes                                                         |
|--------------|--------|---------------------------------------------------------------|
| `seq`        | int    | starts at 0, strictly monotonic per `upload_id`               |
| `data`       | string | base64-encoded raw bytes; chunk size ≤ 1 MB raw (default 64 KB)|

The server reassembles by `seq` order. Out-of-order chunks (rare —
the channel is ordered) are buffered or dropped per server policy.

#### `file_upload_end`

```jsonc
{ "v": 1, "type": "file_upload_end", "upload_id": "01958a01-..." }
```

Triggers the server to flush the buffered chunks to disk, verify
the SHA-256, run any virus-scan / MIME / size policy checks, attach
the file via CDP, and reply with `file_upload_complete` or
`file_upload_error`.

#### `file_upload_cancel`

```jsonc
{ "v": 1, "type": "file_upload_cancel", "upload_id": "01958a01-..." }
```

Client requests abort. Server discards any buffered chunks for the
`upload_id` and replies with `file_upload_error` `code: "cancelled"`.

### Server → Client

#### `file_upload_progress` (best-effort)

```jsonc
{ "v": 1, "type": "file_upload_progress",
  "upload_id": "01958a01-...",
  "bytes_received": 196608,
  "bytes_total":    1234567 }
```

Emitted at most once per N KB to avoid flooding the back channel.
Clients may render this as a progress bar; missing progress events
are not an error (clients can fall back to local "bytes sent"
counters).

#### `file_upload_complete`

```jsonc
{ "v": 1, "type": "file_upload_complete",
  "upload_id": "01958a01-...",
  "server_path": "/var/lib/chromeless-uploads/<sess>/<upload_id>__report.pdf",
  "attached_via": "domSetFileInputFiles" }
```

| field         | type   | notes                                                           |
|---------------|--------|-----------------------------------------------------------------|
| `server_path` | string | absolute path inside the chromium container                     |
| `attached_via`| string | `"fileChooser"` (the page had a chooser open and the file was given to it) \| `"none"` (written to disk, no chooser was waiting). `"domSetFileInputFiles"` was the sidecar's selector path and is **no longer produced** — the receiver has no CDP client and never drives the DOM. |

#### `file_upload_error`

```jsonc
{ "v": 1, "type": "file_upload_error",
  "upload_id": "01958a01-...",
  "code":  "sha256_mismatch",
  "error": "expected 9f8e…d3, got a1b2…cd" }
```

Codes emitted by `CbFileUploadReceiver`. The client treats every code the
same way (it rejects the upload's promise), so a code it does not know is
not an error — but a receiver that invents one is invisible in the UI, which
is why this list is exhaustive rather than illustrative.

| `code` value      | meaning                                                        |
|-------------------|----------------------------------------------------------------|
| `too_large`       | declared `size` > 100 MiB, or the received bytes exceeded it mid-stream |
| `session_quota`   | this session has already written 512 MiB                        |
| `out_of_order`    | a chunk's `seq` was not the next expected one                   |
| `bad_base64`      | a chunk's `data` did not decode                                 |
| `write_failed`    | the disk write failed, or the file could not be read back to hash it |
| `truncated`       | `file_upload_end` arrived but bytes on disk ≠ declared `size`    |
| `hash_mismatch`   | sha256 of what landed ≠ the client's declared `sha256`          |
| `cancelled`       | the client sent `file_upload_cancel`                            |

**Codes the sidecar defined and the receiver does NOT emit**, because the
control behind each is gone with it — do not write client code that waits
for one: `size_limit_exceeded` (now `too_large`), `chunk_too_large` (folded
into `too_large`), `mime_not_allowlisted` (no sniffing — see Security §3),
`sha256_mismatch` (now `hash_mismatch`), `out_of_order_chunk` (now
`out_of_order`), `virus_detected` (no scan — see Security §7),
`attach_failed` (there is no CDP attach step to fail), `internal_error`.

---

## Backpressure

The data channel exposes `RTCDataChannel.bufferedAmount` and
`bufferedamountlow` events. The client's `FileUploadChannel`
implementation MUST:

1. Set `dc.bufferedAmountLowThreshold = N` (default 256 KB).
2. Before sending each chunk, check `dc.bufferedAmount`. If above a
   high-water mark (default 1 MB), suspend sending and `await` the
   next `bufferedamountlow` event.
3. Resume sending after the event fires.

This avoids OOMing the SCTP/SRTP send buffer on a slow link. The
server is not expected to backpressure; it logs and keeps draining.

---

## Size and chunk policy

| Knob                  | Default    | Bound       | Notes                                          |
| --------------------- | ---------- | ----------- | ---------------------------------------------- |
| Chunk size (raw)      | 64 KB      | ≤ 1 MB      | base64 encodes 64 KB → ~88 KB on the wire      |
| Total upload size cap | 100 MB     | per-tenant  | enforced server-side; client SHOULD pre-check  |
| Concurrent uploads    | 4          | per channel | server may queue beyond this                   |
| Inactivity timeout    | 60 s       | per upload  | from last received chunk; sends `file_upload_error` `code: "internal_error"` and discards state |

---

## Security model

v1 is a deliberate narrow path — the file-upload pipe is the
single most dangerous surface in the cloud-browser product.

1. **User-action gated.** The protocol is only invoked as a
   consequence of an explicit user gesture inside the streamed
   region (drop, file-input click). Silent uploads from JS-only
   triggers are out of scope; the client SHOULD NOT expose
   `FileUploadChannel.uploadFile` to arbitrary page content.
2. **Size cap.** Hard-rejected at start; no streaming over.
3. **MIME allowlist.** ⚠️ **NOT IMPLEMENTED by the in-process
   receiver.** The sidecar re-sniffed the first 4 KB via
   `http.DetectContentType` and rejected on mismatch; that sidecar is
   deployed by nothing. `CbFileUploadReceiver` does not sniff and does not
   filter — the file goes to the page's own `<input>`, whose `accept`
   attribute is enforced by blink as it is for a local file.

   This is a deliberate narrowing of the old claim, not an oversight: a
   guest-side allowlist would have to disagree with the page's own
   validation to do anything, and the file never becomes executable or
   reachable outside `<profile>/Uploads`. If a deployment needs one, it
   belongs where the old one was — in front of the browser, not inside it.
4. **SHA-256 verification.** Mandatory. Mismatch ⇒ file deleted
   from disk + `file_upload_error`.
5. **Path traversal guard.** `name` is sanitised to remove path
   separators and normalised; the on-disk filename is
   `<upload_id>__<sanitized_name>` so two uploads with the same
   client-side name don't collide and a maliciously crafted name
   cannot escape the per-session directory.
6. **Per-session directory isolation.** Files land in
   `<profile>/Uploads/` (the profile is `--user-data-dir`, or a temp dir
   when that is unset). The receiver **deletes the whole directory on
   re-arm**, so one viewer's files are never visible to the next, and a
   session byte quota (512 MiB) bounds total disk use on top of the 100 MiB
   per-file cap. A rejected upload's partial bytes are deleted immediately —
   without that, a client retrying a bad hash could fill the disk while
   never incrementing the quota, which only counts successes.
7. **Virus scan stub.** ⚠️ **NOT IMPLEMENTED by the in-process receiver.**
   There is no scan and no stub. The sidecar's ClamAV plan died with the
   sidecar. Do not read this section as describing a control that exists.
8. **No retries on hash failure.** A failed SHA-256 is treated as
   tampering, not transient error. Client must explicitly start a
   new upload with a fresh `upload_id`.

The signaling layer is responsible for authenticating the peer
connection itself (Phase 3). The receiver trusts whatever speaks the
`files` DataChannel — i.e. the peer the broker paired this session with.

**Paths never come from the wire.** `upload_id` and `name` are each reduced
to a single `[A-Za-z0-9._-]` component before the on-disk name
`<upload_id>__<name>` is built, so no separator, `..`, or absolute path can
survive. The client's original filename is preserved separately and reaches
the page as `File.name` via `NativeFileInfo::display_name` — the page sees
what the user picked, while the filesystem only ever sees the sanitised
form.

---

## Versioning

- `v` is integer, currently `1`.
- Adding a new optional field to any envelope is minor (no `v`
  bump). Receivers MUST ignore unknown fields.
- Adding a new envelope `type` is minor. Receivers SHOULD ignore
  unknown types.
- Removing/renaming/repurposing a field bumps `v`.

---

## Test vectors

```jsonc
// 1 KB PDF upload, target a specific input.
{ "v":1, "type":"file_upload_start",
  "upload_id":"abc123",
  "name":"hello.pdf", "mime_type":"application/pdf",
  "size":1024, "sha256":"e3b0c4...b855",
  "target_selector":"#attachment" }
{ "v":1, "type":"file_upload_chunk", "upload_id":"abc123",
  "seq":0, "data":"<base64 of 1024 bytes>" }
{ "v":1, "type":"file_upload_end", "upload_id":"abc123" }

// Server reply (happy path):
{ "v":1, "type":"file_upload_complete",
  "upload_id":"abc123",
  "server_path":"/var/lib/chromeless-uploads/sess1/abc123__hello.pdf",
  "attached_via":"domSetFileInputFiles" }

// Or rejection:
{ "v":1, "type":"file_upload_error",
  "upload_id":"abc123",
  "code":"sha256_mismatch",
  "error":"expected e3b0c4..., got 02d4..." }
```

---

## Cross-references

- T46 — drag-and-drop event support (events only; this protocol is
  the content-bearing companion).
- T22 — input-bridge sidecar; same code shape applies (per-event-
  type CDP wrapping) for the inverse direction.
- T48 — signaling auth; per-tenant cap + MIME allowlist live in
  the same tenant config eventually.

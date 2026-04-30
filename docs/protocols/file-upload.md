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
> + [`capture/file-bridge/`](../../capture/file-bridge/).

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
| `target_selector`| string | no       | CSS selector for `<input type=file>` to attach to on completion. If omitted, the bridge waits for a `Page.fileChooserOpened` event and attaches to that. |

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
  "server_path": "/var/lib/cb-uploads/<sess>/<upload_id>__report.pdf",
  "attached_via": "domSetFileInputFiles" }
```

| field         | type   | notes                                                           |
|---------------|--------|-----------------------------------------------------------------|
| `server_path` | string | absolute path inside the chromium container                     |
| `attached_via`| string | `"domSetFileInputFiles"` (selector path) \| `"fileChooser"` \| `"none"` (file written to disk but not attached to any input) |

#### `file_upload_error`

```jsonc
{ "v": 1, "type": "file_upload_error",
  "upload_id": "01958a01-...",
  "code":  "sha256_mismatch",
  "error": "expected 9f8e…d3, got a1b2…cd" }
```

| `code` value             | meaning                                                                |
|--------------------------|------------------------------------------------------------------------|
| `size_limit_exceeded`    | `size` > tenant cap (default 100 MB)                                   |
| `chunk_too_large`        | a single chunk exceeds the chunk-size cap (default 1 MB raw)           |
| `mime_not_allowlisted`   | server-side MIME allowlist rejected `mime_type` and/or sniffed type    |
| `sha256_mismatch`        | hash of received bytes ≠ asserted `sha256`                              |
| `out_of_order_chunk`     | a chunk's `seq` was lower or duplicate vs the last accepted            |
| `truncated`              | `file_upload_end` arrived but received bytes < `size`                   |
| `virus_detected`         | virus-scan stub rejected the file (Phase 4 ClamAV integration)         |
| `cancelled`              | client sent `file_upload_cancel`                                       |
| `attach_failed`          | file written successfully but CDP attach failed                        |
| `internal_error`         | catch-all for unexpected server failure; log + retry                   |

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
3. **MIME allowlist.** Per-tenant configuration. Default
   allowlist for v1: `application/pdf`, `image/png`, `image/jpeg`,
   `image/gif`, `image/webp`, `text/plain`, `text/csv`. The bridge
   re-sniffs the first 4 KB on disk via `http.DetectContentType`
   and rejects on mismatch.
4. **SHA-256 verification.** Mandatory. Mismatch ⇒ file deleted
   from disk + `file_upload_error`.
5. **Path traversal guard.** `name` is sanitised to remove path
   separators and normalised; the on-disk filename is
   `<upload_id>__<sanitized_name>` so two uploads with the same
   client-side name don't collide and a maliciously crafted name
   cannot escape the per-session directory.
6. **Per-session directory isolation.** Files land in
   `/var/lib/cb-uploads/<session_id>/`; the session pod has no
   write access outside its mount. Phase 3 sandboxing (T44)
   applies on top.
7. **Virus scan stub.** v1 logs a stub line per upload; Phase 4
   wires in ClamAV's `clamd` over a Unix socket. The stub MUST
   reject when the integration env var declares ClamAV is
   unavailable but the tenant policy requires it.
8. **No retries on hash failure.** A failed SHA-256 is treated as
   tampering, not transient error. Client must explicitly start a
   new upload with a fresh `upload_id`.

The signaling layer is responsible for authenticating the peer
connection itself (Phase 3). The bridge trusts whatever speaks the
relay's localhost WebSocket inside the container.

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
  "server_path":"/var/lib/cb-uploads/sess1/abc123__hello.pdf",
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

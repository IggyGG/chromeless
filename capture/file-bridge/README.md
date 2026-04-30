# file-bridge

> **Status:** Phase 4 candidate (T74).
> **Owner:** `platform-dev`.
> **Companion docs:** [`docs/protocols/file-upload.md`](../../docs/protocols/file-upload.md)
> + [`client/src/file-upload.ts`](../../client/src/file-upload.ts).

Server-side sidecar for file *content* uploads from the WebRTC
client into the cloud Chromium. T46 added drag-drop **events** but
explicitly deferred file content; this is the bytes-transmission
companion. Per the project brief, file upload is "a notable rabbit
hole" — v1 is a deliberately narrow path (text + image MIME
allowlist, 100 MiB cap, mandatory SHA-256 verification).

## How

```
   browser client (T14) ──RTCDataChannel("files")──┐
                                                   │
                              relay (in streamer page or signaling)
                                                   │
                                            ws://localhost:9400/files
                                                   │
                              ┌────────────────────┴────────────────────┐
                              │  file-bridge  (this dir)                │
                              │  - reassemble chunks                    │
                              │  - verify SHA-256                       │
                              │  - re-sniff MIME (http.DetectContentType)│
                              │  - virus-scan stub (Phase 4 ClamAV)     │
                              │  - write to /var/lib/cb-uploads/<sess>/ │
                              │  - DOM.setFileInputFiles via CDP        │
                              └────────────────┬────────────────────────┘
                                               │
                                            CDP (ws://127.0.0.1:9222)
                                               │
                                       headless Chromium
```

## Build and run

```bash
go build -o file-bridge .

# Smoke test (no CDP, no real Chromium). Inbound envelopes still
# write to disk and the bridge replies with file_upload_complete /
# attached_via=none.
./file-bridge --dry-run --upload-dir /tmp/uploads --session-id test

# Production-ish: sidecar mode against a real Chromium.
./file-bridge --ws-addr 127.0.0.1:9400 --ws-path /files \
              --cdp-url http://127.0.0.1:9222 \
              --upload-dir /var/lib/cb-uploads \
              --session-id <sess>
```

## Flags

| flag                | default                        | meaning                                                  |
| ------------------- | ------------------------------ | -------------------------------------------------------- |
| `--ws-addr`         | `127.0.0.1:9400`               | WS source listen address                                 |
| `--ws-path`         | `/files`                       | WS source URL path                                       |
| `--cdp-url`         | `http://127.0.0.1:9222`        | Chromium DevTools base URL                               |
| `--upload-dir`      | `/var/lib/cb-uploads`          | Per-session upload root (subdir per session)             |
| `--session-id`      | `default`                      | Subdir under `--upload-dir`; should match the cb session |
| `--max-total-size`  | `104857600` (100 MiB)          | Hard upload size cap                                     |
| `--max-chunk-size`  | `1048576` (1 MiB raw)          | Max bytes per chunk (after base64-decode)                |
| `--mime-allowlist`  | `application/pdf,image/png,…`  | Comma-separated MIME types accepted on `file_upload_start` |
| `--dry-run`         | `false`                        | Skip CDP entirely; useful for tests / smoke              |

## Wire format

See [`docs/protocols/file-upload.md`](../../docs/protocols/file-upload.md)
for the v1 spec. Each envelope is one line of UTF-8 JSON over the
WebSocket:

- `file_upload_start` → bridge opens a tmp file, prepares SHA-256
- `file_upload_chunk` (× N, ordered by `seq`) → bridge writes + hashes
- `file_upload_end` → bridge verifies SHA-256, re-sniffs MIME, runs
  virus-scan stub, attaches via CDP, replies `file_upload_complete`
- `file_upload_cancel` → bridge deletes tmp file, replies
  `file_upload_error code: "cancelled"`

Server emits:

- `file_upload_progress` every 256 KiB received (and at end-of-file)
- `file_upload_complete` on success
- `file_upload_error` for any of the documented error codes

## Tests

```bash
go test ./...
```

Two test layers (matching capture/input-bridge / clipboard-bridge
patterns):

- **Pure-function tests** for `sanitizeName`, `mkAllowlist`,
  `parseFlags`.
- **Bridge happy + error paths** against a recordingCDP fake — drive
  envelopes via in-process `dispatch()` calls, assert the right CDP
  methods fire (`DOM.getDocument` → `DOM.querySelector` →
  `DOM.setFileInputFiles`) and the file lands on disk with matching
  bytes. Coverage:
  - happy path with selector → `attached_via: domSetFileInputFiles`
  - happy path without selector → `attached_via: none`
  - rejected size cap, MIME not allowlisted, SHA-256 mismatch,
    out-of-order chunk, truncated upload, sniff-after-write
    mismatch, cancel mid-stream, chunk for unknown id.

The full WebSocket → bridge → fake CDP integration lives in
[`tests/integration/file_upload_test.go`](../../tests/integration/file_upload_test.go).

## Docker

```bash
docker build -t file-bridge .
docker run --rm --network=host \
           -v /var/lib/cb-uploads:/var/lib/cb-uploads \
           file-bridge
```

Multi-stage; finishes in `gcr.io/distroless/static-debian12:nonroot`.
Runs `go test ./...` at build time.

## Compose / K8s tie-in

The bridge runs as a **sidecar** in the chromium pod so it can talk
to CDP via `127.0.0.1:9222` and share the per-session uploads
volume. The relay (in the streamer page or signaling) forwards
`RTCDataChannel("files")` messages into `ws://localhost:9400/files`
and writes server replies back into the data channel.

The K8s manifest at `infra/k8s/cloud-browser-session.yaml` adds the
bridge as a container in the same pod with a shared `emptyDir`
mount at `/var/lib/cb-uploads`.

## Known v1 gaps

- **Chunks held in memory until end.** Since v1's cap is 100 MiB
  per upload, a single concurrent upload uses up to ~100 MiB of
  resident memory. The disk-write happens incrementally so this is
  the *current* upload's chunks; the cap is the upload size, not
  the cumulative memory footprint. Phase 2 will stream-write
  without buffering.
- **One CDP attach mechanism in v1: `DOM.setFileInputFiles`.** The
  bridge calls `Page.setInterceptFileChooserDialog(true)` at install
  but the file-chooser branch isn't wired to the upload-id table
  yet — only the explicit `target_selector` path is implemented.
  Drag-and-drop into a JS-handled region is out of scope.
- **No CDP reconnect.** Crashes propagate; the orchestrator
  restarts the container.
- **No virus scan.** v1 is a stub that always accepts; ClamAV
  integration is Phase 4.
- **No per-tenant config.** All knobs are global flags; tenant-
  specific MIME allowlist and size cap come with T48's auth tokens.

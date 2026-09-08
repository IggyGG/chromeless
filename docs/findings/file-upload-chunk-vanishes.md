# A file upload's chunk reaches the guest and vanishes

**Status: OPEN — under diagnosis.** Reproduced against the live standalone
stack on 2026-09-08 with guest `cr7727-2b93ba93` (batch B). The chooser, the
request, the picker and the transport all work; the file never appears.

## Symptom

`tests/interactive --only uploads` gets four of five checks, then:

```
FAIL  the file reached the page's own <input>
      window.__file never populated. Client log said:
        INFO → file_upload start (page chooser) {"name":"...","size":27}
        ERR  file_upload failed code=write_failed could not read the file back
```

The guest log agrees, and shows the shape clearly:

```
CV2-UPLOAD: file chooser parked; awaiting an upload from the viewer
CV2-UPLOAD: upload <id> starting: chromeless-upload-probe.txt (27 bytes) -> .../Uploads/<id>__...
CV2-UPLOAD: upload <id> failed (write_failed): could not read the file back
CV2-UPLOAD: file chooser cancelled (no file selected)
```

`<profile>/Uploads/` does not exist afterwards — watched at 200 ms intervals
through the whole run, nothing is ever created.

## What is established

**The client sends the chunk.** Measured, not inferred: hooking
`RTCDataChannel.prototype.send` in the viewer's browser and driving a real
upload through the picker gives

```
CLIENT SENT on files: ['file_upload_start', 'file_upload_chunk', 'file_upload_end']
```

(`scratchpad/upload_probe.py`, which also proves the click landed and the
picker mounted, so the harness is not the variable.)

**`file_upload_start` is processed.** The guest logs the upload's name, size
and destination path, so the envelope parsed, the version matched, the
observer was bound and the map entry was created.

**The guest's error arrives 16 ms after start.** That is `file_upload_end`
being handled — so `end` was processed too. Only the chunk between them
produced nothing.

**Therefore the chunk is dropped inside `CbFileUploadReceiver::HandleChunk`,**
before `AppendChunk` is posted. Both write paths were verified separately: the
directory is created (`CreateDirectory` on the parent) and the first chunk now
uses `base::WriteFile`, since `AppendToFile` opens without `O_CREAT` and cannot
create a file — that was a real, separate bug, fixed in the same batch.

## Ruled out

| theory | how it was refuted |
| --- | --- |
| `!seq` rejecting `seq: 0` | `std::optional::operator bool` **is** `has_value()`. `cb_input_dispatch_mouse.cc:184` relies on exactly this for x/y coordinates that are legitimately 0. |
| protocol version mismatch | both sides are `1` (`file-upload.ts:20`, `cb_file_upload_receiver.cc:28`). |
| `upload_id` mangled by `SafeId` | a UUID survives `[A-Za-z0-9._-]` unchanged; start and chunk compute the same key. |
| `size` parsed as the wrong type | `FindDouble` accepts an integer JSON value (`cb_devtools_agent.cc:686` notes the same), and start succeeded anyway. |
| frame too large | the file is 27 bytes; one chunk. |
| observer not bound | `dc_host` drops unbound frames silently, but `start` and `end` both arrived. |
| the click missed the input | `window.__clicks` = 1, and the picker mounted. |

## Why it took two rebuild cycles

**Both drop paths in `HandleChunk` were silent.** A missing-field check with a
bare `return`, and an unknown-upload check behind `VLOG(1)` — invisible in a
production log. The failure surfaced four steps downstream as "could not read
the file back", which describes a consequence and names no cause.

This is CLAUDE.md's own rule (*a guard whose skipped path is silent is not a
guard*) reproduced by the person who had just re-read it. The lesson generalises
past this defect: **on a component whose only feedback loop is a 20-minute
build plus an image roll, a silent drop costs a full cycle every time.**

## Next step

`eee0fae` makes every path say why:

- `HandleEnvelope` logs each inbound type with its size — one line answers
  "did the chunk arrive at all?", which is the question two cycles were spent
  narrowing down from outside.
- both `HandleChunk` drops log at WARNING with the specific field or id.
- `HandleEnd` says explicitly when **no** chunk was accepted, since that is
  the cause of the read-back failure and is known at that point.

Roll that image, run `tests/interactive --only uploads`, and read
`/var/log/supervisor/chromium.err.log` (**not** `chromium.log`, which is empty
— stdout goes to `/dev/console`).

## Fix location

`capture/build-integration/cb_file_upload_receiver.cc`, `HandleChunk`.

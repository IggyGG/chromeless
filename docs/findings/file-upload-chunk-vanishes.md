# A file upload's chunk reaches the guest and vanishes

**Status: ROOT-CAUSED, fix awaiting live verification.** Reproduced against the live standalone
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

## Narrowed, 2026-09-08 (guest `cr7727-eee0fae6f80f`)

The diagnostics answered it in one line. The chunk **arrives and passes every
guard**:

```
CV2-UPLOAD: <- file_upload_start (233 bytes)
CV2-UPLOAD: upload <id> starting: chromeless-upload-probe.txt (27 bytes) -> .../Uploads/<id>__...
CV2-UPLOAD: <- file_upload_chunk (139 bytes)      <-- arrives, no warning
CV2-UPLOAD: <- file_upload_end (83 bytes)
CV2-UPLOAD: upload <id> failed (write_failed): could not read the file back
```

No drop warning fired, so `HandleChunk` reached its `PostTaskAndReplyWithResult`.
The file still never exists — re-watched at **10 ms** polling (the earlier
200 ms watch could not have seen a ~15 ms window; this one could and saw
nothing). The directory is writable: `mkdir` + `touch` as `cbuser` inside the
running container both succeed.

So the write task never ran. **The suspect is the task runner itself.** The
receiver built its own `base::ThreadPool::CreateSequencedTaskRunner` in the
constructor — and that call appears **nowhere else in `capture/`**. The
download delegate writes files in this same process and works, using
`base::ThreadPool::PostTask` with explicit traits; its own comment warns that
"no in-tree precedent is a cost paid hours later in the build lane".

`60e9c99` switches all five file operations to that shape. Whether it fixes
this is what the next roll says — do not record it as fixed until the uploads
suite passes.

## ROOT CAUSE, 2026-09-08 — a race between `end` and the chunk's write

**The task-runner theory above was WRONG.** Swapping the sequenced runner for
`base::ThreadPool` (guest `cr7727-60e9c992ed3b`) produced the identical
failure. Recorded rather than quietly folded into the eventual fix, because
"the previous theory was refuted" is the useful half of a diagnosis.

The actual sequence:

```
t0  HandleChunk   posts AppendChunk to the pool   -> returns immediately
t1  HandleEnd     posts HashFile to the pool      -> returns immediately
t2  HashFile runs. The write has not.
t3  OnFinalised(ok=false) -> "could not read the file back", ERASES the entry
t4  OnChunkWritten's reply lands -> find() fails -> silent return
```

The client sends the last chunk and `end` back to back; the guest handles
both on the UI sequence in microseconds, while the write is a pool task.
`HandleEnd` never waited for it.

**Two silent paths hid it from each other.** `OnChunkWritten`'s "entry already
gone" branch returned with no log, so the write's own outcome — success or
failure — was never reported. The only evidence anywhere was `end`'s error
message, which describes a consequence and names no cause. That is why three
image rolls went into theories about *why the write failed* when the write had
simply not happened yet.

### The fix (`24ebd98`)

`Upload` gains `writes_in_flight` and `end_pending`. `HandleEnd` defers when
writes are outstanding (and says so); the last write's reply calls
`FinaliseUpload` instead. `OnChunkWritten`'s orphan branch now LOGS.

### Cost, and what would have avoided it

Five build-and-roll cycles, ~20 minutes each. Every one of the three real
defects in this feature was a **silent path**, not a wrong computation:

| defect | how it presented |
| --- | --- |
| `AppendToFile` has no `O_CREAT` | `write_failed`, four steps from the cause |
| chunk-drop guards (`return`, `VLOG(1)`) | nothing at all |
| `OnChunkWritten` orphan branch | nothing at all |

On a component whose only feedback loop is a 20-minute build plus an image
roll, **a silent branch costs a full cycle every time it is hit.** The
per-frame log added in `eee0fae` is what finally made the shape visible, and
it should have been there from the first failure.

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

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.

#include "capture/build-integration/cb_file_upload_receiver.h"

#include <utility>

#include "base/base64.h"
// base::as_byte_span — the sha256 helper hashes a std::string as bytes.
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
// base::ToLowerASCII — the client's declared digest is normalised before
// it is compared against ours.
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "crypto/hash.h"

namespace cloud_browser {
namespace {

constexpr char kLog[] = "CV2-UPLOAD: ";
constexpr int kProtocolVersion = 1;

// File IO traits.
//
// A SEQUENCED runner built in the constructor is what this used to use, and
// it was the only base::ThreadPool::CreateSequencedTaskRunner anywhere in
// capture/ — the download delegate's own comment warns that "no in-tree
// precedent is a cost paid hours later in the build lane", and this cost
// three rolls: the chunk reached HandleChunk, passed every guard, was posted
// to that runner, and the file never appeared.
//
// cb_download_manager_delegate.cc:98 is the shape that demonstrably works in
// this process, so this matches it exactly.
//
// ORDERING, stated precisely, because dropping a sequenced runner is not
// free: two chunks posted to the pool can run on different threads. What
// makes that safe here is that they cannot be in flight at once — the
// receiver posts a chunk's write only from HandleChunk, which runs on the
// UI sequence, and the seq check (`*seq != up.next_seq`) rejects any chunk
// that arrives before the previous one has been counted. The client is also
// strictly serial (file-upload.ts awaits each send). If either of those ever
// changes, this needs a sequence again — and the first chunk's WriteFile
// truncates, so an out-of-order first write would silently discard data
// rather than fail loudly.
constexpr base::TaskTraits kFileIoTraits = {
    base::MayBlock(), base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN,
    base::TaskPriority::USER_VISIBLE};

// Everything the client can put in `name` becomes one path component, and
// nothing in it can escape the directory. Not a "sanitiser" in the sense of
// trying to preserve intent — a deliberate reduction to [A-Za-z0-9._-].
std::string SafeComponent(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (const char c : raw) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    out.push_back(ok ? c : '_');
  }
  if (out.empty() || out == "." || out == "..") {
    out = "upload";
  }
  if (out.size() > 128) {
    out.resize(128);
  }
  return out;
}

// The upload_id is used as a directory-name prefix, so it gets the same
// treatment. The client generates a UUID; a peer that does not is still
// harmless.
std::string SafeId(std::string_view raw) {
  std::string out = SafeComponent(raw);
  if (out.size() > 64) {
    out.resize(64);
  }
  return out;
}

// Blocking. Appends |bytes| to |path|, creating it on the first chunk.
//
// base::AppendToFile does NOT create the file. Its POSIX implementation
// opens with `O_WRONLY | O_APPEND` and no `O_CREAT`
// (base/files/file_util_posix.cc:1269), so on a path that does not exist
// yet it returns false — and its header says only "Appends |data| to
// |filename|", which is exactly true and reads as if it creates one.
//
// Measured on the live guest 2026-09-08: every upload failed at the FIRST
// chunk with `write_failed`, having already parked the chooser, sent the
// request, shown the viewer a picker and received the bytes. The whole
// path worked and the file was never created. So the first chunk goes
// through WriteFile (which does create) and the rest append.
bool AppendChunk(const base::FilePath& path,
                 const std::string& bytes,
                 bool first_chunk) {
  if (!base::CreateDirectory(path.DirName())) {
    LOG(ERROR) << kLog << "could not create " << path.DirName().value();
    return false;
  }
  const bool ok = first_chunk
                      // Truncating create. A leftover file here can only be a
                      // stale partial from an abandoned upload_id; appending
                      // to it would silently corrupt the new one.
                      ? base::WriteFile(path, bytes)
                      : base::AppendToFile(path, bytes);
  // Read the size straight back. A write that returns true and leaves no
  // file is the exact shape being chased here (the hash then fails with
  // "could not read the file back" and the write looks innocent), so the
  // two facts are logged together on ONE line.
  const std::optional<int64_t> on_disk = base::GetFileSize(path);
  LOG(INFO) << kLog << (first_chunk ? "WriteFile" : "AppendToFile") << " -> "
            << (ok ? "ok" : "FAILED") << ", " << bytes.size()
            << " bytes; file is now "
            << (on_disk ? base::NumberToString(*on_disk) : std::string("ABSENT"))
            << " at " << path.value();
  return ok;
}

// Blocking. Reads the finished file back and returns its SHA-256 as
// lowercase hex, plus its size. Reading it back (rather than hashing the
// chunks as they arrive) is deliberate: it verifies what is ON DISK, which
// is what will be handed to the page.
std::string HashFile(const base::FilePath& path, int64_t* size_out) {
  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    // Distinguish "the file is gone" from "it is there and unreadable".
    // Those are different bugs — a delete racing the read, versus
    // permissions or a directory in the way — and the caller only reports
    // "could not read the file back", which covers both.
    const std::optional<int64_t> sz = base::GetFileSize(path);
    LOG(ERROR) << kLog << "ReadFileToString FAILED for " << path.value()
               << "; the file is "
               << (sz ? base::NumberToString(*sz) + " bytes on disk"
                      : std::string("ABSENT"));
    return std::string();
  }
  *size_out = static_cast<int64_t>(contents.size());
  const auto digest = crypto::hash::Sha256(base::as_byte_span(contents));
  return base::ToLowerASCII(base::HexEncode(digest));
}

}  // namespace

CbFileUploadReceiver::CbFileUploadReceiver(
    signaling::CbDataChannelHost* dc_host,
    base::FilePath profile_dir,
    scoped_refptr<base::SequencedTaskRunner> ui_runner)
    : dc_host_(dc_host),
      uploads_dir_(profile_dir.Append(FILE_PATH_LITERAL("Uploads"))),
      ui_runner_(std::move(ui_runner)) {
  LOG(INFO) << kLog << "receiver bound — uploads land in "
            << uploads_dir_.value() << " (max " << (kMaxUploadBytes >> 20)
            << " MiB per file, " << (kMaxSessionUploadBytes >> 20)
            << " MiB per session)";
}

CbFileUploadReceiver::~CbFileUploadReceiver() {
  // The listener contract outlives us: a parked chooser MUST be resolved.
  CancelChooser();
}

void CbFileUploadReceiver::BeginChooser(
    scoped_refptr<content::FileSelectListener> listener,
    blink::mojom::FileChooserParams::Mode mode) {
  // A page that opens a second chooser has abandoned the first, and the
  // first's listener still has to be resolved or chromium CHECKs on release.
  CancelChooser();
  pending_listener_ = std::move(listener);
  pending_mode_ = mode;
  // Bound the park HERE. The control-channel request the delegate sends has
  // its own deadline, but it is cancelled as soon as the viewer answers — so
  // a viewer who says "sending a file" and then closes the tab would leave
  // this listener parked forever, and the page's <input type=file> would
  // never fire change and never can. Unretained: the timer is a member and
  // stops in the destructor.
  chooser_deadline_.Start(
      FROM_HERE, kFileChooserDeadline,
      base::BindOnce(
          [](CbFileUploadReceiver* self) {
            LOG(WARNING) << kLog
                         << "no upload arrived within the chooser deadline; "
                            "cancelling so the page is not left waiting";
            self->CancelChooser();
          },
          base::Unretained(this)));
  LOG(INFO) << kLog << "file chooser parked; awaiting an upload from the "
                       "viewer";
}

void CbFileUploadReceiver::CancelChooser() {
  // Stop unconditionally: BeginChooser calls this to displace an earlier
  // chooser, and a live timer from that one would otherwise fire against
  // the NEW listener at the old deadline.
  chooser_deadline_.Stop();
  if (!pending_listener_) {
    return;
  }
  scoped_refptr<content::FileSelectListener> listener =
      std::move(pending_listener_);
  pending_listener_ = nullptr;
  listener->FileSelectionCanceled();
  LOG(INFO) << kLog << "file chooser cancelled (no file selected)";
}

void CbFileUploadReceiver::ResetForNewSession() {
  CancelChooser();

  // Count what is still being written before dropping the map, because the
  // map is the only record of it.
  //
  // The wipe below and any outstanding chunk write are separate
  // base::ThreadPool tasks with no shared sequence, so the delete can run
  // FIRST and the late write then re-creates the directory and leaves an
  // orphan the next session never clears. The old sequenced runner ordered
  // these implicitly; it was removed while chasing a theory that turned out
  // to be wrong, so this ordering is a regression introduced by that change
  // rather than a pre-existing one.
  //
  // A count, not a barrier: the wipe is best-effort housekeeping and a
  // rare orphan is bounded by the per-session quota, so this LOGS the
  // condition rather than delaying re-arm on it. If the log ever shows a
  // non-zero count in practice, that is the signal to make the delete wait
  // on the writes properly.
  int32_t still_writing = 0;
  for (const auto& [id, up] : uploads_) {
    still_writing += up.writes_in_flight;
  }
  if (still_writing > 0) {
    LOG(WARNING) << kLog << "wiping Uploads/ with " << still_writing
                 << " chunk write(s) still in flight — a late write can "
                    "re-create the directory and leave an orphan";
  }

  uploads_.clear();
  session_bytes_ = 0;
  // Fire-and-forget: the next session's writes create the directory again.
  base::ThreadPool::PostTask(
      FROM_HERE, kFileIoTraits,
      base::BindOnce(
          [](const base::FilePath& dir) { base::DeletePathRecursively(dir); },
          uploads_dir_));
  LOG(INFO) << kLog << "Uploads/ wiped for the next viewer";
}

void CbFileUploadReceiver::OnMessage(const webrtc::DataBuffer& buffer) {
  if (buffer.binary) {
    LOG(WARNING) << kLog << "binary frame on the files channel — v1 is JSON "
                            "text; dropped";
    return;
  }
  std::string json(reinterpret_cast<const char*>(buffer.data.data()),
                   buffer.data.size());
  // Hop to the UI sequence: everything below touches uploads_, the listener,
  // and chromium objects that are UI-thread-only.
  ui_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&CbFileUploadReceiver::HandleEnvelope,
                     weak_factory_.GetWeakPtr(), std::move(json)));
}

void CbFileUploadReceiver::OnEnvelopeForTesting(const std::string& json) {
  HandleEnvelope(json);
}

void CbFileUploadReceiver::HandleEnvelope(const std::string& json) {
  // JSON_PARSE_RFC, like every other ReadDict caller in this tree
  // (cb_control_channel.cc:257, cb_clipboard_relay.cc:109,
  // cb_devtools_agent.cc:664). The options argument is REQUIRED at 7727 —
  // it has no default — and omitting it is a compile error, which is what
  // this line was until the lane found it.
  std::optional<base::DictValue> parsed =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!parsed) {
    LOG(WARNING) << kLog << "inbound frame is not a JSON object — dropped";
    return;
  }
  const std::optional<int> v = parsed->FindInt("v");
  const std::string* type = parsed->FindString("type");
  if (!v || *v != kProtocolVersion || !type) {
    LOG(WARNING) << kLog << "envelope is not a v1 file_upload_* frame — "
                            "dropped";
    return;
  }
  // Log EVERY inbound type with its size. This is what would have told me
  // in one look, on 2026-09-08, whether a chunk reached the guest at all —
  // instead of two rebuild cycles spent narrowing it down from the outside.
  // One line per frame is affordable: a 100 MiB upload at 64 KiB chunks is
  // 1600 lines, and the alternative is a channel whose failures are only
  // visible as a symptom four steps downstream.
  LOG(INFO) << kLog << "<- " << *type << " (" << json.size() << " bytes)";
  if (*type == "file_upload_start") {
    HandleStart(*parsed);
  } else if (*type == "file_upload_chunk") {
    HandleChunk(*parsed);
  } else if (*type == "file_upload_end") {
    HandleEnd(*parsed);
  } else if (*type == "file_upload_cancel") {
    HandleCancel(*parsed);
  } else {
    VLOG(1) << kLog << "ignoring inbound type " << *type;
  }
}

void CbFileUploadReceiver::HandleStart(const base::DictValue& data) {
  const std::string* id = data.FindString("upload_id");
  const std::string* name = data.FindString("name");
  const std::string* sha = data.FindString("sha256");
  const std::optional<double> size = data.FindDouble("size");
  if (!id || !name || !sha || !size) {
    LOG(WARNING) << kLog << "file_upload_start missing a required field";
    return;
  }
  const std::string upload_id = SafeId(*id);
  const int64_t declared = static_cast<int64_t>(*size);

  if (declared <= 0 || declared > kMaxUploadBytes) {
    ReplyError(upload_id, "too_large",
               "file is " + base::NumberToString(declared) +
                   " bytes; the limit is " +
                   base::NumberToString(kMaxUploadBytes));
    return;
  }
  if (session_bytes_ + declared > kMaxSessionUploadBytes) {
    ReplyError(upload_id, "session_quota",
               "this session has already uploaded " +
                   base::NumberToString(session_bytes_) +
                   " bytes; the limit is " +
                   base::NumberToString(kMaxSessionUploadBytes));
    return;
  }

  Upload up;
  up.name = SafeComponent(*name);
  up.display_name = base::UTF8ToUTF16(*name);
  up.sha256 = base::ToLowerASCII(*sha);
  up.declared_size = declared;
  // The ONLY place a path is built. upload_id and name are both reduced to a
  // single safe component first; nothing from the wire reaches the
  // filesystem verbatim.
  up.path = uploads_dir_.AppendASCII(upload_id + "__" + up.name);
  LOG(INFO) << kLog << "upload " << upload_id << " starting: " << up.name
            << " (" << declared << " bytes) -> " << up.path.value();
  uploads_[upload_id] = std::move(up);
}

void CbFileUploadReceiver::HandleChunk(const base::DictValue& data) {
  const std::string* id = data.FindString("upload_id");
  const std::string* b64 = data.FindString("data");
  const std::optional<int> seq = data.FindInt("seq");
  // `.has_value()` spelled out because the three operands convert to bool
  // for DIFFERENT reasons — the pointers by non-null, the optional by
  // contains-a-value — and the uniform `!x` reads as if seq==0 would be
  // rejected. It would not (std::optional::operator bool IS has_value), and
  // cb_input_dispatch.cc:196 spells it out for the same reason. Kept
  // explicit so nobody has to re-derive that, as I did.
  if (!id || !b64 || !seq.has_value()) {
    // LOG, not a bare return. A dropped chunk surfaces four steps later as
    // "could not read the file back" from file_upload_end, and a silent
    // return here means the log cannot tell you WHY the file is missing —
    // which cost a full diagnose-rebuild-roll cycle on 2026-09-08. Same
    // rule as CLAUDE.md's "a guard whose skipped path is silent is not a
    // guard": the drop must report differently from the success.
    LOG(WARNING) << kLog << "file_upload_chunk missing a required field"
                 << " (upload_id=" << (id ? "yes" : "NO")
                 << " data=" << (b64 ? "yes" : "NO")
                 << " seq=" << (seq.has_value() ? "yes" : "NO") << ")";
    return;
  }
  const std::string upload_id = SafeId(*id);
  auto it = uploads_.find(upload_id);
  if (it == uploads_.end()) {
    // Was VLOG(1), i.e. invisible in production. Same reasoning as the
    // field check above — this is one of only two ways a chunk vanishes
    // without a reply, and both have to be readable in a normal log.
    LOG(WARNING) << kLog << "chunk for unknown upload " << upload_id
                 << " — cancelled, never started, or the id did not survive"
                    " sanitising; dropped";
    return;
  }
  Upload& up = it->second;
  if (*seq != up.next_seq) {
    // The data channel is ordered, so this means frames were lost or the
    // client is buggy. Either way the file would be wrong; fail loudly
    // rather than write corrupt bytes and fail the hash later.
    AbandonUpload(it, "out_of_order",
                  "expected seq " + base::NumberToString(up.next_seq) +
                      ", got " + base::NumberToString(*seq));
    return;
  }
  std::string bytes;
  if (!base::Base64Decode(*b64, &bytes)) {
    AbandonUpload(it, "bad_base64", "chunk did not decode");
    return;
  }
  const int64_t chunk_bytes = static_cast<int64_t>(bytes.size());
  if (up.received + chunk_bytes > up.declared_size) {
    AbandonUpload(it, "too_large",
                  "more bytes arrived than the declared size");
    return;
  }
  ++up.next_seq;
  // Count the chunk NOW, before the write is even queued. This is the
  // running total the size guard above tests against and the number
  // file_upload_progress reports, and it must be the DECODED length —
  // counting the base64 (as an earlier draft did by passing b64->size()
  // through to the reply) overstates every file by a third, and leaving it
  // at zero (as the same draft did, by never incrementing it at all) makes
  // the guard test 0 > declared_size on every chunk, i.e. never fire.
  up.received += chunk_bytes;
  ++up.writes_in_flight;
  const base::FilePath path = up.path;
  // next_seq was incremented above, so seq 0 is the chunk that has just
  // taken it to 1. That chunk creates the file; the rest append.
  const bool first_chunk = up.next_seq == 1;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, kFileIoTraits,
      base::BindOnce(&AppendChunk, path, std::move(bytes), first_chunk),
      base::BindOnce(&CbFileUploadReceiver::OnChunkWritten,
                     weak_factory_.GetWeakPtr(), upload_id, chunk_bytes));
}

void CbFileUploadReceiver::AbandonUpload(
    std::map<std::string, Upload>::iterator it,
    const std::string& code,
    const std::string& message) {
  const std::string upload_id = it->first;
  const base::FilePath path = it->second.path;
  uploads_.erase(it);
  base::ThreadPool::PostTask(FROM_HERE, kFileIoTraits,
                             base::GetDeleteFileCallback(path));
  ReplyError(upload_id, code, message);
}

void CbFileUploadReceiver::OnChunkWritten(std::string upload_id,
                                          int64_t /*chunk_bytes*/,
                                          bool ok) {
  auto it = uploads_.find(upload_id);
  if (it == uploads_.end()) {
    // The upload was abandoned (or wrongly finalised) while this write was
    // in flight. Worth saying: a silent return here is what MASKED the race
    // this function now closes — the write reply arrived after OnFinalised
    // had already erased the entry, so the only thing the log showed was
    // end's "could not read the file back".
    LOG(WARNING) << kLog << "write reply for upload " << upload_id
                 << " arrived after it was erased (ok=" << ok << ")";
    return;
  }
  if (it->second.writes_in_flight > 0) {
    --it->second.writes_in_flight;
  }
  if (!ok) {
    AbandonUpload(it, "write_failed",
                  "could not write to " + it->second.path.value());
    return;
  }
  // Progress is best-effort per the spec; the client falls back to its own
  // bytes-sent count. Report what is actually on disk after each chunk.
  ReplyProgress(upload_id, it->second.received, it->second.declared_size);

  // file_upload_end got here first and deferred to us. This is the last
  // write, so the file is now complete on disk and safe to hash.
  if (it->second.end_pending && it->second.writes_in_flight == 0) {
    FinaliseUpload(upload_id);
  }
}

void CbFileUploadReceiver::HandleEnd(const base::DictValue& data) {
  const std::string* id = data.FindString("upload_id");
  if (!id) {
    return;
  }
  const std::string upload_id = SafeId(*id);
  auto it = uploads_.find(upload_id);
  if (it == uploads_.end()) {
    LOG(WARNING) << kLog << "file_upload_end for unknown upload "
                 << upload_id << " — nothing to finalise";
    return;
  }
  if (it->second.next_seq == 0) {
    LOG(WARNING) << kLog << "file_upload_end for " << upload_id
                 << " but NOT ONE chunk was accepted — the read-back below"
                    " will fail; look for a chunk-drop warning above";
  }

  // THE RACE THIS CLOSES.
  //
  // The client sends the last chunk and `end` back to back, and the guest
  // handles both on the UI sequence within microseconds — while the chunk's
  // WRITE is a pool task that has not run yet. Hashing here read a file that
  // did not exist and reported "could not read the file back": a truthful
  // description of a consequence, naming no cause.
  //
  // It was invisible from both ends. OnChunkWritten's reply landed after
  // OnFinalised had already erased the map entry, so its `find` failed and it
  // returned silently; the only evidence was end's own error. Three rolls
  // went into theories about the task runner before the ordering itself was
  // the answer — see docs/findings/file-upload-chunk-vanishes.md.
  if (it->second.writes_in_flight > 0) {
    it->second.end_pending = true;
    LOG(INFO) << kLog << "file_upload_end for " << upload_id << " arrived with "
              << it->second.writes_in_flight
              << " write(s) still in flight; finalising when they land";
    return;
  }
  FinaliseUpload(upload_id);
}

void CbFileUploadReceiver::FinaliseUpload(const std::string& upload_id) {
  auto it = uploads_.find(upload_id);
  if (it == uploads_.end()) {
    return;
  }
  const base::FilePath path = it->second.path;
  // Log the path AS A STRING here and in AppendChunk. If a write succeeds and
  // a read of "the same" path fails a millisecond later, the two paths being
  // different is the first thing to rule out, and only printing both rules
  // it out.
  LOG(INFO) << kLog << "finalising " << upload_id << "; hashing "
            << path.value();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, kFileIoTraits,
      base::BindOnce(
          [](const base::FilePath& p) {
            int64_t size = 0;
            std::string hash = HashFile(p, &size);
            return std::make_pair(std::move(hash), size);
          },
          path),
      base::BindOnce(
          [](base::WeakPtr<CbFileUploadReceiver> self, std::string id,
             base::FilePath p, std::pair<std::string, int64_t> result) {
            if (!self) {
              return;
            }
            // THE BUG, and it hid behind an error message that named the
            // wrong thing for five image rolls.
            //
            // This used to read:
            //
            //   OnFinalised(..., std::move(result.first), result.second,
            //               !result.first.empty());
            //
            // `result.first` is MOVED into argument 3 and READ in argument
            // 5, and C++ leaves argument evaluation order UNSPECIFIED. On
            // this toolchain argument 3 won, so `ok` was computed from a
            // moved-from (empty) string: every upload reported failure
            // while the file sat correct and complete on disk.
            //
            // It compiled clean, it linted clean, and the failure surfaced
            // as OnFinalised's "could not read the file back" — a message
            // about a read that, the logs eventually proved, had never
            // failed once.
            //
            // Compute the flag FIRST, into a named local. Never read a
            // value in the same call that moves it.
            const bool hashed_ok = !result.first.empty();
            self->OnFinalised(std::move(id), std::move(p),
                              std::move(result.first), result.second,
                              hashed_ok);
          },
          weak_factory_.GetWeakPtr(), upload_id, path));
}

void CbFileUploadReceiver::OnFinalised(std::string upload_id,
                                       base::FilePath path,
                                       std::string actual_sha256,
                                       int64_t total_bytes,
                                       bool ok) {
  auto it = uploads_.find(upload_id);
  if (it == uploads_.end()) {
    return;
  }
  const Upload up = it->second;

  // Every rejection below leaves bytes on disk that nothing will ever read,
  // so each goes through AbandonUpload (erase + delete + reply) rather than
  // a bare ReplyError. An upload that fails its hash ten times must not cost
  // ten files' worth of the guest's disk, and the session quota credited
  // below only counts uploads that SUCCEEDED, so nothing else bounds a
  // retry loop.
  if (!ok) {
    // "could not read the file back" was this message for five image rolls,
    // and it was WRONG: `ok` is "HashFile returned a hash", which a
    // use-after-move could falsify while the read had succeeded. The read
    // now logs its own failure (see HashFile), so this message says only
    // what it actually knows.
    AbandonUpload(it, "hash_failed",
                  "the file could not be hashed — see the guest log for "
                  "whether the read itself failed");
    return;
  }
  if (total_bytes != up.declared_size) {
    AbandonUpload(it, "truncated",
                  "expected " + base::NumberToString(up.declared_size) +
                      " bytes, have " + base::NumberToString(total_bytes));
    return;
  }
  if (!up.sha256.empty() && actual_sha256 != up.sha256) {
    // The client hashes what it read off the disk; we hash what landed on
    // ours. A mismatch means the bytes changed in transit, and handing a
    // corrupt file to the page is worse than failing the upload.
    AbandonUpload(it, "hash_mismatch",
                  "expected " + up.sha256 + ", got " + actual_sha256);
    return;
  }
  // Committed: past every rejection, so the entry can go and the bytes stay.
  uploads_.erase(it);
  session_bytes_ += total_bytes;

  const bool attached = ResolveChooserWith(path, up.display_name);
  LOG(INFO) << kLog << "upload " << upload_id << " complete: " << total_bytes
            << " bytes, sha ok, "
            << (attached ? "attached to the page's file chooser"
                         : "stored (no chooser was waiting)");
  ReplyComplete(upload_id, path, attached);
}

bool CbFileUploadReceiver::ResolveChooserWith(
    const base::FilePath& path,
    const std::u16string& display_name) {
  if (!pending_listener_) {
    return false;
  }
  // The park is over — stop the deadline before it can fire against a
  // listener we have already resolved.
  chooser_deadline_.Stop();
  scoped_refptr<content::FileSelectListener> listener =
      std::move(pending_listener_);
  pending_listener_ = nullptr;

  // display_name is what the page reads as File.name. Passing it empty makes
  // blink fall back to the base of file_path, which here is our sanitised
  // "<upload_id>__<name>" — so a site that echoes the filename, or validates
  // its extension after our sanitiser mangled it, sees the wrong thing.
  // Read from file_chooser.mojom:88 on the pinned tree.
  auto info = blink::mojom::FileChooserFileInfo::NewNativeFile(
      blink::mojom::NativeFileInfo::New(path, display_name,
                                        std::vector<std::u16string>()));
  std::vector<blink::mojom::FileChooserFileInfoPtr> files;
  files.push_back(std::move(info));
  // base_dir is EMPTY for everything except kUploadFolder — the listener
  // header is explicit ("This is an empty FilePath otherwise",
  // file_select_listener.h:24). We reject kUploadFolder in the delegate, so
  // it is always empty here; passing uploads_dir_ would advertise an
  // enumeration root that does not describe this selection.
  listener->FileSelected(std::move(files), base::FilePath(), pending_mode_);
  return true;
}

void CbFileUploadReceiver::HandleCancel(const base::DictValue& data) {
  const std::string* id = data.FindString("upload_id");
  if (!id) {
    return;
  }
  const std::string upload_id = SafeId(*id);
  // The chooser goes first either way: a cancel for an upload we never saw
  // still means "the viewer is not sending a file", and the page must not be
  // left waiting on a chooser nobody will satisfy.
  CancelChooser();
  auto it = uploads_.find(upload_id);
  if (it != uploads_.end()) {
    AbandonUpload(it, "cancelled", "the viewer cancelled the upload");
    return;
  }
  ReplyError(upload_id, "cancelled", "the viewer cancelled the upload");
}

void CbFileUploadReceiver::ReplyProgress(const std::string& upload_id,
                                         int64_t got, int64_t total) {
  base::DictValue env;
  env.Set("v", kProtocolVersion);
  env.Set("type", "file_upload_progress");
  env.Set("upload_id", upload_id);
  env.Set("bytes_received", static_cast<double>(got));
  env.Set("bytes_total", static_cast<double>(total));
  Send(std::move(env));
}

void CbFileUploadReceiver::ReplyComplete(const std::string& upload_id,
                                         const base::FilePath& path,
                                         bool attached) {
  base::DictValue env;
  env.Set("v", kProtocolVersion);
  env.Set("type", "file_upload_complete");
  env.Set("upload_id", upload_id);
  env.Set("server_path", path.value());
  env.Set("attached_via", attached ? "fileChooser" : "none");
  Send(std::move(env));
}

void CbFileUploadReceiver::ReplyError(const std::string& upload_id,
                                      const std::string& code,
                                      const std::string& message) {
  LOG(WARNING) << kLog << "upload " << upload_id << " failed (" << code
               << "): " << message;
  base::DictValue env;
  env.Set("v", kProtocolVersion);
  env.Set("type", "file_upload_error");
  env.Set("upload_id", upload_id);
  env.Set("code", code);
  env.Set("error", message);
  Send(std::move(env));
}

void CbFileUploadReceiver::Send(base::DictValue envelope) {
  std::string json;
  if (!base::JSONWriter::Write(envelope, &json)) {
    LOG(ERROR) << kLog << "failed to serialise a reply envelope";
    return;
  }
  dc_host_->SendAsync(
      signaling::CbDcLabel::kFiles, std::move(json), ui_runner_,
      base::BindOnce([](bool ok, std::string message) {
        if (!ok) {
          LOG(WARNING) << kLog << "reply not delivered: " << message;
        }
      }));
}

}  // namespace cloud_browser

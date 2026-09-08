// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cb_file_upload_receiver.{h,cc} — the guest half of file upload (Batch B).
//
// # What this replaces
//
// `cb_file_upload_relay.{h,cc}` was built around a WebSocket bridge to a
// helper process that never existed: it is constructed with `url="off"`, so
// `disabled()` is permanently true and every inbound frame is dropped before
// it reaches any logic. Its own boot line says so —
// "file transfer is INERT on this channel". The client half
// (`client/src/file-upload.ts`) has been complete for months and chunks a
// File into exactly the envelopes this class consumes; nothing was reading
// them. This is that reader.
//
// # The flow
//
//   1. A page runs `<input type=file>`. Chromium calls
//      `WebContentsDelegate::RunFileChooser` with a `FileSelectListener`.
//      `CbWebContentsDelegate` parks the listener here (`BeginChooser`) and
//      asks the viewer over the `control` channel ("file_chooser").
//   2. The client shows a picker and uploads the chosen file over the
//      `files` channel: `file_upload_start`, N × `file_upload_chunk`
//      (base64), `file_upload_end`.
//   3. This class writes the bytes to `<profile>/Uploads/<id>__<name>`,
//      verifies the SHA-256 the client declared, and hands the path to the
//      parked listener via `FileSelected` — exactly once.
//   4. `file_upload_complete` (or `file_upload_error`) goes back on the
//      files channel; the wire contract is `docs/protocols/file-upload.md`,
//      which the client already implements and which is NOT changed here.
//
// # Rules that are not negotiable
//
//   * **A path never comes from the wire.** The client sends a NAME; this
//     class sanitises it and joins it under the profile's `Uploads/`
//     directory itself. A `name` of "../../etc/passwd" writes to
//     `Uploads/.._.._etc_passwd`.
//   * **Exactly one listener resolution.** `FileSelectListener` requires
//     `FileSelected` or `FileSelectionCanceled` before release
//     (api-pins: content::FileSelectListener). Every exit path here runs one
//     of them, including teardown and re-arm.
//   * **Bytes are written on a MayBlock pool sequence**, never the UI
//     thread — the same discipline `cb_download_manager_delegate.cc` uses
//     for its own file IO.
//   * **Uploads/ is wiped on re-arm.** A new viewer must not find the
//     previous one's files, and the directory is inside a profile that now
//     persists (`--user-data-dir` is honoured since Batch A).
//   * **Per-session byte cap.** `kMaxSessionUploadBytes` bounds what one
//     viewer can write into the guest's disk.

#ifndef CAPTURE_BUILD_INTEGRATION_CB_FILE_UPLOAD_RECEIVER_H_
#define CAPTURE_BUILD_INTEGRATION_CB_FILE_UPLOAD_RECEIVER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "capture/signaling/cb_dc_host.h"
#include "content/public/browser/file_select_listener.h"
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom.h"
#include "third_party/webrtc/api/data_channel_interface.h"

namespace cloud_browser {

// Hard ceiling on one upload. Matches the client's MAX_TOTAL_SIZE
// (client/src/file-upload.ts) so a file the client would refuse to send is
// also one the guest would refuse to accept — the two limits disagreeing is
// how a 101 MiB upload gets half-written and then rejected.
inline constexpr int64_t kMaxUploadBytes = 100LL * 1024 * 1024;

// Ceiling on everything one session writes. A viewer that uploads 100 MiB
// twenty times is filling the guest's disk, not using the feature.
inline constexpr int64_t kMaxSessionUploadBytes = 512LL * 1024 * 1024;

// How long a parked file chooser waits for the viewer before it gives up and
// cancels — the page gets "no file selected", which is what a human closing
// the picker produces. Deliberately long: a person browsing for a file is
// slow, and the failure mode of a short deadline is a picker that closes
// itself while they are still looking.
inline constexpr base::TimeDelta kFileChooserDeadline = base::Minutes(5);

class CbFileUploadReceiver : public webrtc::DataChannelObserver {
 public:
  // |dc_host| and |profile_dir| must outlive this object. |ui_runner| is the
  // UI sequence; every listener resolution and every reply happens there.
  CbFileUploadReceiver(signaling::CbDataChannelHost* dc_host,
                       base::FilePath profile_dir,
                       scoped_refptr<base::SequencedTaskRunner> ui_runner);
  CbFileUploadReceiver(const CbFileUploadReceiver&) = delete;
  CbFileUploadReceiver& operator=(const CbFileUploadReceiver&) = delete;
  ~CbFileUploadReceiver() override;

  // Park a chooser: the next completed upload is handed to |listener|.
  // Replaces (and cancels) any previously parked one — a page that opens a
  // second chooser has abandoned the first.
  void BeginChooser(scoped_refptr<content::FileSelectListener> listener,
                    blink::mojom::FileChooserParams::Mode mode);

  // Resolve a parked chooser with "the user picked nothing". Safe with
  // nothing parked. Called on teardown, on re-arm, and on the deadline.
  void CancelChooser();

  // Delete everything under <profile>/Uploads and reset the session byte
  // count. Called on re-arm so a new viewer starts clean.
  void ResetForNewSession();

  base::WeakPtr<CbFileUploadReceiver> AsWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // webrtc::DataChannelObserver:
  void OnMessage(const webrtc::DataBuffer& buffer) override;
  void OnStateChange() override {}
  void OnBufferedAmountChange(uint64_t) override {}
  bool IsOkToCallOnTheNetworkThread() override { return false; }

  // Test seam: feed one JSON envelope as if it arrived on the channel.
  void OnEnvelopeForTesting(const std::string& json);

 private:
  // One upload in flight. The client may only have one at a time per the
  // protocol, but the map is keyed by upload_id so a stale chunk from an
  // abandoned upload is dropped rather than appended to the current one.
  struct Upload {
    std::string name;         // sanitised — this is the ON-DISK name only
    // The name as the client sent it. It is NEVER used to build a path; it
    // becomes NativeFileInfo::display_name, which is what the page reads as
    // File.name. Without it the page sees our sanitised
    // "<upload_id>__<name>" instead of the file the user picked — an empty
    // display_name makes blink fall back to the base part of file_path
    // (file_chooser.mojom:88).
    std::u16string display_name;
    std::string sha256;       // as declared by the client, lowercase hex
    int64_t declared_size = 0;
    int64_t received = 0;
    int32_t next_seq = 0;
    base::FilePath path;
    // Chunk writes posted to the pool that have not replied yet.
    //
    // file_upload_end arrives while these are still in flight — the client
    // sends chunk and end back-to-back and the guest handles both on the UI
    // sequence in microseconds, while the write is a pool task. Hashing then
    // reads a file that does not exist yet and reports "could not read the
    // file back", which describes the symptom of a race and names no cause.
    int32_t writes_in_flight = 0;
    // Set when file_upload_end arrived early. Finalisation runs from the
    // last write's reply instead.
    bool end_pending = false;
  };

  void HandleEnvelope(const std::string& json);
  void HandleStart(const base::DictValue& data);
  void HandleChunk(const base::DictValue& data);
  void HandleEnd(const base::DictValue& data);
  // Hash the finished file and resolve the chooser. Called from HandleEnd
  // when nothing is in flight, or from OnChunkWritten when the last
  // outstanding write lands.
  void FinaliseUpload(const std::string& upload_id);
  void HandleCancel(const base::DictValue& data);

  // Runs on the UI sequence with the result of the blocking write.
  void OnChunkWritten(std::string upload_id, int64_t bytes, bool ok);
  void OnFinalised(std::string upload_id,
                   base::FilePath path,
                   std::string actual_sha256,
                   int64_t total_bytes,
                   bool ok);

  // Erase an upload, delete whatever it already wrote, and tell the client
  // why. Every abandon path goes through here: a partial file left behind is
  // invisible (nothing reads Uploads/ but the chooser) and unbounded (the
  // session quota only counts uploads that SUCCEEDED, so a client retrying a
  // bad chunk could fill the disk without ever tripping it).
  void AbandonUpload(std::map<std::string, Upload>::iterator it,
                     const std::string& code,
                     const std::string& message);

  void ReplyProgress(const std::string& upload_id, int64_t got, int64_t total);
  void ReplyComplete(const std::string& upload_id,
                     const base::FilePath& path,
                     bool attached);
  void ReplyError(const std::string& upload_id,
                  const std::string& code,
                  const std::string& message);
  void Send(base::DictValue envelope);

  // Hand |path| to the parked listener, exactly once. Returns true if a
  // listener was waiting (i.e. the file was attached to a real chooser).
  bool ResolveChooserWith(const base::FilePath& path,
                          const std::u16string& display_name);

  const raw_ptr<signaling::CbDataChannelHost> dc_host_;
  const base::FilePath uploads_dir_;
  const scoped_refptr<base::SequencedTaskRunner> ui_runner_;

  std::map<std::string, Upload> uploads_;
  int64_t session_bytes_ = 0;

  scoped_refptr<content::FileSelectListener> pending_listener_;
  blink::mojom::FileChooserParams::Mode pending_mode_ =
      blink::mojom::FileChooserParams::Mode::kOpen;
  // The listener MUST be resolved exactly once before it is released, and
  // nothing outside this object can be relied on to do it: the viewer may
  // answer "a file is coming" and then drop the connection mid-upload, and
  // the control channel's own deadline is cancelled the moment that answer
  // arrives. So the park itself carries the bound. Same discipline as
  // cb_control_channel.h's "every request resolves, and MUST NOT depend on
  // the portal to do so".
  base::OneShotTimer chooser_deadline_;

  base::WeakPtrFactory<CbFileUploadReceiver> weak_factory_{this};
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CB_FILE_UPLOAD_RECEIVER_H_

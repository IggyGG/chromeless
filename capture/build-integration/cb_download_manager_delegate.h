// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbDownloadManagerDelegate — give a download somewhere to go.
//
// WHY THIS EXISTS
//
// The worker shipped a half-state that is worse than either end of it.
// CbWebContentsDelegate::CanDownload returns true and logs the attempt, so
// a download is PERMITTED and observable — but
// CloudBrowserBrowserContext::GetDownloadManagerDelegate returned nullptr,
// and //content's DownloadManagerImpl cannot determine a target without a
// delegate. The bytes go nowhere.
//
// From the user's seat that is the worst possible shape: the link is
// clickable, the click is accepted, the page's onclick handler runs, and
// then nothing happens. No error, no file, no indication that the browser
// decided not to. "Downloads silently do nothing" was diagnosable only by
// reading the embedder.
//
// WHAT THIS DOES AND DELIBERATELY DOES NOT DO
//
// It lands the file inside the guest container, under the browser
// context's own path. That is genuinely useful on its own — a page that
// downloads a file and then re-reads it (export/re-import flows, PDF
// generators that fetch and then display, anything using a blob: URL round
// trip) now works where it previously wedged.
//
// It does NOT yet relay the bytes to the viewer. That needs the kFiles
// channel plus a backpressure gate (see cb_dc_host.h:378 — 32 MiB at
// 64 KiB/chunk is 512 sends, and an ungated loop balloons the SCTP send
// buffer and stalls every other channel including input). Landing the
// delegate first is deliberate: it is the half that makes the guest
// correct, it is independently verifiable, and it does not require a new
// wire contract. The relay is the next change, not this one.
//
// TARGET PATH RULES, learned from //content rather than guessed
//
//   * DownloadItemImpl DCHECKs that intermediate_path.DirName() ==
//     target_path.DirName() (download_item_impl.cc:1837 at 7727). The
//     intermediate file is therefore the target plus a suffix, never a
//     path in a scratch directory.
//   * An EMPTY target_path is not "use a default" — it is a silent cancel.
//     Every path through DetermineDownloadTarget must produce a real one.
//   * DetermineDownloadTarget returns bool and takes a
//     download::DownloadTargetCallback* (a POINTER, and the callback is
//     moved out of it). Returning true means "I own this callback"; the
//     callback must then run exactly once or the download hangs forever
//     in TARGET_PENDING.
//
// This class is intentionally close to content/shell's
// ShellDownloadManagerDelegate, which is the reference implementation for
// an embedder with no UI to prompt with. Where it differs, the difference
// is commented.

#ifndef CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_DOWNLOAD_MANAGER_DELEGATE_H_
#define CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_DOWNLOAD_MANAGER_DELEGATE_H_

#include <stdint.h>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_target_info.h"
#include "content/public/browser/download_manager_delegate.h"

namespace content {
class DownloadManager;
}  // namespace content

namespace cloud_browser {

class CbDownloadManagerDelegate : public content::DownloadManagerDelegate {
 public:
  CbDownloadManagerDelegate();

  CbDownloadManagerDelegate(const CbDownloadManagerDelegate&) = delete;
  CbDownloadManagerDelegate& operator=(const CbDownloadManagerDelegate&) =
      delete;

  ~CbDownloadManagerDelegate() override;

  // Must be called before the manager issues any download. Not passed to
  // the constructor because the BrowserContext is not fully initialised at
  // the point the delegate is created — the same ordering constraint
  // content/shell documents on its own SetDownloadManager.
  void SetDownloadManager(content::DownloadManager* manager);

  // content::DownloadManagerDelegate:
  void Shutdown() override;
  bool DetermineDownloadTarget(
      download::DownloadItem* download,
      download::DownloadTargetCallback* callback) override;
  bool ShouldOpenDownload(
      download::DownloadItem* item,
      content::DownloadOpenDelayedCallback callback) override;
  void GetNextId(content::DownloadIdCallback callback) override;

  // Overrides the directory downloads land in. Exists for tests; the
  // production path derives it from the BrowserContext.
  void SetDownloadDirForTesting(const base::FilePath& dir);

 private:
  // Runs on a MayBlock() ThreadPool sequence: it touches the filesystem to
  // avoid clobbering an existing file, which must not happen on the UI
  // thread.
  static void GenerateFilename(const GURL& url,
                               const std::string& content_disposition,
                               const std::string& suggested_filename,
                               const std::string& mime_type,
                               const base::FilePath& download_dir,
                               base::OnceCallback<void(const base::FilePath&)>
                                   filename_determined_callback);

  void OnDownloadPathGenerated(download::DownloadTargetCallback callback,
                               const base::FilePath& suggested_path);

  raw_ptr<content::DownloadManager> download_manager_ = nullptr;
  base::FilePath download_dir_;
  uint32_t next_download_id_ = download::DownloadItem::kInvalidId + 1;

  base::WeakPtrFactory<CbDownloadManagerDelegate> weak_factory_{this};
};

}  // namespace cloud_browser

#endif  // CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_DOWNLOAD_MANAGER_DELEGATE_H_

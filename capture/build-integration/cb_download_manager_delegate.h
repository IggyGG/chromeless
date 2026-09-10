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

#include <set>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_target_info.h"
// CV2-DOWNLOAD: this class is now also a DownloadManager::Observer, so the
// manager header is part of its interface rather than a forward decl.
#include "content/public/browser/download_manager.h"
#include "content/public/browser/download_manager_delegate.h"

namespace content {
class DownloadManager;
}  // namespace content

namespace cloud_browser {

// CV2-DOWNLOAD: also a DownloadManager::Observer and a DownloadItem::Observer.
//
// The delegate already decides WHERE a download lands; observing lets it say
// that a download is HAPPENING. Before this, a file appeared in the guest's
// profile and the viewer was told nothing at all — the interactive suite had
// to read the guest's filesystem over kubectl exec to prove downloads worked,
// because there was no other evidence anywhere.
//
// Both observer faces on one class rather than a separate object: the
// delegate already outlives every item (the BrowserContext owns it), already
// holds the download directory, and adding a second lifetime here would be
// two things to get wrong instead of one.
class CbDownloadManagerDelegate : public content::DownloadManagerDelegate,
                                  public content::DownloadManager::Observer,
                                  public download::DownloadItem::Observer {
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

  // content::DownloadManager::Observer:
  void OnDownloadCreated(content::DownloadManager* manager,
                         download::DownloadItem* item) override;
  void ManagerGoingDown(content::DownloadManager* manager) override;

  // download::DownloadItem::Observer:
  void OnDownloadUpdated(download::DownloadItem* item) override;
  void OnDownloadDestroyed(download::DownloadItem* item) override;

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

  // Tell the viewer where this download has got to. No-op with no control
  // channel, which is the between-sessions case.
  void EmitDownloadEvent(download::DownloadItem* item, const char* phase);

  raw_ptr<content::DownloadManager> download_manager_ = nullptr;
  // Items we have added ourselves as an observer to, so teardown removes
  // exactly those. A DownloadItem outlives its download (it stays in
  // history), and observing one twice would double every event.
  std::set<raw_ptr<download::DownloadItem>> observed_items_;
  base::FilePath download_dir_;
  uint32_t next_download_id_ = download::DownloadItem::kInvalidId + 1;

  base::WeakPtrFactory<CbDownloadManagerDelegate> weak_factory_{this};
};

}  // namespace cloud_browser

#endif  // CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_DOWNLOAD_MANAGER_DELEGATE_H_

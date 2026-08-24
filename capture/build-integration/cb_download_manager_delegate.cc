// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cloud-browser/capture/build-integration/cb_download_manager_delegate.h"

#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/task/thread_pool.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/download_manager.h"
#include "net/base/filename_util.h"
#include "url/gurl.h"

namespace cloud_browser {

CbDownloadManagerDelegate::CbDownloadManagerDelegate() = default;

CbDownloadManagerDelegate::~CbDownloadManagerDelegate() = default;

void CbDownloadManagerDelegate::SetDownloadManager(
    content::DownloadManager* manager) {
  download_manager_ = manager;
}

void CbDownloadManagerDelegate::Shutdown() {
  // Drop the raw_ptr before the manager goes away. raw_ptr is not just a
  // pointer at 7727 — a dangling one is a detected fault, not a latent
  // one, so this is load-bearing rather than tidiness.
  download_manager_ = nullptr;
}

void CbDownloadManagerDelegate::SetDownloadDirForTesting(
    const base::FilePath& dir) {
  download_dir_ = dir;
}

void CbDownloadManagerDelegate::GetNextId(content::DownloadIdCallback callback) {
  // Monotonic and never kInvalidId. //content treats kInvalidId as "no id
  // yet" and will re-request, so handing it out once is an infinite loop.
  std::move(callback).Run(next_download_id_++);
}

bool CbDownloadManagerDelegate::ShouldOpenDownload(
    download::DownloadItem* /*item*/,
    content::DownloadOpenDelayedCallback /*callback*/) {
  // "Open" means hand the file to the OS shell. There is no desktop in the
  // guest container and no user at it; the honest answer is no. Returning
  // true here would have //content try to launch a handler that cannot
  // exist. The callback is only used when returning false AND deferring,
  // which we never do — we answer synchronously.
  return false;
}

bool CbDownloadManagerDelegate::DetermineDownloadTarget(
    download::DownloadItem* download,
    download::DownloadTargetCallback* callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // Resolved lazily, not in SetDownloadManager: the BrowserContext's path
  // is not reliably available at the point the delegate is installed.
  // content/shell carries the same comment for the same reason.
  if (download_dir_.empty() && download_manager_) {
    download_dir_ = download_manager_->GetBrowserContext()->GetPath().Append(
        FILE_PATH_LITERAL("Downloads"));
  }
  if (download_dir_.empty()) {
    // No manager and no test override. Cancel EXPLICITLY rather than
    // running the callback with an empty target_path, which //content also
    // treats as a cancel but which reads at the call site as a bug.
    LOG(WARNING) << "CbDownloadManagerDelegate: no download directory; "
                    "cancelling download";
    std::move(*callback).Run(download::DownloadTargetInfo());
    return true;
  }

  // A forced path (Save-Page-As, and some blob: flows) is already decided;
  // honouring it is required, not optional. intermediate == target here
  // because there is no rename step to protect.
  if (!download->GetForcedFilePath().empty()) {
    download::DownloadTargetInfo info;
    info.target_path = download->GetForcedFilePath();
    info.intermediate_path = download->GetForcedFilePath();
    std::move(*callback).Run(std::move(info));
    return true;
  }

  // Filename generation touches the filesystem (uniquifying against what
  // is already there), so it must not run on the UI thread.
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN,
       base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          &CbDownloadManagerDelegate::GenerateFilename, download->GetURL(),
          download->GetContentDisposition(), download->GetSuggestedFilename(),
          download->GetMimeType(), download_dir_,
          // GenerateFilename posts this back to the UI thread itself. That
          // is content/shell's shape (shell_download_manager_delegate.cc:140)
          // and it is used here in preference to base::BindPostTask, which
          // would be the FIRST use of that helper anywhere in capture/ —
          // and this tree cannot be compiled locally, so "no in-tree
          // precedent" is a cost paid hours later in the build lane.
          base::BindOnce(&CbDownloadManagerDelegate::OnDownloadPathGenerated,
                         weak_factory_.GetWeakPtr(), std::move(*callback))));

  // true = "this delegate owns the callback and will run it". Returning
  // false after having moved the callback out would strand the download in
  // TARGET_PENDING forever.
  return true;
}

// static
void CbDownloadManagerDelegate::GenerateFilename(
    const GURL& url,
    const std::string& content_disposition,
    const std::string& suggested_filename,
    const std::string& mime_type,
    const base::FilePath& download_dir,
    base::OnceCallback<void(const base::FilePath&)>
        filename_determined_callback) {
  // "download" is the last-resort stem, matching content/shell. Never
  // empty: an empty name yields an empty target_path, i.e. a silent cancel.
  base::FilePath generated_name =
      net::GenerateFileName(url, content_disposition, std::string(),
                            suggested_filename, mime_type, "download");

  if (!base::PathExists(download_dir)) {
    base::CreateDirectory(download_dir);
  }

  // Uniquify rather than overwrite. TARGET_DISPOSITION_OVERWRITE is the
  // struct default, so without this a second download of the same name
  // destroys the first one's bytes with no warning.
  base::FilePath suggested_path =
      base::GetUniquePath(download_dir.Append(generated_name));
  if (suggested_path.empty()) {
    // GetUniquePath exhausted its attempts. Fall back to the plain path
    // rather than returning empty, which would cancel silently.
    suggested_path = download_dir.Append(generated_name);
  }

  // Hop back to the UI thread. OnDownloadPathGenerated DCHECKs it is on
  // UI, and DownloadTargetCallback must be run there.
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(filename_determined_callback), suggested_path));
}

void CbDownloadManagerDelegate::OnDownloadPathGenerated(
    download::DownloadTargetCallback callback,
    const base::FilePath& suggested_path) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  download::DownloadTargetInfo info;
  info.target_path = suggested_path;
  // MUST share a directory with target_path: DownloadItemImpl DCHECKs
  // intermediate_path.DirName() == target_path.DirName()
  // (download_item_impl.cc:1837). AddExtension keeps it in the same
  // directory by construction; a scratch-dir intermediate would crash a
  // debug build the first time anyone downloaded anything.
  info.intermediate_path =
      suggested_path.AddExtension(FILE_PATH_LITERAL(".crdownload"));

  LOG(INFO) << "CbDownloadManagerDelegate: download target "
            << info.target_path.AsUTF8Unsafe();

  std::move(callback).Run(std::move(info));
}

}  // namespace cloud_browser

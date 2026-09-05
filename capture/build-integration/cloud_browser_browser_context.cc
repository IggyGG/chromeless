// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserBrowserContext — see cloud_browser_browser_context.h.

#include "capture/build-integration/cloud_browser_browser_context.h"

#include <atomic>
#include <cstdint>
#include <memory>

#include "base/files/file_path.h"
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/strings/stringprintf.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "components/keyed_service/core/simple_dependency_manager.h"
#include "components/keyed_service/core/simple_factory_key.h"
#include "components/keyed_service/core/simple_key_map.h"

namespace cloud_browser {

namespace {

std::atomic<uint64_t> g_next_profile_id{0};

// The switch name Chrome uses (chrome/common/chrome_switches.h, which this
// //content embedder does not link). Spelled here rather than taken from
// //content, which has no such switch of its own.
constexpr char kUserDataDirSwitch[] = "user-data-dir";

base::FilePath NextProfilePath() {
  const uint64_t profile_id =
      g_next_profile_id.fetch_add(1, std::memory_order_relaxed);

  // --user-data-dir, when given, is the profile's home — the FIRST context
  // gets it verbatim. infra/launch-chromeless.sh has passed
  // `--user-data-dir=/home/cbuser/.config/chromium` since the beginning and
  // it was read by nothing: every profile went under DIR_TEMP, so the
  // cookies, history and logins the flag exists to keep died with the
  // container, and a volume mounted at that path persisted an empty
  // directory. The second and later contexts (CreateNewTarget's per-target
  // ones) keep their own numbered dirs beneath it so two contexts never
  // share a storage partition.
  const base::CommandLine& cmd = *base::CommandLine::ForCurrentProcess();
  if (cmd.HasSwitch(kUserDataDirSwitch)) {
    const base::FilePath root = cmd.GetSwitchValuePath(kUserDataDirSwitch);
    if (!root.empty()) {
      return profile_id == 0
                 ? root
                 : root.AppendASCII(base::StringPrintf(
                       "context_%llu",
                       static_cast<unsigned long long>(profile_id)));
    }
  }

  base::FilePath tmp_dir;
  CHECK(base::PathService::Get(base::DIR_TEMP, &tmp_dir));
  return tmp_dir.AppendASCII(
      base::StringPrintf("cloud_browser_profile_%llu",
                         static_cast<unsigned long long>(profile_id)));
}

}  // namespace

CloudBrowserBrowserContext::CloudBrowserBrowserContext() {
  InitWhileIOAllowed();
  // Register this BrowserContext with chromium's KeyedService factories.
  // Without this call, GetForBrowserContext-style accessors on KeyedServices
  // (used pervasively across //content) trip a CHECK because the context
  // is not in the dependency graph.
  BrowserContextDependencyManager::GetInstance()
      ->CreateBrowserContextServices(this);
}

CloudBrowserBrowserContext::~CloudBrowserBrowserContext() {
  // Mirrors shell_browser_context.cc + headless_browser_context_impl.cc:
  // the SimpleDependencyManager must be torn down AFTER the
  // BrowserContextDependencyManager so SimpleKeyedServices that depend on
  // BrowserContextKeyedServices see the dependency in the right order.
  NotifyWillBeDestroyed();
  DependencyManager::PerformInterlockedTwoPhaseShutdown(
      BrowserContextDependencyManager::GetInstance(), this,
      SimpleDependencyManager::GetInstance(), simple_factory_key_.get());
  SimpleKeyMap::GetInstance()->Dissociate(this);
  ShutdownStoragePartitions();
}

void CloudBrowserBrowserContext::InitWhileIOAllowed() {
  path_ = NextProfilePath();
  // Best-effort create; if the dir already exists this is a no-op.
  // We don't propagate the failure — chromium's storage layer will
  // surface a clearer error if the path turns out to be unwritable.
  base::CreateDirectory(path_);

  simple_factory_key_ =
      std::make_unique<SimpleFactoryKey>(path_, /*is_off_the_record=*/false);
  SimpleKeyMap::GetInstance()->Associate(this, simple_factory_key_.get());
}

std::unique_ptr<content::ZoomLevelDelegate>
CloudBrowserBrowserContext::CreateZoomLevelDelegate(
    const base::FilePath& /*partition_path*/) {
  // No persisted zoom levels — we're a single-tab server-side worker.
  return nullptr;
}

base::FilePath CloudBrowserBrowserContext::GetPath() const {
  return path_;
}

bool CloudBrowserBrowserContext::IsOffTheRecord() {
  // Not incognito. Persistent disk storage simplifies chromium's
  // internal cache lifetime — the temp-dir parent is wiped on
  // container restart anyway, so persistence buys us no leaked state.
  return false;
}

content::DownloadManagerDelegate*
CloudBrowserBrowserContext::GetDownloadManagerDelegate() {
  // Was nullptr, which produced the worst possible half-state:
  // CbWebContentsDelegate::CanDownload returns true and LOGS the attempt,
  // so a download was permitted and observable — and then //content's
  // DownloadManagerImpl could not determine a target without a delegate,
  // so the bytes went nowhere. A clickable link, an accepted click, and
  // nothing happening, with no error anywhere.
  //
  // Lazy rather than constructed eagerly: this is called after the context
  // is fully initialised, which is when the profile path this delegate
  // needs is actually reliable.
  if (!download_manager_delegate_) {
    download_manager_delegate_ = std::make_unique<CbDownloadManagerDelegate>();
    download_manager_delegate_->SetDownloadManager(GetDownloadManager());
  }
  return download_manager_delegate_.get();
}

content::BrowserPluginGuestManager*
CloudBrowserBrowserContext::GetGuestManager() {
  return nullptr;
}

::storage::SpecialStoragePolicy*
CloudBrowserBrowserContext::GetSpecialStoragePolicy() {
  return nullptr;
}

content::PlatformNotificationService*
CloudBrowserBrowserContext::GetPlatformNotificationService() {
  return nullptr;
}

content::PushMessagingService*
CloudBrowserBrowserContext::GetPushMessagingService() {
  return nullptr;
}

content::StorageNotificationService*
CloudBrowserBrowserContext::GetStorageNotificationService() {
  return nullptr;
}

content::SSLHostStateDelegate*
CloudBrowserBrowserContext::GetSSLHostStateDelegate() {
  return nullptr;
}

content::PermissionControllerDelegate*
CloudBrowserBrowserContext::GetPermissionControllerDelegate() {
  // Returning nullptr means the default PermissionController denies all
  // permission prompts. The worker has no UI to surface a prompt
  // anyway, so this is the right behaviour.
  return nullptr;
}

content::ReduceAcceptLanguageControllerDelegate*
CloudBrowserBrowserContext::GetReduceAcceptLanguageControllerDelegate() {
  return nullptr;
}

content::ClientHintsControllerDelegate*
CloudBrowserBrowserContext::GetClientHintsControllerDelegate() {
  return nullptr;
}

content::BackgroundFetchDelegate*
CloudBrowserBrowserContext::GetBackgroundFetchDelegate() {
  return nullptr;
}

content::BackgroundSyncController*
CloudBrowserBrowserContext::GetBackgroundSyncController() {
  return nullptr;
}

content::BrowsingDataRemoverDelegate*
CloudBrowserBrowserContext::GetBrowsingDataRemoverDelegate() {
  return nullptr;
}

}  // namespace cloud_browser

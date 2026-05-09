// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserBrowserContext — see cloud_browser_browser_context.h.

#include "capture/build-integration/cloud_browser_browser_context.h"

#include <atomic>
#include <cstdint>
#include <memory>

#include "base/files/file_path.h"
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

base::FilePath NextProfilePath() {
  base::FilePath tmp_dir;
  CHECK(base::PathService::Get(base::DIR_TEMP, &tmp_dir));
  const uint64_t profile_id =
      g_next_profile_id.fetch_add(1, std::memory_order_relaxed);
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
  // No downloads. If a page tries to trigger one, chromium's default
  // path will silently drop the request — fine for the worker's
  // streaming-only use case.
  return nullptr;
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

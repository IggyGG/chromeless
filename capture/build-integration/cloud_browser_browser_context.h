// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserBrowserContext — minimal content::BrowserContext subclass.
//
// content::BrowserContext is a pure-virtual surface that every chromium
// embedder must subclass to teach the browser process about its profile
// (where state lives on disk, whether it's incognito, which delegates
// hand back permission/notification/storage/etc. services). Without an
// instance of this living somewhere in the browser process, no
// WebContents can be created — and without a WebContents,
// DevToolsAgentHost has nothing to register, so chromium's
// remote-debugging HTTP listener never binds.
//
// We hand back nullptr for almost every optional delegate (download
// manager, notifications, push, SSL state, permissions, …); the worker
// is single-tenant, single-tab, server-side and has no use for them.
// The only things we actually populate are GetPath() (a temp dir under
// base::DIR_TEMP) and IsOffTheRecord() (false — we want stable storage
// to persist for the duration of the worker's lifetime so chromium's
// internal caches don't churn).
//
// We deliberately mirror the headless_browser_context_impl shape, not
// shell_browser_context — content_shell drags in download manager
// delegates, mock background sync controllers, leveldb origin trials
// and a permission manager that we don't need. headless's much smaller
// surface is the right reference for a non-interactive worker.
//
// Cross-references:
//   * content/public/browser/browser_context.h
//     (the contract — every `= 0` virtual must be overridden)
//   * headless/lib/browser/headless_browser_context_impl.{h,cc}
//     (template — same minimal-server-side shape)
//   * content/shell/browser/shell_browser_context.{h,cc}
//     (template — heavier; for reference only)

#ifndef CAPTURE_BUILD_INTEGRATION_CLOUD_BROWSER_BROWSER_CONTEXT_H_
#define CAPTURE_BUILD_INTEGRATION_CLOUD_BROWSER_BROWSER_CONTEXT_H_

#include <memory>

#include "base/files/file_path.h"
#include "content/public/browser/browser_context.h"

#include "cloud-browser/capture/build-integration/cb_download_manager_delegate.h"

class SimpleFactoryKey;

namespace content {
class BackgroundSyncController;
class BrowsingDataRemoverDelegate;
class ClientHintsControllerDelegate;
class DownloadManagerDelegate;
class PermissionControllerDelegate;
class PlatformNotificationService;
class PushMessagingService;
class ReduceAcceptLanguageControllerDelegate;
class SSLHostStateDelegate;
class StorageNotificationService;
class ZoomLevelDelegate;
}  // namespace content

namespace cloud_browser {

class CloudBrowserBrowserContext final : public content::BrowserContext {
 public:
  CloudBrowserBrowserContext();

  CloudBrowserBrowserContext(const CloudBrowserBrowserContext&) = delete;
  CloudBrowserBrowserContext& operator=(const CloudBrowserBrowserContext&) =
      delete;

  ~CloudBrowserBrowserContext() override;

  // content::BrowserContext (every `= 0` virtual must be overridden):
  std::unique_ptr<content::ZoomLevelDelegate> CreateZoomLevelDelegate(
      const base::FilePath& partition_path) override;
  base::FilePath GetPath() const override;
  bool IsOffTheRecord() override;
  content::DownloadManagerDelegate* GetDownloadManagerDelegate() override;
  content::BrowserPluginGuestManager* GetGuestManager() override;
  ::storage::SpecialStoragePolicy* GetSpecialStoragePolicy() override;
  content::PlatformNotificationService* GetPlatformNotificationService()
      override;
  content::PushMessagingService* GetPushMessagingService() override;
  content::StorageNotificationService* GetStorageNotificationService() override;
  content::SSLHostStateDelegate* GetSSLHostStateDelegate() override;
  content::PermissionControllerDelegate* GetPermissionControllerDelegate()
      override;
  content::ReduceAcceptLanguageControllerDelegate*
  GetReduceAcceptLanguageControllerDelegate() override;
  content::ClientHintsControllerDelegate* GetClientHintsControllerDelegate()
      override;
  content::BackgroundFetchDelegate* GetBackgroundFetchDelegate() override;
  content::BackgroundSyncController* GetBackgroundSyncController() override;
  content::BrowsingDataRemoverDelegate* GetBrowsingDataRemoverDelegate()
      override;

 private:
  // Initialised in the constructor while disk I/O is allowed on the
  // caller thread (BrowserMainParts::PreMainMessageLoopRun is the
  // entry point — the IO restriction is not yet in force there).
  void InitWhileIOAllowed();

  // Profile path. Lives under base::DIR_TEMP; the directory is created
  // in InitWhileIOAllowed and never removed (the worker process exit
  // tears down its temp dir parent on container restart anyway).
  base::FilePath path_;

  // SimpleFactoryKey is the SimpleKeyedService association handle. The
  // BrowserContextDependencyManager and SimpleDependencyManager use it
  // to interlock service shutdown — see ~CloudBrowserBrowserContext for
  // the exact teardown sequence.
  std::unique_ptr<SimpleFactoryKey> simple_factory_key_;

  // Lazily created by GetDownloadManagerDelegate. Owned here so it
  // outlives every DownloadManager query and is destroyed with the
  // context — //content holds the returned pointer raw.
  std::unique_ptr<CbDownloadManagerDelegate> download_manager_delegate_;
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CLOUD_BROWSER_BROWSER_CONTEXT_H_

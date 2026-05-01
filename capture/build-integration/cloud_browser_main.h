// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserMainDelegate — content::ContentMainDelegate plumbing for
// the cloud_browser_worker binary. The only override we need today is
// CreateContentBrowserClient(), which hands back a
// CloudBrowserContentBrowserClient so our encoder factory gets
// installed on libwebrtc.
//
// All other ContentMainDelegate hooks (BasicStartupComplete,
// PreSandboxStartup, RunProcess, …) are intentionally left at the base
// class default — see the headless and content_shell embedders in the
// upstream tree for examples of the hooks we'd grow into when the
// worker needs crash reporting, locale handling, custom feature lists,
// etc.
//
// Cross-references:
//   * content/shell/app/shell_main_delegate.h            (template)
//   * headless/lib/headless_content_main_delegate.h      (template)
//   * capture/build-integration/content_browser_client.h (what we install)

#ifndef CAPTURE_BUILD_INTEGRATION_CLOUD_BROWSER_MAIN_H_
#define CAPTURE_BUILD_INTEGRATION_CLOUD_BROWSER_MAIN_H_

#include <memory>

#include "content/public/app/content_main_delegate.h"

namespace content {
class ContentBrowserClient;
}  // namespace content

namespace cloud_browser {

class CloudBrowserContentBrowserClient;

class CloudBrowserMainDelegate : public content::ContentMainDelegate {
 public:
  CloudBrowserMainDelegate();

  CloudBrowserMainDelegate(const CloudBrowserMainDelegate&) = delete;
  CloudBrowserMainDelegate& operator=(const CloudBrowserMainDelegate&) = delete;

  ~CloudBrowserMainDelegate() override;

  // content::ContentMainDelegate:
  content::ContentBrowserClient* CreateContentBrowserClient() override;

 private:
  // Owned for the lifetime of the delegate; ContentMain retains a raw
  // pointer to the instance returned by CreateContentBrowserClient,
  // which is documented to outlive ContentMainRunner.
  std::unique_ptr<CloudBrowserContentBrowserClient> browser_client_;
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CLOUD_BROWSER_MAIN_H_

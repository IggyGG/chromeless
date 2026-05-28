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
#include <optional>

#include "capture/build-integration/cloud_browser_content_client.h"
#include "content/public/app/content_main_delegate.h"

namespace content {
class ContentClient;
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
  // Registers CloudBrowserContentClient so data-resource lookups from EVERY
  // process (incl. the renderer, where Blink builds the default SVG UA
  // stylesheet) reach the process-local ResourceBundle. Without this the
  // base ContentClient returns empty and Blink DCHECKs in
  // css_default_style_sheets.cc — the CV2 Gate 6 crash. Mirrors
  // headless/lib/headless_content_main_delegate.cc CreateContentClient.
  content::ContentClient* CreateContentClient() override;
  content::ContentBrowserClient* CreateContentBrowserClient() override;

  // CV2-69 (M55-R5-merge-with-m3-r4-r6) — embedder bootstrap inits the
  // worker was previously skipping. See
  // `/tmp/cv2-embedder-init-audit.md` finding F1 (ResourceBundle), F2
  // (Mojo core), F3 (crash keys). Without these the GPU code path
  // SIGABRTs at +97s on `ui/base/resource/resource_bundle.cc:384 Check
  // failed: g_shared_instance_ != nullptr` (the SOFTWARE path bypasses
  // it). Reference impls: `content/shell/app/shell_main_delegate.cc`
  // (PreSandboxStartup @ L289 + PostEarlyInitialization @ L433);
  // `headless/lib/headless_content_main_delegate.cc` (PreSandboxStartup
  // @ L415 + PostEarlyInitialization @ L552).
  void PreSandboxStartup() override;
  std::optional<int> PostEarlyInitialization(InvokedIn invoked_in) override;

 private:
  // Owned for the lifetime of the delegate; ContentMain retains a raw
  // pointer to the instance returned by CreateContentBrowserClient,
  // which is documented to outlive ContentMainRunner.
  std::unique_ptr<CloudBrowserContentBrowserClient> browser_client_;

  // Owned for the lifetime of the delegate. ContentMain stores the raw
  // pointer returned by CreateContentClient() and uses it for the whole
  // run, so a value member here (mirroring HeadlessContentClient
  // content_client_) is the correct lifetime.
  CloudBrowserContentClient content_client_;
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CLOUD_BROWSER_MAIN_H_

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserContentClient — content::ContentClient subclass whose only
// job is to bridge data-resource lookups from EVERY process (browser,
// renderer, gpu, utility) to the process-local ui::ResourceBundle.
//
// Why this exists (CV2 SVG-DCHECK / Gate 6):
//   Blink's renderer builds the default SVG UA stylesheet via
//   data_resource_helper.cc → Platform::Current()->GetDataResourceString(
//   IDR_UASTYLE_SVG_CSS = 46472). That routes through
//   content::ContentClient::GetDataResourceString. The base
//   content::ContentClient impl returns an empty string_view
//   (content/public/common/content_client.cc), so ParseUASheet("") yields
//   0 rules and css_default_style_sheets.cc DCHECKs
//   (UniversalRules().size() == 1u, 0 vs 1) → SIGABRT ~50-90s into
//   rendering the SVG-heavy portal SPA. The worker registered NO
//   ContentClient, so it inherited that empty base. We override the four
//   data-resource accessors to delegate to ResourceBundle, exactly like
//   headless and content_shell.
//
//   NB: this bridge is only as good as the ResourceBundle it reads. The
//   data pak that carries IDR_UASTYLE_SVG_CSS is loaded in
//   cloud_browser_main.cc PreSandboxStartup (which runs in every process);
//   a no-op-populated ResourceBundle would still return empty here.
//
// Cross-references:
//   * headless/lib/headless_content_client.h        (the template)
//   * content/shell/common/shell_content_client.h   (mirror)
//   * capture/build-integration/cloud_browser_main.h (registers this)

#ifndef CAPTURE_BUILD_INTEGRATION_CLOUD_BROWSER_CONTENT_CLIENT_H_
#define CAPTURE_BUILD_INTEGRATION_CLOUD_BROWSER_CONTENT_CLIENT_H_

#include <string>
#include <string_view>

#include "content/public/common/content_client.h"
#include "ui/base/resource/resource_scale_factor.h"

namespace base {
class RefCountedMemory;
}  // namespace base

namespace gfx {
class Image;
}  // namespace gfx

namespace cloud_browser {

class CloudBrowserContentClient : public content::ContentClient {
 public:
  CloudBrowserContentClient();

  CloudBrowserContentClient(const CloudBrowserContentClient&) = delete;
  CloudBrowserContentClient& operator=(const CloudBrowserContentClient&) =
      delete;

  ~CloudBrowserContentClient() override;

  // content::ContentClient:
  std::string_view GetDataResource(
      int resource_id,
      ui::ResourceScaleFactor scale_factor) override;
  base::RefCountedMemory* GetDataResourceBytes(int resource_id) override;
  std::string GetDataResourceString(int resource_id) override;
  gfx::Image& GetNativeImageNamed(int resource_id) override;
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CLOUD_BROWSER_CONTENT_CLIENT_H_

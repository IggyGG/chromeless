// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbPermissionManager — geolocation, notifications, camera, clipboard-read:
// the prompts a real browser shows and this one silently refused.
//
// WHAT IT REPLACES
// ----------------
// Nothing. `BrowserContext::GetPermissionControllerDelegate()` returned
// nullptr, and //content's fallback DENIES every permission without asking.
// So a page calling navigator.geolocation.getCurrentPosition() got an
// immediate PERMISSION_DENIED, Notification.requestPermission() resolved
// "denied", and the user was never shown anything. Indistinguishable, from
// the page's side, from a user who said no — which is exactly why it went
// unnoticed.
//
// THE ONE INVARIANT
// -----------------
// Every request callback MUST run, exactly once, with one PermissionResult
// per requested descriptor. //content's PermissionController waits on it and
// the page's promise never settles otherwise. Same discipline as
// CbControlChannel's dialogs (see its header), and for the same reason: a
// callback that can be dropped is a page that hangs forever.
//
// So DENY is the default on every path that is not an explicit yes — no
// control channel, a closed one, a timeout, a malformed answer, a viewer who
// declines. Asking is an improvement on refusing silently; defaulting to
// "allow" would be a regression in the other direction, and this browser can
// be pointed at anything.
//
// DESCRIPTORS, NOT THE ENUM
// -------------------------
// At 7727 every accessor is keyed on `blink::mojom::PermissionDescriptorPtr`.
// Only ResetPermission still takes `blink::PermissionType`. The roadmap's
// policy table was written against the enum and had to be rewritten; see
// docs/build/chromium-7727-api-pins.md, which calls this the highest-drift
// surface in the workstream. Eight of the thirteen virtuals are pure, so
// none of them can be skipped.

#ifndef CAPTURE_BUILD_INTEGRATION_CB_PERMISSION_MANAGER_H_
#define CAPTURE_BUILD_INTEGRATION_CB_PERMISSION_MANAGER_H_

#include <vector>

#include "base/functional/callback.h"
#include "content/public/browser/permission_controller_delegate.h"
#include "content/public/browser/permission_request_description.h"
#include "content/public/browser/permission_result.h"
#include "third_party/blink/public/common/permissions/permission_utils.h"
#include "third_party/blink/public/mojom/permissions/permission.mojom.h"
#include "third_party/blink/public/mojom/permissions/permission_status.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace cloud_browser {

class CbPermissionManager : public content::PermissionControllerDelegate {
 public:
  CbPermissionManager();
  ~CbPermissionManager() override;

  CbPermissionManager(const CbPermissionManager&) = delete;
  CbPermissionManager& operator=(const CbPermissionManager&) = delete;

  // content::PermissionControllerDelegate:
  void RequestPermissions(
      content::RenderFrameHost* render_frame_host,
      const content::PermissionRequestDescription& request_description,
      base::OnceCallback<void(const std::vector<content::PermissionResult>&)>
          callback) override;
  void RequestPermissionsFromCurrentDocument(
      content::RenderFrameHost* render_frame_host,
      const content::PermissionRequestDescription& request_description,
      base::OnceCallback<void(const std::vector<content::PermissionResult>&)>
          callback) override;
  blink::mojom::PermissionStatus GetPermissionStatus(
      const blink::mojom::PermissionDescriptorPtr& permission,
      const GURL& requesting_origin,
      const GURL& embedding_origin) override;
  content::PermissionResult GetPermissionResultForOriginWithoutContext(
      const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
      const url::Origin& requesting_origin,
      const url::Origin& embedding_origin) override;
  content::PermissionResult GetPermissionResultForCurrentDocument(
      const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
      content::RenderFrameHost* render_frame_host,
      bool should_include_device_status) override;
  content::PermissionResult GetPermissionResultForWorker(
      const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
      content::RenderProcessHost* render_process_host,
      const GURL& worker_origin) override;
  content::PermissionResult GetPermissionResultForEmbeddedRequester(
      const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
      content::RenderFrameHost* render_frame_host,
      const url::Origin& requesting_origin) override;
  void ResetPermission(blink::PermissionType permission,
                       const GURL& requesting_origin,
                       const GURL& embedding_origin) override;

 private:
  // Ask the viewer about |descriptors| for |origin| and answer |callback|
  // with one result each. Shared by both Request* entry points, which differ
  // only in where the origin comes from.
  void AskViewer(
      const GURL& origin,
      const std::vector<blink::mojom::PermissionDescriptorPtr>& descriptors,
      base::OnceCallback<void(const std::vector<content::PermissionResult>&)>
          callback);
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CB_PERMISSION_MANAGER_H_

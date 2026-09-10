// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "capture/build-integration/cb_permission_manager.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "base/values.h"
#include "capture/build-integration/cb_control_channel.h"
#include "capture/build-integration/cb_web_contents_delegate.h"
#include "content/public/browser/render_frame_host.h"

namespace cloud_browser {
namespace {

constexpr char kLog[] = "CV2-PERMISSION: ";

// How long a permission prompt waits for the viewer. Shorter than the file
// chooser's five minutes: a permission prompt appears mid-interaction and a
// page waiting on one is usually blocked, so a stale prompt is worse than a
// denial. Long enough that a person can actually read it.
constexpr base::TimeDelta kPermissionDeadline = base::Seconds(90);

// DENIED, one per descriptor. The invariant is one result per request, so
// every early return builds the full vector rather than an empty one — a
// short vector is a page whose promise never settles.
std::vector<content::PermissionResult> AllDenied(size_t n) {
  return std::vector<content::PermissionResult>(
      n, content::PermissionResult(blink::mojom::PermissionStatus::DENIED));
}

}  // namespace

CbPermissionManager::CbPermissionManager() {
  LOG(INFO) << kLog
            << "permission manager bound — geolocation, notifications and "
               "friends now ASK the viewer instead of being denied silently";
}

CbPermissionManager::~CbPermissionManager() = default;

void CbPermissionManager::RequestPermissions(
    content::RenderFrameHost* render_frame_host,
    const content::PermissionRequestDescription& request_description,
    base::OnceCallback<void(const std::vector<content::PermissionResult>&)>
        callback) {
  // requesting_origin is only populated on some construction paths, so fall
  // back to the frame's own origin. Getting this wrong shows the viewer the
  // wrong site in the prompt, which is a security-relevant lie.
  GURL origin = request_description.requesting_origin;
  if (origin.is_empty() && render_frame_host) {
    origin = render_frame_host->GetLastCommittedOrigin().GetURL();
  }
  AskViewer(origin, request_description.permissions, std::move(callback));
}

void CbPermissionManager::RequestPermissionsFromCurrentDocument(
    content::RenderFrameHost* render_frame_host,
    const content::PermissionRequestDescription& request_description,
    base::OnceCallback<void(const std::vector<content::PermissionResult>&)>
        callback) {
  // "FromCurrentDocument" — the document IS the origin, so it wins over
  // anything carried in the description.
  const GURL origin =
      render_frame_host ? render_frame_host->GetLastCommittedOrigin().GetURL()
                        : request_description.requesting_origin;
  AskViewer(origin, request_description.permissions, std::move(callback));
}

void CbPermissionManager::AskViewer(
    const GURL& origin,
    const std::vector<blink::mojom::PermissionDescriptorPtr>& descriptors,
    base::OnceCallback<void(const std::vector<content::PermissionResult>&)>
        callback) {
  const size_t count = descriptors.size();
  if (count == 0) {
    std::move(callback).Run({});
    return;
  }

  CbControlChannel* channel =
      GetCloudBrowserWebContentsDelegate()->control_channel();
  if (!channel) {
    LOG(INFO) << kLog << "no control channel; denying " << count
              << " permission(s) for " << origin.possibly_invalid_spec();
    std::move(callback).Run(AllDenied(count));
    return;
  }

  base::ListValue names;
  for (const auto& descriptor : descriptors) {
    // The readable name the viewer shows. PermissionDescriptorToPermissionType
    // + GetPermissionString is the pair blink provides; a descriptor with no
    // mapping would otherwise reach the UI as a bare integer.
    names.Append(blink::GetPermissionString(
        blink::PermissionDescriptorToPermissionType(descriptor)));
  }

  base::DictValue payload;
  payload.Set("origin", origin.possibly_invalid_spec());
  payload.Set("permissions", std::move(names));

  channel->SendRequest(
      "permission", std::move(payload), kPermissionDeadline,
      base::BindOnce(
          [](base::OnceCallback<void(
                 const std::vector<content::PermissionResult>&)> cb,
             size_t count, base::DictValue response) {
            // An empty dict is the channel's "no answer" — closed, timed
            // out, or torn down. Anything but an explicit true denies.
            const std::optional<bool> granted = response.FindBool("granted");
            const blink::mojom::PermissionStatus status =
                granted.value_or(false)
                    ? blink::mojom::PermissionStatus::GRANTED
                    : blink::mojom::PermissionStatus::DENIED;
            // One result per descriptor, always. The viewer answers the
            // group as a whole: a per-permission UI would be a better
            // prompt, and it is not worth a protocol that can return the
            // wrong NUMBER of answers.
            std::move(cb).Run(std::vector<content::PermissionResult>(
                count, content::PermissionResult(status)));
          },
          std::move(callback), count));
}

// ---------------------------------------------------------------------------
// Status queries.
//
// These are synchronous — a page asking "may I?" without prompting, e.g.
// navigator.permissions.query(). There is nobody to ask synchronously, so
// they report ASK: the honest answer is "you would have to request it", and
// reporting DENIED here would make a page skip the request that WOULD have
// prompted the viewer.
// ---------------------------------------------------------------------------

blink::mojom::PermissionStatus CbPermissionManager::GetPermissionStatus(
    const blink::mojom::PermissionDescriptorPtr& /*permission*/,
    const GURL& /*requesting_origin*/,
    const GURL& /*embedding_origin*/) {
  return blink::mojom::PermissionStatus::ASK;
}

content::PermissionResult
CbPermissionManager::GetPermissionResultForOriginWithoutContext(
    const blink::mojom::PermissionDescriptorPtr& /*permission_descriptor*/,
    const url::Origin& /*requesting_origin*/,
    const url::Origin& /*embedding_origin*/) {
  return content::PermissionResult(blink::mojom::PermissionStatus::ASK);
}

content::PermissionResult
CbPermissionManager::GetPermissionResultForCurrentDocument(
    const blink::mojom::PermissionDescriptorPtr& /*permission_descriptor*/,
    content::RenderFrameHost* /*render_frame_host*/,
    bool /*should_include_device_status*/) {
  return content::PermissionResult(blink::mojom::PermissionStatus::ASK);
}

content::PermissionResult CbPermissionManager::GetPermissionResultForWorker(
    const blink::mojom::PermissionDescriptorPtr& /*permission_descriptor*/,
    content::RenderProcessHost* /*render_process_host*/,
    const GURL& /*worker_origin*/) {
  // A worker has no viewer-visible frame to prompt over, so this one is a
  // real DENY rather than ASK: there is no path by which it could become a
  // yes, and ASK would invite a request nobody can answer.
  return content::PermissionResult(blink::mojom::PermissionStatus::DENIED);
}

content::PermissionResult
CbPermissionManager::GetPermissionResultForEmbeddedRequester(
    const blink::mojom::PermissionDescriptorPtr& /*permission_descriptor*/,
    content::RenderFrameHost* /*render_frame_host*/,
    const url::Origin& /*requesting_origin*/) {
  return content::PermissionResult(blink::mojom::PermissionStatus::ASK);
}

void CbPermissionManager::ResetPermission(
    blink::PermissionType /*permission*/,
    const GURL& /*requesting_origin*/,
    const GURL& /*embedding_origin*/) {
  // Nothing is persisted, so there is nothing to reset. Note this is the ONE
  // virtual still keyed on the PermissionType enum rather than a descriptor
  // (7727) — the asymmetry is real, not a mistake to "fix".
}

}  // namespace cloud_browser

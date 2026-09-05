// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbWebContentsDelegate — the parts of a browser that aren't pixels.
//
// WHY THIS EXISTS
//
// Until this class, `capture/` contained NO content::WebContentsDelegate
// at all. A //content embedder without one does not degrade gracefully —
// chromium's defaults silently drop the request:
//
//   window.open() / target=_blank  -> new WebContents created and dropped
//   alert/confirm/prompt           -> auto-dismissed
//   <input type=file> click        -> nothing happens
//   right-click                    -> no menu
//   requestFullscreen()            -> never takes effect
//   downloads                      -> silently discarded
//
// Every one of those is a page that "just doesn't work" with no error
// anywhere. That is the single largest gap between this and a browser a
// person can actually use, and it is concentrated here.
//
// OWNERSHIP: process-lifetime singleton
//
// content::WebContentsImpl holds its delegate as a RAW back-pointer and
// never owns it, so the delegate must outlive every WebContents. In this
// embedder that is a hard constraint rather than a style preference:
// CbDevToolsManagerDelegate::web_contents_holders_ is owned by content's
// DevToolsManager singleton, which AtExitManager destroys AFTER
// main_parts. A delegate owned by main_parts would therefore be freed
// while live WebContents still point at it — the same use-after-free class
// that is why aura_ is deliberately leaked (see PostMainMessageLoopRun).
//
// So: base::NoDestructor singleton, reached via
// GetCloudBrowserWebContentsDelegate(). Per-SESSION state (the control
// channel) is injected with SetSessionContext() and cleared on teardown,
// so the long-lived object never holds a dangling pointer into a
// torn-down session.
//
// Adopted popups live in a vector inside the singleton. They are destroyed
// only when the PAGE closes them (CloseContents — window.close(), or the
// opener closing its popup), never on teardown: destroying them at exit
// would race the DevToolsManager singleton that also holds them, the same
// use-after-free class above. A popup the page never closes therefore costs
// one WebContents for the life of a guest, which is measured in minutes.

#ifndef CAPTURE_BUILD_INTEGRATION_CB_WEB_CONTENTS_DELEGATE_H_
#define CAPTURE_BUILD_INTEGRATION_CB_WEB_CONTENTS_DELEGATE_H_

#include <memory>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"

namespace aura {
class Window;
}

namespace cloud_browser {

class CbControlChannel;
class CbJavaScriptDialogManager;

class CbWebContentsDelegate : public content::WebContentsDelegate {
 public:
  CbWebContentsDelegate();
  ~CbWebContentsDelegate() override;

  CbWebContentsDelegate(const CbWebContentsDelegate&) = delete;
  CbWebContentsDelegate& operator=(const CbWebContentsDelegate&) = delete;

  // Per-session wiring. |aura_context| is the root window new WebContents
  // must be parented to (BUGS-529: without it the view falls outside the
  // focus chain and renderer-side input is silently dropped). Pass
  // nullptrs on teardown.
  void SetSessionContext(aura::Window* aura_context,
                         CbControlChannel* control_channel);

  CbJavaScriptDialogManager* dialog_manager() { return dialog_manager_.get(); }

  // content::WebContentsDelegate:

  // Popups. Adopts |new_contents| as a real tab rather than navigating the
  // opener — see the .cc for why same-tab navigation breaks OAuth flows.
  content::WebContents* AddNewContents(
      content::WebContents* source,
      std::unique_ptr<content::WebContents> new_contents,
      const GURL& target_url,
      WindowOpenDisposition disposition,
      const blink::mojom::WindowFeatures& window_features,
      bool user_gesture,
      bool* was_blocked) override;

  // window.close(), or an opener closing its popup. Without this override
  // content's default is a no-op: the tab stays open, invisible, forever.
  // Adopted tabs are destroyed here; the initial tab is never ours to
  // destroy (main_parts owns it), so a page closing it is reported and
  // ignored, as Chrome does for the last tab of a window.
  void CloseContents(content::WebContents* source) override;

  // Renderer-initiated navigations that content will otherwise drop.
  content::WebContents* OpenURLFromTab(
      content::WebContents* source,
      const content::OpenURLParams& params,
      base::OnceCallback<void(content::NavigationHandle&)>
          navigation_handle_callback) override;

  content::JavaScriptDialogManager* GetJavaScriptDialogManager(
      content::WebContents* source) override;

  // Fullscreen. Pure bookkeeping — there is no OS window to maximise, the
  // aura root is already a fixed offscreen host, and the captured rect is
  // the widget either way. Tracking the flag is nonetheless what makes
  // requestFullscreen() RESOLVE instead of reject, which is what video
  // players and canvas games gate on.
  //
  // CanEnterFullscreenModeForTab must return true or chromium never calls
  // EnterFullscreenModeForTab at all (web_contents_delegate.h:525-529).
  bool CanEnterFullscreenModeForTab(
      content::RenderFrameHost* requesting_frame) override;
  void EnterFullscreenModeForTab(
      content::RenderFrameHost* requesting_frame,
      const blink::mojom::FullscreenOptions& options) override;
  void ExitFullscreenModeForTab(content::WebContents* web_contents) override;
  bool IsFullscreenForTabOrPending(
      const content::WebContents* web_contents) override;

  // Allow downloads to reach the DownloadManager. The byte path to the
  // user lands with the download relay; permitting them here first makes
  // the attempt observable instead of invisible.
  void CanDownload(const GURL& url,
                   const std::string& request_method,
                   base::OnceCallback<void(bool)> callback) override;

  // getUserMedia. Denied for now — camera/mic passthrough is a separate
  // designed feature, and granting without the v4l2 writer hands the page
  // a black/silent device, which is worse than a clean denial.
  void RequestMediaAccessPermission(
      content::WebContents* web_contents,
      const content::MediaStreamRequest& request,
      content::MediaResponseCallback callback) override;
  bool CheckMediaAccessPermission(content::RenderFrameHost* render_frame_host,
                                  const url::Origin& security_origin,
                                  blink::mojom::MediaStreamType type) override;

  // Context menu. Returning true SUPPRESSES chromium's default handling
  // (web_contents_impl.cc:8882 consults this before falling through to the
  // view delegate at :8886). A portal-rendered menu replaces this later;
  // for now suppressing explicitly is the honest no-op.
  bool HandleContextMenu(content::RenderFrameHost& render_frame_host,
                         const content::ContextMenuParams& params) override;

 private:
  // Wire a freshly-created WebContents into this embedder: parent it to
  // the aura root, show + focus it, publish a DevToolsAgentHost so the
  // orchestrator can see and drive it, and adopt ownership.
  content::WebContents* AdoptWebContents(
      std::unique_ptr<content::WebContents> contents);

  std::unique_ptr<CbJavaScriptDialogManager> dialog_manager_;

  raw_ptr<aura::Window> aura_context_ = nullptr;
  raw_ptr<CbControlChannel> control_channel_ = nullptr;

  // Non-null while a tab believes it is fullscreen.
  raw_ptr<content::WebContents> fullscreen_contents_ = nullptr;

  // Adopted popups. Destroyed only via CloseContents — see the ownership
  // note above.
  std::vector<std::unique_ptr<content::WebContents>> adopted_;

  // Tell the viewer a tab came or went, so a tab strip can update without
  // polling /json. Advisory: `/json` stays the source of truth.
  void SendTabEvent(const char* kind, content::WebContents* wc);
};

// Process-lifetime accessor. Never returns null.
CbWebContentsDelegate* GetCloudBrowserWebContentsDelegate();

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CB_WEB_CONTENTS_DELEGATE_H_

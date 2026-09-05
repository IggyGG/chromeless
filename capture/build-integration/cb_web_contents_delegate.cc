// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "capture/build-integration/cb_web_contents_delegate.h"

#include <tuple>
#include <utility>

#include "base/logging.h"
#include "base/values.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "capture/build-integration/cb_control_channel.h"
#include "capture/build-integration/cb_javascript_dialog_manager.h"
#include "content/public/browser/devtools_agent_host.h"
// MediaResponseCallback + the blink mediastream types it names
// (StreamDevicesSet, MediaStreamRequestResult) — this header pulls in the
// mojom for both (media_stream_request.h:14-15).
#include "content/public/browser/media_stream_request.h"
// media_stream_request.h pulls only media_stream.mojom-SHARED.h, which
// forward-declares StreamDevicesSet without defining it — enough to name
// the type in the callback signature, not enough to CONSTRUCT one. The
// full mojom header is what every in-tree caller that builds a
// StreamDevicesSet includes (e.g. content/browser/media/
// captured_surface_controller.cc:22).
#include "third_party/blink/public/mojom/mediastream/media_stream.mojom.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "ui/aura/window.h"
#include "url/gurl.h"

namespace cloud_browser {

CbWebContentsDelegate::CbWebContentsDelegate()
    : dialog_manager_(std::make_unique<CbJavaScriptDialogManager>()) {}

CbWebContentsDelegate::~CbWebContentsDelegate() = default;

void CbWebContentsDelegate::SetSessionContext(
    aura::Window* aura_context,
    CbControlChannel* control_channel) {
  aura_context_ = aura_context;
  control_channel_ = control_channel;
  if (dialog_manager_) {
    dialog_manager_->SetControlChannel(control_channel);
  }
}

content::WebContents* CbWebContentsDelegate::AdoptWebContents(
    std::unique_ptr<content::WebContents> contents) {
  if (!contents) {
    return nullptr;
  }
  content::WebContents* raw = contents.get();

  // This delegate handles the adopted tab too, so a popup that itself
  // opens a popup keeps working.
  raw->SetDelegate(this);

  // Same visibility + focus chain the boot WebContents gets. Without the
  // native view Show(), aura hit-testing finds no handler and cursor
  // routing stops before CbCursorClient::SetCursor; without WasShown() +
  // Focus() the renderer binds its WidgetInputHandler in "no focused page"
  // state and drops injected input on the floor (BUGS-529).
  if (aura::Window* native_view = raw->GetNativeView()) {
    native_view->Show();
  }
  raw->WasShown();
  raw->Focus();

  // Publish a DevToolsAgentHost so the tab appears in /json and fires
  // Target.targetCreated. That is how the orchestrator learns a new tab
  // exists and allocates a tab id for it — without this the popup would
  // render into the void, invisible to both the operator and the user.
  std::ignore = content::DevToolsAgentHost::GetOrCreateFor(raw);

  // Deliberately NOT stealing the FrameSink capture. Which tab is streamed
  // is the orchestrator's decision (it drives Target.activateTarget +
  // Cb.startFrameSinkCapture); a guest that unilaterally retargeted
  // capture on every window.open() would yank the user's view to a popup
  // they may not have wanted.
  adopted_.push_back(std::move(contents));

  if (auto* rwhv = raw->GetRenderWidgetHostView()) {
    LOG(INFO) << "CbWebContentsDelegate: adopted WebContents bounds="
              << rwhv->GetViewBounds().ToString()
              << " total_adopted=" << adopted_.size();
  }
  // Tell the viewer. Until this event existed a popup was visible only to
  // something polling /json; the standalone client polls nothing, so an
  // OAuth window opened and the user saw the opener sit there "doing
  // nothing". The gateway's tab list is still authoritative — this is the
  // nudge to go read it.
  SendTabEvent("tab_opened", raw);
  return raw;
}

void CbWebContentsDelegate::SendTabEvent(const char* kind,
                                         content::WebContents* wc) {
  if (!control_channel_ || !wc) {
    return;
  }
  base::DictValue payload;
  // The DevTools target id is what /json lists and what the gateway's tab
  // verbs take, so the viewer can correlate without a second lookup.
  // GetOrCreateFor is idempotent: AdoptWebContents already created it.
  payload.Set("targetId",
              content::DevToolsAgentHost::GetOrCreateFor(wc)->GetId());
  payload.Set("url", wc->GetLastCommittedURL().possibly_invalid_spec());
  payload.Set("title", base::UTF16ToUTF8(wc->GetTitle()));
  control_channel_->SendEvent(kind, std::move(payload));
}

void CbWebContentsDelegate::CloseContents(content::WebContents* source) {
  // window.close() on a popup, or the opener closing it. content's default
  // is a NO-OP, so before this override a popup that closed itself stayed
  // alive and invisible — and the commonest real popup, an OAuth window,
  // closes itself as its last act.
  //
  // Notify BEFORE destroying: the payload reads the WebContents.
  SendTabEvent("tab_closed", source);

  for (auto it = adopted_.begin(); it != adopted_.end(); ++it) {
    if (it->get() == source) {
      // The captured tab may be this one. The resolver observes it and
      // clears itself in WebContentsDestroyed; main_parts then re-arms
      // capture onto the initial tab (CV2-CAPTURE-FALLBACK) so the stream
      // does not go dark and the 30 s no-frames watchdog does not fire.
      //
      // Erase via a local so the WebContents is destroyed AFTER the vector
      // is consistent — its destructor can re-enter this delegate
      // (WebContentsDestroyed observers, dialogs cancelling).
      std::unique_ptr<content::WebContents> doomed = std::move(*it);
      adopted_.erase(it);
      LOG(INFO) << "CbWebContentsDelegate: CloseContents destroying adopted "
                   "tab, total_adopted=" << adopted_.size();
      doomed.reset();
      return;
    }
  }
  // Not ours: the initial tab, which main_parts owns for the life of the
  // process. Chrome keeps the last tab of a window open too; closing the
  // only surface a viewer has would strand the session. Logged so a page
  // that tries is visible in the guest log.
  LOG(INFO) << "CbWebContentsDelegate: CloseContents on the initial tab "
               "ignored (a session needs one tab)";
}

content::WebContents* CbWebContentsDelegate::AddNewContents(
    content::WebContents* /*source*/,
    std::unique_ptr<content::WebContents> new_contents,
    const GURL& target_url,
    WindowOpenDisposition /*disposition*/,
    const blink::mojom::WindowFeatures& /*window_features*/,
    bool /*user_gesture*/,
    bool* was_blocked) {
  // Adopt as a real tab rather than navigating the opener in place.
  //
  // Same-tab navigation is the tempting shortcut (one visible surface, no
  // tab bookkeeping) and it is wrong: it destroys the opener's page state,
  // which breaks the single most common real-world popup — an OAuth /
  // SSO window that postMessage()s a token back to the opener that
  // launched it. Discarding the opener discards the handler waiting for
  // that message, and the sign-in silently never completes.
  if (was_blocked) {
    *was_blocked = false;
  }
  LOG(INFO) << "CbWebContentsDelegate: AddNewContents url="
            << target_url.possibly_invalid_spec();
  return AdoptWebContents(std::move(new_contents));
}

content::WebContents* CbWebContentsDelegate::OpenURLFromTab(
    content::WebContents* source,
    const content::OpenURLParams& params,
    base::OnceCallback<void(content::NavigationHandle&)>
        navigation_handle_callback) {
  // With no delegate, content DROPS renderer-initiated navigations that
  // want a disposition other than "current tab" — target=_blank links,
  // window.open with features, ctrl-click. They simply do nothing.
  if (!source) {
    return nullptr;
  }

  content::NavigationController::LoadURLParams load_params(params);
  base::WeakPtr<content::NavigationHandle> handle =
      source->GetController().LoadURLWithParams(load_params);
  if (handle && navigation_handle_callback) {
    std::move(navigation_handle_callback).Run(*handle);
  }
  return source;
}

content::JavaScriptDialogManager*
CbWebContentsDelegate::GetJavaScriptDialogManager(
    content::WebContents* /*source*/) {
  return dialog_manager_.get();
}

bool CbWebContentsDelegate::CanEnterFullscreenModeForTab(
    content::RenderFrameHost* /*requesting_frame*/) {
  // Must be true or EnterFullscreenModeForTab is never called and
  // requestFullscreen() rejects.
  return true;
}

void CbWebContentsDelegate::EnterFullscreenModeForTab(
    content::RenderFrameHost* requesting_frame,
    const blink::mojom::FullscreenOptions& /*options*/) {
  // There is no OS window to maximise: the aura root is a fixed offscreen
  // host and the captured rect is the widget regardless. Tracking the flag
  // is what makes the Fullscreen API resolve and :fullscreen CSS apply, so
  // players and games behave; the pixels the user sees are unchanged.
  if (requesting_frame) {
    fullscreen_contents_ =
        content::WebContents::FromRenderFrameHost(requesting_frame);
  }
  // Tell the viewer so it can hide its own browser chrome and give the
  // stream the whole panel.
  if (control_channel_) {
    base::DictValue payload;
    payload.Set("fullscreen", true);
    control_channel_->SendEvent("fullscreen_changed", std::move(payload));
  }
}

void CbWebContentsDelegate::ExitFullscreenModeForTab(
    content::WebContents* web_contents) {
  if (fullscreen_contents_ == web_contents) {
    fullscreen_contents_ = nullptr;
  }
  if (control_channel_) {
    base::DictValue payload;
    payload.Set("fullscreen", false);
    control_channel_->SendEvent("fullscreen_changed", std::move(payload));
  }
}

bool CbWebContentsDelegate::IsFullscreenForTabOrPending(
    const content::WebContents* web_contents) {
  return fullscreen_contents_ == web_contents;
}

void CbWebContentsDelegate::CanDownload(
    const GURL& url,
    const std::string& /*request_method*/,
    base::OnceCallback<void(bool)> callback) {
  // Allow. The DownloadManagerDelegate is still null, so the bytes do not
  // yet reach the user — but permitting the download makes the attempt
  // observable (it reaches the manager and logs) instead of vanishing at
  // this gate with no trace, which is what made "downloads silently do
  // nothing" so hard to diagnose.
  LOG(INFO) << "CbWebContentsDelegate: download permitted url="
            << url.possibly_invalid_spec()
            << " (byte path to the viewer not yet wired)";
  std::move(callback).Run(true);
}

void CbWebContentsDelegate::RequestMediaAccessPermission(
    content::WebContents* /*web_contents*/,
    const content::MediaStreamRequest& /*request*/,
    content::MediaResponseCallback callback) {
  // Deny with an empty device list. Camera/mic passthrough is a separate
  // designed feature (docs/protocols/webcam-mic-passthrough.md); granting
  // before the v4l2 writer exists would hand the page a black/silent
  // device, which pages handle far worse than an honest denial.
  //
  // NOTE: infra/launch-chromeless.sh passes --use-fake-ui-for-media-stream,
  // which auto-grants at the ContentBrowserClient layer and may pre-empt
  // this callback entirely. If getUserMedia appears to succeed despite
  // this denial, that flag is why — check it before concluding this
  // method is broken.
  // Note the callback takes a StreamDevicesSet (plural), not StreamDevices
  // — media_stream_request.h:199. An empty set is the correct "no devices"
  // payload for a denial.
  std::move(callback).Run(
      blink::mojom::StreamDevicesSet(),
      blink::mojom::MediaStreamRequestResult::PERMISSION_DENIED,
      /*ui=*/nullptr);
}

bool CbWebContentsDelegate::CheckMediaAccessPermission(
    content::RenderFrameHost* /*render_frame_host*/,
    const url::Origin& /*security_origin*/,
    blink::mojom::MediaStreamType /*type*/) {
  return false;
}

bool CbWebContentsDelegate::HandleContextMenu(
    content::RenderFrameHost& /*render_frame_host*/,
    const content::ContextMenuParams& /*params*/) {
  // Return true = "handled", which suppresses chromium's default path
  // (web_contents_impl.cc:8882 checks this before falling through to the
  // view delegate at :8886).
  //
  // Suppressing is the honest state today: there is no native menu surface
  // in a headless embedder, so the default would render nothing anyway.
  // The difference is that this is now a deliberate, greppable decision
  // with a hook to replace, rather than an unhandled event. Forwarding
  // ContextMenuParams to the viewer for a portal-rendered menu plugs in
  // exactly here.
  return true;
}

CbWebContentsDelegate* GetCloudBrowserWebContentsDelegate() {
  static base::NoDestructor<CbWebContentsDelegate> instance;
  return instance.get();
}

}  // namespace cloud_browser

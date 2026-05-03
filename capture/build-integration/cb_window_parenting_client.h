// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CbWindowParentingClient — minimal aura::client::WindowParentingClient
// for the cb-chromium worker. Returns the embedder's root window as the
// default parent for every aura::Window that gets created inside the
// browser process.
//
// Why we need it (BUGS-529 follow-up):
//   When content::WebContents::Create() is called on Aura,
//   WebContentsViewAura::CreateAuraWindow() (content/browser/web_contents/
//   web_contents_view_aura.cc:992 in the pinned 7727 tree) invokes
//   aura::client::ParentWindowWithContext(window, context->GetRootWindow(),
//                                         …)
//   if the |context| field on the CreateParams is non-null. That call walks
//   the parenting client registered on the context's root window and
//   parents the new WebContents view under whatever GetDefaultParent
//   returns. If no parenting client is registered (or no |context| is
//   passed), the WebContents view is never parented to anything — it
//   floats outside the focus chain Aura tracks, so WebContents::Focus()
//   becomes a silent no-op and the renderer-side WidgetInputHandler
//   binds in "no focused page" state.
//
//   The 9703db5 patch added WebContents::WasShown() + WebContents::Focus()
//   on every WebContents the embedder creates, but Focus() has no effect
//   because the Aura layer underneath has no parenting client. CDP
//   Input.dispatch{Mouse,Key}Event then ack at the protocol layer but
//   never reach the renderer's input pipeline. That was the BUGS-529
//   second-layer issue.
//
// Cross-references:
//   * headless/lib/browser/headless_window_parenting_client.{h,cc}
//     (the canonical minimal pattern; we mirror it nearly verbatim)
//   * ui/aura/test/test_window_parenting_client.{h,cc}
//     (the testonly equivalent content_shell uses; we cannot depend on
//     ui/aura:test_support from a non-test executable, so we roll our
//     own embedder copy)
//   * ui/aura/client/window_parenting_client.h (the interface contract)

#ifndef CAPTURE_BUILD_INTEGRATION_CB_WINDOW_PARENTING_CLIENT_H_
#define CAPTURE_BUILD_INTEGRATION_CB_WINDOW_PARENTING_CLIENT_H_

#include "base/memory/raw_ptr.h"
#include "ui/aura/client/window_parenting_client.h"

namespace cloud_browser {

class CbWindowParentingClient : public aura::client::WindowParentingClient {
 public:
  // Registers itself as the parenting client on |root_window| at
  // construction; deregisters in the dtor. |root_window| must outlive
  // this client — main_parts owns both the root window (via the
  // WindowTreeHost) and this client, so the destruction order is
  // enforced by main_parts's member declaration order (root host first,
  // client after).
  explicit CbWindowParentingClient(aura::Window* root_window);

  CbWindowParentingClient(const CbWindowParentingClient&) = delete;
  CbWindowParentingClient& operator=(const CbWindowParentingClient&) = delete;

  ~CbWindowParentingClient() override;

  // aura::client::WindowParentingClient:
  aura::Window* GetDefaultParent(aura::Window* window,
                                 const gfx::Rect& bounds,
                                 const int64_t display_id) override;

 private:
  raw_ptr<aura::Window> root_window_;  // Not owned.
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CB_WINDOW_PARENTING_CLIENT_H_

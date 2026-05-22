// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CbAuraPlatformData — owns the Aura subsystem the cb-chromium worker
// needs so that WebContents created by main_parts and CbDevToolsManager
// Delegate get parented into a real focus chain.
//
// Construction sequence (all done in PreMainMessageLoopRun, after
// chromium's BrowserMainLoop has already created aura::Env, initialised
// ozone, and registered display::Screen — see browser_main_loop.cc:1517
// for the Env::CreateInstance call):
//
//   1. aura::WindowTreeHost::Create() with the configured viewport size.
//      Ozone-X11 backed; cb-chromium runs under Xvfb on :99 so this
//      creates a real X11 window owned by the host. We don't display
//      this window — it exists purely to give the embedder a root for
//      Aura's window/focus tree.
//   2. host->InitHost() + host->window()->Show().
//   3. FillLayout layout manager — children of the root window resize
//      to fill the root's bounds. WebContentsViewAura instances added
//      as children inherit the root's geometry without each call site
//      having to set bounds explicitly.
//   4. CbFocusClient on host->window() — makes WebContents::Focus()
//      actually move focus instead of being a no-op.
//   5. wm::DefaultActivationClient on host->window() — activation +
//      modal-window support; required by some WebContents code paths.
//   6. aura::client::DefaultCaptureClient on host->window() — pointer
//      capture; required by drag handling and certain mouse paths.
//   7. CbWindowParentingClient on host->window() — every aura::Window
//      created without an explicit parent (including the windows
//      WebContentsViewAura makes for each WebContents) gets parented
//      under host->window().
//   8. CbCursorClient on host->window() — observes every renderer-
//      driven cursor change (CV2-19 / M5 R1). Foundation for the
//      native cursor egress path that replaces the JS+CDP polling
//      sidecar in capture/cursor-watcher/. Without it,
//      GetCursorClient(window) returns nullptr and SetCursor() is a
//      silent no-op — the browser process never learns about CSS
//      cursor changes under the pointer.
//
// Lifetime is tied to CloudBrowserBrowserMainParts: constructed in
// PreMainMessageLoopRun, destroyed in PostMainMessageLoopRun. WebContents
// holders MUST be destroyed before this object — the WebContents views
// are children of host->window() and walking up will UAF if the host
// dies first. main_parts enforces that ordering by declaration order +
// reset() sequence.
//
// Cross-references:
//   * content/shell/browser/shell_platform_data_aura.{h,cc} (the
//     canonical content_shell pattern this mirrors; uses test-only
//     focus + parenting client classes that we cannot depend on from
//     a non-test executable, so we provide our own — see cb_focus_
//     client.{h,cc} and cb_window_parenting_client.{h,cc}).
//   * headless/lib/browser/headless_window_tree_host.cc (the headless
//     pattern; uses a custom WindowTreeHost subclass instead of the
//     ozone-backed real one because headless deliberately runs without
//     a platform window — we DO have Xvfb so we can use the real one).

#ifndef CAPTURE_BUILD_INTEGRATION_CB_AURA_PLATFORM_DATA_H_
#define CAPTURE_BUILD_INTEGRATION_CB_AURA_PLATFORM_DATA_H_

#include <memory>

#include "ui/gfx/geometry/size.h"

namespace aura {
class WindowTreeHost;
namespace client {
class DefaultCaptureClient;
}  // namespace client
}  // namespace aura

namespace cloud_browser {

class CbCursorClient;
class CbFocusClient;
class CbWindowParentingClient;

class CbAuraPlatformData {
 public:
  // Initial viewport. The cb-chromium pod brings up Xvfb at 1280x720,
  // so the default platform data uses the same; pages can grow beyond
  // that via the renderer-side viewport, this is just the host window's
  // initial size.
  explicit CbAuraPlatformData(const gfx::Size& initial_size);

  CbAuraPlatformData(const CbAuraPlatformData&) = delete;
  CbAuraPlatformData& operator=(const CbAuraPlatformData&) = delete;

  ~CbAuraPlatformData();

  aura::WindowTreeHost* host() { return host_.get(); }
  CbCursorClient* cursor_client() { return cursor_client_.get(); }

 private:
  // Declaration order matters — destruction is reverse. Clients that
  // observe / are registered against host_->window() must be torn down
  // BEFORE host_ itself is destroyed, otherwise their dtors would reach
  // into a dead window. unique_ptr's reverse-declaration teardown
  // handles that automatically.
  //
  // wm::DefaultActivationClient is INTENTIONALLY NOT held in a smart
  // pointer here. Per its class doc (ui/wm/core/default_activation_
  // client.h:24), "this object deletes itself when the root window it
  // is associated with is destroyed" — it installs an inner aura::
  // WindowObserver Deleter that calls `delete this` from
  // OnWindowDestroyed. Owning it via unique_ptr would cause a
  // double-free (unique_ptr.reset() deletes; then root-window teardown
  // triggers Deleter's delete on the now-dangling pointer). Same shape
  // content_shell uses (`new wm::DefaultActivationClient(host_->
  // window())` in shell_platform_data_aura.cc, return value discarded).
  std::unique_ptr<aura::WindowTreeHost> host_;
  std::unique_ptr<CbFocusClient> focus_client_;
  std::unique_ptr<aura::client::DefaultCaptureClient> capture_client_;
  std::unique_ptr<CbWindowParentingClient> window_parenting_client_;
  // M5 R1 (CV2-19) — cursor observer. Registered via
  // aura::client::SetCursorClient on host_->window() in the ctor,
  // explicitly deregistered in the dtor before host_ goes away (same
  // shape as focus_client_).
  std::unique_ptr<CbCursorClient> cursor_client_;
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CB_AURA_PLATFORM_DATA_H_

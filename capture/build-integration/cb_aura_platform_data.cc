// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CbAuraPlatformData — see cb_aura_platform_data.h.

#include "capture/build-integration/cb_aura_platform_data.h"

#include <memory>
#include <utility>

#include "base/check.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "capture/build-integration/cb_cursor_client.h"
#include "capture/build-integration/cb_focus_client.h"
#include "capture/build-integration/cb_window_parenting_client.h"
#include "ui/aura/client/cursor_client.h"
#include "ui/aura/client/default_capture_client.h"
#include "ui/aura/client/focus_client.h"
#include "ui/aura/client/window_types.h"
#include "ui/aura/env.h"
#include "ui/aura/layout_manager.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/aura/window_tree_host_platform.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/platform_window/platform_window_init_properties.h"
#include "ui/wm/core/default_activation_client.h"

namespace cloud_browser {

namespace {

// Layout manager that grows children to fill the root window. Children
// added via WebContentsViewAura don't carry their own bounds, so without
// a layout manager their renderer-side viewport reports as 0x0. Same
// pattern content_shell uses (shell_platform_data_aura.cc::FillLayout).
class FillLayout : public aura::LayoutManager {
 public:
  explicit FillLayout(aura::Window* root)
      : root_(root), has_bounds_(!root->bounds().IsEmpty()) {}

  FillLayout(const FillLayout&) = delete;
  FillLayout& operator=(const FillLayout&) = delete;

  ~FillLayout() override = default;

 private:
  void OnWindowResized() override {
    if (!has_bounds_) {
      has_bounds_ = true;
      for (aura::Window* child : root_->children()) {
        SetChildBoundsDirect(child, gfx::Rect(root_->bounds().size()));
      }
    }
  }

  void OnWindowAddedToLayout(aura::Window* child) override {
    child->SetBounds(root_->bounds());
  }

  void OnWillRemoveWindowFromLayout(aura::Window* /*child*/) override {}
  void OnWindowRemovedFromLayout(aura::Window* /*child*/) override {}
  void OnChildWindowVisibilityChanged(aura::Window* /*child*/,
                                      bool /*visible*/) override {}

  void SetChildBounds(aura::Window* child,
                      const gfx::Rect& requested_bounds) override {
    SetChildBoundsDirect(child, requested_bounds);
  }

  raw_ptr<aura::Window> root_;
  bool has_bounds_;
};

// WindowTreeHostPlatform subclass that creates its ui::Compositor with
// use_external_begin_frame_control=true.
//
// WHY a subclass (and why we cannot just call WindowTreeHost::Create):
// use_external_begin_frame_control is a CONST argument to the ui::Compositor
// constructor — it cannot be flipped after the compositor exists. On the
// ozone path, the concrete host is aura::WindowTreeHostPlatform, whose PUBLIC
// constructor (window_tree_host_platform.cc) hard-codes
// CreateCompositor(false, /*use_external_begin_frame_control=*/false, ...)
// BEFORE CreateAndSetPlatformWindow(). InitHost() does NOT create the
// compositor on this path — the public ctor already did. So to enable the
// flag we must intercept compositor creation at CONSTRUCTION time.
//
// WindowTreeHostPlatform exposes a PROTECTED ctor that takes the root Window
// and intentionally does NOT create a compositor ("subclasses must call
// CreateCompositor() at the appropriate time"). We use it to replicate the
// public ctor verbatim, changing ONLY the second CreateCompositor argument to
// true. Ordering is identical to upstream: CreateCompositor() first, then
// CreateAndSetPlatformWindow() — which synchronously brings up the ozone-X11
// window and dispatches OnAcceleratedWidgetAvailable(xid). Because the
// compositor already exists by then, the base OnAcceleratedWidgetAvailable
// wires SetAcceleratedWidget(xid) exactly as the stock path does. The caller
// then runs the normal InitHost() (which only does UpdateRootWindowSize +
// InitCompositor on this path).
//
// This is the same external-begin-frame-control model headless uses, but
// adapted to a REAL ozone-X11 platform window (headless subclasses
// WindowTreeHost directly with a null widget; we keep the real X11 window so
// hit-testing / cursor routing / the BUGS-529 focus chain are unchanged).
//
// See cb_begin_frame_driver.h for why external begin frame control is needed
// at all (the capturer does not drive the renderer; without a driven
// BeginFrameSource the captured renderer idles at viz's 1s refresh ≈0.5 fps).
class CbExternalBeginFrameWindowTreeHost : public aura::WindowTreeHostPlatform {
 public:
  explicit CbExternalBeginFrameWindowTreeHost(
      ui::PlatformWindowInitProperties properties)
      // Protected ctor: hands the root Window to the base WindowTreeHost and
      // does NOT create a compositor. Construct the Window exactly as
      // WindowTreeHost::Create does.
      : aura::WindowTreeHostPlatform(std::make_unique<aura::Window>(
            nullptr,
            aura::client::WINDOW_TYPE_UNKNOWN)) {
    // Replicate aura::WindowTreeHostPlatform's public ctor, with the single
    // change of use_external_begin_frame_control false -> true. We deliberately
    // do NOT touch size_in_pixels_ here (it is private to the base and is set
    // by CreateAndSetPlatformWindow below from properties.bounds — the public
    // ctor's separate assignment is redundant with that). The host's bounds
    // are additionally force-propagated by CbAuraPlatformData via
    // SetBoundsInPixels right after InitHost(), as before.
    CreateCompositor(/*force_software_compositor=*/false,
                     /*use_external_begin_frame_control=*/true,
                     properties.enable_compositing_based_throttling,
                     properties.compositor_memory_limit_mb);
    CreateAndSetPlatformWindow(std::move(properties));
  }

  CbExternalBeginFrameWindowTreeHost(
      const CbExternalBeginFrameWindowTreeHost&) = delete;
  CbExternalBeginFrameWindowTreeHost& operator=(
      const CbExternalBeginFrameWindowTreeHost&) = delete;

  ~CbExternalBeginFrameWindowTreeHost() override = default;
};

}  // namespace

CbAuraPlatformData::CbAuraPlatformData(const gfx::Size& initial_size) {
  // aura::Env was created by content::BrowserMainLoop before our
  // PreMainMessageLoopRun ran; ozone (X11 backend, --ozone-platform=x11)
  // was initialised inside that aura::Env ctor. Just sanity-check.
  CHECK(aura::Env::HasInstance());

  ui::PlatformWindowInitProperties properties;
  properties.bounds = gfx::Rect(initial_size);

  // Use our WindowTreeHostPlatform subclass instead of
  // aura::WindowTreeHost::Create() so the root UI compositor is created with
  // use_external_begin_frame_control=true. That is the precondition for
  // CbBeginFrameDriver to drive frame production via
  // ui::Compositor::IssueExternalBeginFrame — which is what actually advances
  // the captured renderer at the target fps (see cb_begin_frame_driver.h for
  // the renderer-vs-root-compositor rationale). The const flag must be set at
  // compositor-construction time; WindowTreeHost::Create hard-codes it false,
  // hence the subclass. InitHost() below performs the same
  // UpdateRootWindowSize + InitCompositor it always did (it does NOT re-create
  // the compositor on the platform path).
  host_ = std::make_unique<CbExternalBeginFrameWindowTreeHost>(
      std::move(properties));
  host_->InitHost();
  host_->window()->Show();

  // Force-propagate the bounds to the aura::Window root + the platform
  // window. WindowTreeHost::Create stores the bounds on the host but the
  // ozone-X11 backend dispatches the actual platform-window resize
  // asynchronously, so host_->window()->bounds() can still be 0x0 by the
  // time PreMainMessageLoopRun's WebContents::Create runs. With 0x0 root
  // bounds, FillLayout::OnWindowAddedToLayout adds the WebContentsView
  // Aura with 0x0 bounds, the renderer's viewport reports as 0x0, and
  // CDP Input.dispatchMouseEvent coordinates fall outside every DOM
  // element — clicks register at the document level but never fire on
  // the buttons the test expects (the BUGS-529 wire-clicks symptom seen
  // post-cr7727-aura: 0/3 PASS even though events DO reach window —
  // form-element listeners never fire because hit-testing misses).
  // SetBoundsInPixels propagates synchronously through the ozone path
  // and updates host_->window()->bounds() before we leave this ctor.
  // Mirrors content_shell's ShellPlatformDelegate::CreatePlatformWindow
  // → ShellPlatformDataAura::ResizeWindow sequence (called between
  // platform_data ctor and SetContents).
  const gfx::Rect bounds_before = host_->window()->bounds();
  host_->SetBoundsInPixels(gfx::Rect(initial_size));
  const gfx::Rect bounds_after = host_->window()->bounds();
  LOG(INFO) << "CbAuraPlatformData: host bounds before SetBoundsInPixels="
            << bounds_before.ToString()
            << " / after=" << bounds_after.ToString()
            << " / requested=" << gfx::Rect(initial_size).ToString();

  // Mirror content_shell's Aura platform path: showing the root Window is not
  // enough on its own; the WindowTreeHost must also be shown so Aura hit
  // testing can route points to the WebContents child instead of returning no
  // event handler. Without this, RenderWidgetHostViewAura receives renderer
  // cursor updates but UpdateCursorIfOverSelf() stops at
  // root_window->GetEventHandlerForPoint(...)=nullptr before it reaches
  // CbCursorClient::SetCursor.
  host_->Show();

  host_->window()->SetLayoutManager(
      std::make_unique<FillLayout>(host_->window()));

  // Order of client registration:
  //   1. Focus client — registered via SetFocusClient inside the dtor
  //      of CbFocusClient registered on host_->window(); but we hold
  //      it as a member so its lifetime is bound here.
  //
  //   We use SetFocusClient explicitly here because CbFocusClient's
  //   ctor doesn't take the root window — mirrors HeadlessFocusClient
  //   which is per-window-tree-host registered by external code.
  focus_client_ = std::make_unique<CbFocusClient>();
  aura::client::SetFocusClient(host_->window(), focus_client_.get());

  // 2. Activation client. wm::DefaultActivationClient self-deletes on
  //    root-window destruction (see header note + ui/wm/core/default_
  //    activation_client.h:24). Bare `new` matches content_shell's
  //    shell_platform_data_aura.cc pattern; the inner Deleter
  //    aura::WindowObserver handles cleanup.
  new wm::DefaultActivationClient(host_->window());

  // 3. Capture client.
  capture_client_ = std::make_unique<aura::client::DefaultCaptureClient>(
      host_->window());

  // 4. Window parenting client. RAII self-registers on construction,
  //    deregisters on destruction.
  window_parenting_client_ =
      std::make_unique<CbWindowParentingClient>(host_->window());

  // 5. Cursor client (M5 R1 / CV2-19) — last because nothing else
  //    needs it during platform-data construction, but the renderer
  //    will reach for GetCursorClient(window) as soon as the first
  //    cursor-style change hits RenderWidgetHostViewAura. Mirrors
  //    CbFocusClient: explicit SetCursorClient registration here +
  //    explicit clear in dtor (the ctor doesn't take root_window
  //    only because TestCursorClient happens to register from its
  //    ctor; we centralise registration here for teardown-order
  //    parity with focus_client_).
  cursor_client_ = std::make_unique<CbCursorClient>(host_->window());
  aura::client::SetCursorClient(host_->window(), cursor_client_.get());
}

CbAuraPlatformData::~CbAuraPlatformData() {
  // Explicit teardown — the focus client was registered via
  // SetFocusClient (not by its ctor), so it wouldn't auto-deregister
  // in its dtor. Clear the registration BEFORE the host (and its
  // window) goes away so the SetFocusClient(window, nullptr) sees a
  // live window.
  if (host_ && host_->window()) {
    aura::client::SetFocusClient(host_->window(), nullptr);
    // M5 R1 — cursor client was registered via SetCursorClient (not
    // by its ctor), so it wouldn't auto-deregister in its dtor. Clear
    // the registration BEFORE host_->window() goes away.
    aura::client::SetCursorClient(host_->window(), nullptr);
  }
  // Other clients (parenting, capture, activation) self-deregister in
  // their dtors. unique_ptr destruction order (reverse of declaration)
  // already handles that — the host is destroyed last.
}

}  // namespace cloud_browser

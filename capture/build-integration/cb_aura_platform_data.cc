// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CbAuraPlatformData — see cb_aura_platform_data.h.

#include "capture/build-integration/cb_aura_platform_data.h"

#include <memory>
#include <utility>

#include "base/check.h"
#include "base/memory/raw_ptr.h"
#include "capture/build-integration/cb_focus_client.h"
#include "capture/build-integration/cb_window_parenting_client.h"
#include "ui/aura/client/default_capture_client.h"
#include "ui/aura/client/focus_client.h"
#include "ui/aura/env.h"
#include "ui/aura/layout_manager.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
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

}  // namespace

CbAuraPlatformData::CbAuraPlatformData(const gfx::Size& initial_size) {
  // aura::Env was created by content::BrowserMainLoop before our
  // PreMainMessageLoopRun ran; ozone (X11 backend, --ozone-platform=x11)
  // was initialised inside that aura::Env ctor. Just sanity-check.
  CHECK(aura::Env::HasInstance());

  ui::PlatformWindowInitProperties properties;
  properties.bounds = gfx::Rect(initial_size);

  host_ = aura::WindowTreeHost::Create(std::move(properties));
  host_->InitHost();
  host_->window()->Show();
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

  // 4. Window parenting client — last so a window-create that races
  //    against ctor doesn't get parented to a half-built tree. RAII
  //    self-registers on construction, deregisters on destruction.
  window_parenting_client_ =
      std::make_unique<CbWindowParentingClient>(host_->window());
}

CbAuraPlatformData::~CbAuraPlatformData() {
  // Explicit teardown — the focus client was registered via
  // SetFocusClient (not by its ctor), so it wouldn't auto-deregister
  // in its dtor. Clear the registration BEFORE the host (and its
  // window) goes away so the SetFocusClient(window, nullptr) sees a
  // live window.
  if (host_ && host_->window()) {
    aura::client::SetFocusClient(host_->window(), nullptr);
  }
  // Other clients (parenting, capture, activation) self-deregister in
  // their dtors. unique_ptr destruction order (reverse of declaration)
  // already handles that — the host is destroyed last.
}

}  // namespace cloud_browser

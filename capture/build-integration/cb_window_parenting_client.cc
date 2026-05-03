// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CbWindowParentingClient — see cb_window_parenting_client.h.

#include "capture/build-integration/cb_window_parenting_client.h"

#include "ui/aura/window.h"

namespace cloud_browser {

CbWindowParentingClient::CbWindowParentingClient(aura::Window* root_window)
    : root_window_(root_window) {
  aura::client::SetWindowParentingClient(root_window_, this);
}

CbWindowParentingClient::~CbWindowParentingClient() {
  aura::client::SetWindowParentingClient(root_window_, nullptr);
}

aura::Window* CbWindowParentingClient::GetDefaultParent(
    aura::Window* /*window*/,
    const gfx::Rect& /*bounds*/,
    const int64_t /*display_id*/) {
  return root_window_;
}

}  // namespace cloud_browser

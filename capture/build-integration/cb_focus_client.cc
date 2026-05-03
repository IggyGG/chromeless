// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CbFocusClient — see cb_focus_client.h.

#include "capture/build-integration/cb_focus_client.h"

#include "ui/aura/client/focus_change_observer.h"
#include "ui/aura/window.h"

namespace cloud_browser {

CbFocusClient::CbFocusClient() = default;

CbFocusClient::~CbFocusClient() = default;

void CbFocusClient::AddObserver(
    aura::client::FocusChangeObserver* observer) {
  focus_observers_.AddObserver(observer);
}

void CbFocusClient::RemoveObserver(
    aura::client::FocusChangeObserver* observer) {
  focus_observers_.RemoveObserver(observer);
}

void CbFocusClient::FocusWindow(aura::Window* window) {
  if (window && !window->CanFocus()) {
    return;
  }

  if (focused_window_) {
    DCHECK(observation_manager_.IsObservingSource(focused_window_.get()));
    observation_manager_.Reset();
  }
  aura::Window* old_focused_window = focused_window_;
  focused_window_ = window;
  if (focused_window_) {
    observation_manager_.Observe(focused_window_.get());
  }

  for (aura::client::FocusChangeObserver& observer : focus_observers_) {
    observer.OnWindowFocused(focused_window_, old_focused_window);
  }
  if (aura::client::FocusChangeObserver* observer =
          aura::client::GetFocusChangeObserver(old_focused_window)) {
    observer->OnWindowFocused(focused_window_, old_focused_window);
  }
  if (aura::client::FocusChangeObserver* observer =
          aura::client::GetFocusChangeObserver(focused_window_)) {
    observer->OnWindowFocused(focused_window_, old_focused_window);
  }
}

void CbFocusClient::ResetFocusWithinActiveWindow(aura::Window* window) {
  if (!window->Contains(focused_window_)) {
    FocusWindow(window);
  }
}

aura::Window* CbFocusClient::GetFocusedWindow() {
  return focused_window_;
}

void CbFocusClient::OnWindowDestroying(aura::Window* window) {
  DCHECK_EQ(window, focused_window_);
  FocusWindow(nullptr);
}

}  // namespace cloud_browser

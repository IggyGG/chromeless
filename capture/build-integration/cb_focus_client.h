// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CbFocusClient — minimal aura::client::FocusClient for the cb-chromium
// worker. Single-window focus model: tracks one currently-focused
// aura::Window, observes its destruction, and notifies the registered
// FocusChangeObservers on every focus change.
//
// Why we need it (BUGS-529 follow-up):
//   content::WebContents::Focus() walks down to RenderWidgetHostViewAura::
//   Focus(), which calls aura::client::GetFocusClient(window)->FocusWindow
//   (window). If no FocusClient is registered on the window's root, the
//   GetFocusClient lookup returns nullptr and the call is a silent no-op.
//   The renderer-side WidgetInputHandler then binds in "no focused page"
//   state, treating the page as a backgrounded tab, and CDP
//   Input.dispatch{Mouse,Key}Event are dropped on the floor by the
//   renderer despite acking at the protocol layer.
//
//   The 9703db5 patch wired WebContents::WasShown() + WebContents::Focus()
//   on every WebContents we create, but Focus() had no effect because no
//   FocusClient was registered. That was BUGS-529's deeper root cause.
//
// Cross-references:
//   * headless/lib/browser/headless_focus_client.{h,cc}
//     (the canonical minimal pattern; we mirror it nearly verbatim)
//   * ui/aura/test/test_focus_client.{h,cc}
//     (the testonly equivalent content_shell uses; we cannot depend on
//     ui/aura:test_support from a non-test executable, so we roll our
//     own embedder copy)
//   * ui/aura/client/focus_client.h (the interface contract)

#ifndef CAPTURE_BUILD_INTEGRATION_CB_FOCUS_CLIENT_H_
#define CAPTURE_BUILD_INTEGRATION_CB_FOCUS_CLIENT_H_

#include "base/memory/raw_ptr.h"
#include "base/observer_list.h"
#include "base/scoped_observation.h"
#include "ui/aura/client/focus_client.h"
#include "ui/aura/window_observer.h"

namespace cloud_browser {

class CbFocusClient : public aura::client::FocusClient,
                      public aura::WindowObserver {
 public:
  CbFocusClient();

  CbFocusClient(const CbFocusClient&) = delete;
  CbFocusClient& operator=(const CbFocusClient&) = delete;

  ~CbFocusClient() override;

 private:
  // aura::client::FocusClient:
  void AddObserver(aura::client::FocusChangeObserver* observer) override;
  void RemoveObserver(aura::client::FocusChangeObserver* observer) override;
  void FocusWindow(aura::Window* window) override;
  void ResetFocusWithinActiveWindow(aura::Window* window) override;
  aura::Window* GetFocusedWindow() override;

  // aura::WindowObserver:
  void OnWindowDestroying(aura::Window* window) override;

  raw_ptr<aura::Window> focused_window_ = nullptr;
  base::ScopedObservation<aura::Window, aura::WindowObserver>
      observation_manager_{this};
  base::ObserverList<aura::client::FocusChangeObserver> focus_observers_;
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CB_FOCUS_CLIENT_H_

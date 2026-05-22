// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CbCursorClient — minimal aura::client::CursorClient for the cb-chromium
// worker. Observes every renderer-driven cursor change in the browser
// process so M5 can lift cursor egress out of the JS+CDP poll loop in
// capture/cursor-watcher/ and emit cursor envelopes directly off the
// DataChannel host wired in M3.
//
// Why we need it (M5 / CV2-19):
//   When a CSS style change moves the cursor under the pointer (e.g.
//   hovering a `cursor: pointer` button), RenderWidgetHostViewAura
//   calls aura::client::GetCursorClient(window)->SetCursor(cursor).
//   If no CursorClient is registered on the window's root, the
//   GetCursorClient lookup returns nullptr and the call is a silent
//   no-op — the browser process never sees the cursor change. The
//   only way the operator side learns about it today is the JS probe
//   in capture/cursor-watcher/main.go polling
//   getComputedStyle(elementFromPoint(...)).cursor on every rAF and
//   funnelling the result through Runtime.bindingCalled. That works
//   but adds a frame of latency, costs a sidecar, and breaks on every
//   navigation while the probe re-installs.
//
//   Registering a CursorClient on host_->window() lets the browser
//   process intercept SetCursor / ShowCursor / HideCursor directly.
//   R1 is the foundation: it captures the change and emits a
//   LOG(INFO) trampoline that proves the seam works end-to-end.
//   R2..R6 in this module hang envelope assembly + DataChannel
//   emission off the same callback the trampoline drives.
//
// Mirrors the CbFocusClient pattern exactly:
//   - Single-window model (host_->window() only — we run one tab).
//   - Observes that window's destruction to clear state.
//   - Registered explicitly via aura::client::SetCursorClient by
//     CbAuraPlatformData; cleared in CbAuraPlatformData's dtor before
//     the host window goes away.
//
// Cross-references:
//   - ui/aura/test/test_cursor_client.{h,cc}
//     The testonly equivalent. We can't depend on ui/aura:test_support
//     from a non-test executable, so we roll our own embedder copy.
//   - ui/aura/client/cursor_client.h (the interface contract).
//   - ui/wm/core/cursor_manager.{h,cc} (the heavyweight ash/chrome
//     implementation; far more than we need — it owns native cursor
//     loading, display routing, locking semantics. The cb-chromium
//     pod runs without a real cursor surface so most of that machinery
//     is overkill).
//   - capture/cursor-watcher/main.go (the Go sidecar this seam will
//     replace once M5 lands end-to-end; documents the wire envelope
//     shape downstream consumers expect).
//   - capture/build-integration/cb_focus_client.{h,cc} (the structural
//     template).
//
// Non-goals (R1 only):
//   * Envelope formatting — TODO(M5-R2-envelope).
//   * DataChannel emission — TODO(M5-R3-emit).
//   * Pointer position — RenderWidgetHostViewAura's SetCursor signature
//     does not carry coordinates; M5 R4 wires a separate mouse-move
//     observer for that.
//   * Custom-image cursor bytes — TODO(M5-R5-custom-image).

#ifndef CAPTURE_BUILD_INTEGRATION_CB_CURSOR_CLIENT_H_
#define CAPTURE_BUILD_INTEGRATION_CB_CURSOR_CLIENT_H_

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/observer_list.h"
#include "base/scoped_observation.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/aura/client/cursor_client.h"
#include "ui/aura/window_observer.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/cursor/cursor_size.h"
#include "ui/display/display.h"

namespace cloud_browser {

// Callback the embedder registers to receive cursor changes. Fired on
// SetCursor, SetCursorForced, ShowCursor, and HideCursor.
//
// Arguments:
//   type    — the cursor type ui::mojom::CursorType the renderer
//             asked for (kPointer, kHand, kIBeam, kWait, kCustom, …).
//             For kCustom the actual bitmap is on the ui::Cursor; R1
//             ignores the bytes, R5 will plumb them through.
//   visible — current visibility state of the cursor; toggles in
//             response to ShowCursor / HideCursor independently of
//             SetCursor.
//
// R1 stub: CbAuraPlatformData does NOT register a callback in this
// commit — the LOG(INFO) trampoline is the only consumer so the
// acceptance probe (pod-log scrape) can fire without depending on
// M5 R2+. R2 will plumb the DataChannel host through here.
using CursorChangeCallback =
    base::RepeatingCallback<void(ui::mojom::CursorType type, bool visible)>;

class CbCursorClient : public aura::client::CursorClient,
                       public aura::WindowObserver {
 public:
  // The constructor takes the root window so it can observe its
  // destruction (mirrors TestCursorClient + the ScopedObservation
  // pattern in CbFocusClient). Registration on the root window is
  // done by CbAuraPlatformData via aura::client::SetCursorClient,
  // NOT here — that mirrors CbFocusClient and lets the platform-data
  // class control teardown ordering.
  explicit CbCursorClient(aura::Window* root_window);

  CbCursorClient(const CbCursorClient&) = delete;
  CbCursorClient& operator=(const CbCursorClient&) = delete;

  ~CbCursorClient() override;

  // Wires the in-process change callback. CbAuraPlatformData calls
  // this once after construction when an embedder consumer (M5 R2's
  // envelope emitter) is ready. Pass an empty callback to detach.
  // Replacing a previously-set callback is allowed.
  void SetCursorChangeCallback(CursorChangeCallback callback);

 private:
  // aura::client::CursorClient:
  void SetCursor(gfx::NativeCursor cursor) override;
  gfx::NativeCursor GetCursor() const override;
  void SetCursorForced(gfx::NativeCursor cursor) override;
  void ShowCursor() override;
  void HideCursor() override;
  void SetCursorSize(ui::CursorSize cursor_size) override;
  ui::CursorSize GetCursorSize() const override;
  void SetLargeCursorSizeInDip(int large_cursor_size_in_dip) override;
  int GetLargeCursorSizeInDip() const override;
  void SetCursorColor(SkColor color) override;
  SkColor GetCursorColor() const override;
  bool IsCursorVisible() const override;
  void EnableMouseEvents() override;
  void DisableMouseEvents() override;
  bool IsMouseEventsEnabled() const override;
  void SetDisplay(const display::Display& display) override;
  const display::Display& GetDisplay() const override;
  void LockCursor() override;
  void UnlockCursor() override;
  bool IsCursorLocked() const override;
  void AddObserver(aura::client::CursorClientObserver* observer) override;
  void RemoveObserver(aura::client::CursorClientObserver* observer) override;
  bool ShouldHideCursorOnKeyEvent(const ui::KeyEvent& event) const override;
  bool ShouldHideCursorOnTouchEvent(
      const ui::TouchEvent& event) const override;
  gfx::Size GetSystemCursorSize() const override;

  // aura::WindowObserver:
  void OnWindowDestroying(aura::Window* window) override;

  // Internal — common path for SetCursor/SetCursorForced. Updates
  // cached state, fires the LOG(INFO) trampoline + the registered
  // callback (if any), and notifies CursorClientObservers when
  // visibility changes as a side-effect (it does not in R1, but kept
  // symmetric for SetCursorForced).
  void HandleCursorSet(gfx::NativeCursor cursor, bool forced);

  // Cached cursor + visibility. We don't drive a real surface — the
  // cb-chromium pod runs without a visible OS cursor — so the state is
  // purely observed-from-renderer.
  ui::Cursor current_cursor_;
  bool visible_ = true;
  bool mouse_events_enabled_ = true;
  int cursor_lock_count_ = 0;
  ui::CursorSize cursor_size_ = ui::CursorSize::kNormal;
  int large_cursor_size_in_dip_ = ui::kDefaultLargeCursorSize;
  SkColor cursor_color_ = ui::kDefaultCursorColor;
  display::Display display_;

  // Embedder consumer; empty by default. See CursorChangeCallback above.
  CursorChangeCallback change_callback_;

  // Mirrors CbFocusClient's pattern: ScopedObservation watches the
  // root window so we get OnWindowDestroying before the registration
  // pointer goes stale.
  raw_ptr<aura::Window> root_window_;
  base::ScopedObservation<aura::Window, aura::WindowObserver>
      observation_manager_{this};

  // Cursor-client observer list (visibility changes etc.). Kept
  // primarily for protocol completeness — the cb-chromium pod has
  // no on-screen toolbar wiring up to it today.
  base::ObserverList<aura::client::CursorClientObserver>::Unchecked
      observers_;
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CB_CURSOR_CLIENT_H_

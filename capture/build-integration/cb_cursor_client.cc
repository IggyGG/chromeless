// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CbCursorClient — see cb_cursor_client.h.

#include "capture/build-integration/cb_cursor_client.h"

#include "base/check.h"
#include "base/logging.h"
#include "ui/aura/client/cursor_client_observer.h"
#include "ui/aura/window.h"
#include "ui/base/cursor/mojom/cursor_type.mojom-shared.h"
#include "ui/gfx/geometry/size.h"

namespace cloud_browser {

CbCursorClient::CbCursorClient(aura::Window* root_window)
    : root_window_(root_window) {
  DCHECK(root_window_);
  observation_manager_.Observe(root_window_.get());
  LOG(INFO) << "CbCursorClient: constructed; root_window=" << root_window_;
}

CbCursorClient::~CbCursorClient() {
  // If the window outlived us (the platform-data-driven teardown
  // path), observation_manager_ is still observing; its dtor handles
  // the Reset(). If the window was destroyed first,
  // OnWindowDestroying already reset us. Either way is safe.
  LOG(INFO) << "CbCursorClient: destroyed";
}

void CbCursorClient::SetCursorChangeCallback(CursorChangeCallback callback) {
  change_callback_ = std::move(callback);
}

// ---------------------------------------------------------------------
// aura::client::CursorClient — capturing path
// ---------------------------------------------------------------------

void CbCursorClient::SetCursor(gfx::NativeCursor cursor) {
  HandleCursorSet(cursor, /*forced=*/false);
}

void CbCursorClient::SetCursorForced(gfx::NativeCursor cursor) {
  HandleCursorSet(cursor, /*forced=*/true);
}

void CbCursorClient::HandleCursorSet(gfx::NativeCursor cursor, bool forced) {
  const ui::mojom::CursorType old_type = current_cursor_.type();
  current_cursor_ = cursor;
  const ui::mojom::CursorType new_type = current_cursor_.type();

  // Track-E LOG(INFO) trampoline — acceptance probe scrapes pod logs
  // for this line. Pre-fix expectation: no such line; post-fix
  // expectation: one per CSS-cursor-style change under the pointer.
  //
  // Format kept stable for grep-friendly assertions in the verification
  // harness — change with care (capture/gate-harness or M5 R6
  // acceptance script will need a matching update). Logging the
  // numeric enum value avoids dragging the IPC_ENUM_TRAITS chrome
  // logging hooks into this TU.
  LOG(INFO) << "CbCursorClient::SetCursor"
            << (forced ? " (forced)" : "")
            << " new_type=" << static_cast<int>(new_type)
            << " old_type=" << static_cast<int>(old_type)
            << " visible=" << visible_;

  // TODO(M5-R2-envelope): assemble a v1 cursor envelope from
  // (new_type, visible_, current_cursor_.custom_bitmap(), …) and
  // hand it off via change_callback_ once the embedder hooks the
  // DataChannel host through.
  if (change_callback_) {
    change_callback_.Run(new_type, visible_);
  }
}

gfx::NativeCursor CbCursorClient::GetCursor() const {
  return current_cursor_;
}

void CbCursorClient::ShowCursor() {
  const bool changed = !visible_;
  visible_ = true;
  if (changed) {
    LOG(INFO) << "CbCursorClient::ShowCursor visible=true";
    observers_.Notify(
        &aura::client::CursorClientObserver::OnCursorVisibilityChanged, true);
    if (change_callback_) {
      change_callback_.Run(current_cursor_.type(), /*visible=*/true);
    }
  }
}

void CbCursorClient::HideCursor() {
  const bool changed = visible_;
  visible_ = false;
  if (changed) {
    LOG(INFO) << "CbCursorClient::HideCursor visible=false";
    observers_.Notify(
        &aura::client::CursorClientObserver::OnCursorVisibilityChanged, false);
    if (change_callback_) {
      change_callback_.Run(current_cursor_.type(), /*visible=*/false);
    }
  }
}

bool CbCursorClient::IsCursorVisible() const {
  return visible_;
}

// ---------------------------------------------------------------------
// aura::client::CursorClient — passthrough state (size / color / lock /
// display / mouse-events / observers / system-cursor metrics).
//
// The cb-chromium worker has no on-screen UI to honor these, but the
// renderer queries them through the GetCursorClient path during
// layout / input routing, so we keep cached values + plausible
// defaults rather than CHECK(false).
// ---------------------------------------------------------------------

void CbCursorClient::SetCursorSize(ui::CursorSize cursor_size) {
  cursor_size_ = cursor_size;
}

ui::CursorSize CbCursorClient::GetCursorSize() const {
  return cursor_size_;
}

void CbCursorClient::SetLargeCursorSizeInDip(int large_cursor_size_in_dip) {
  large_cursor_size_in_dip_ = large_cursor_size_in_dip;
}

int CbCursorClient::GetLargeCursorSizeInDip() const {
  return large_cursor_size_in_dip_;
}

void CbCursorClient::SetCursorColor(SkColor color) {
  cursor_color_ = color;
}

SkColor CbCursorClient::GetCursorColor() const {
  return cursor_color_;
}

void CbCursorClient::EnableMouseEvents() {
  mouse_events_enabled_ = true;
}

void CbCursorClient::DisableMouseEvents() {
  mouse_events_enabled_ = false;
}

bool CbCursorClient::IsMouseEventsEnabled() const {
  return mouse_events_enabled_;
}

void CbCursorClient::SetDisplay(const display::Display& display) {
  display_ = display;
}

const display::Display& CbCursorClient::GetDisplay() const {
  return display_;
}

void CbCursorClient::LockCursor() {
  ++cursor_lock_count_;
}

void CbCursorClient::UnlockCursor() {
  --cursor_lock_count_;
  if (cursor_lock_count_ < 0) {
    cursor_lock_count_ = 0;
  }
}

bool CbCursorClient::IsCursorLocked() const {
  return cursor_lock_count_ > 0;
}

void CbCursorClient::AddObserver(
    aura::client::CursorClientObserver* observer) {
  observers_.AddObserver(observer);
}

void CbCursorClient::RemoveObserver(
    aura::client::CursorClientObserver* observer) {
  observers_.RemoveObserver(observer);
}

bool CbCursorClient::ShouldHideCursorOnKeyEvent(
    const ui::KeyEvent& /*event*/) const {
  // Matches TestCursorClient default. The cb-chromium pod has no real
  // cursor surface so hide-on-key has no visible effect; we return
  // true to stay protocol-faithful — the renderer still asks.
  return true;
}

bool CbCursorClient::ShouldHideCursorOnTouchEvent(
    const ui::TouchEvent& /*event*/) const {
  return true;
}

gfx::Size CbCursorClient::GetSystemCursorSize() const {
  // 25x25 DIP matches TestCursorClient's stub. Real OS-supplied
  // cursor size doesn't apply — the cb-chromium worker streams its
  // cursor metadata over the DC and the client side renders it.
  return gfx::Size(25, 25);
}

// ---------------------------------------------------------------------
// aura::WindowObserver
// ---------------------------------------------------------------------

void CbCursorClient::OnWindowDestroying(aura::Window* window) {
  DCHECK_EQ(window, root_window_.get());
  observation_manager_.Reset();
  root_window_ = nullptr;
  LOG(INFO) << "CbCursorClient: root window destroyed";
}

}  // namespace cloud_browser

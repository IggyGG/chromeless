// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "capture/build-integration/cb_headless_screen.h"

#include "base/logging.h"

namespace cloud_browser {

CbHeadlessScreen::CbHeadlessScreen() = default;
CbHeadlessScreen::~CbHeadlessScreen() = default;

void CbHeadlessScreen::SetLastPointerSource(
    const CbLastPointerState* last_pointer_state) {
  last_pointer_state_ = last_pointer_state;
  LOG(INFO) << "CV2-83: CbHeadlessScreen last-pointer source "
            << (last_pointer_state_ ? "installed" : "cleared");
}

void CbHeadlessScreen::SetRootWindow(gfx::NativeWindow root_window) {
  root_window_ = root_window;
  LOG(INFO) << "CV2-83: CbHeadlessScreen root window "
            << (root_window_ ? "installed" : "cleared");
}

bool CbHeadlessScreen::IsWindowUnderCursor(gfx::NativeWindow window) {
  // Single-root-window embedder: cb_aura_platform_data.cc constructs
  // exactly one WindowTreeHost, and the only consumer that ever asks
  // this question is the RenderWidgetHostViewAura attached to that
  // root's WebContents. Any non-null window is therefore "under the
  // cursor" by virtue of being the only window we have. Null returns
  // false defensively — aura should not ask the question about a
  // null window, but the honest answer if it does is "no".
  //
  // This is the gate that the upstream display::ScreenBase stub
  // (`return false; NOTIMPLEMENTED_LOG_ONCE();`) was closing. With
  // the gate open, aura proceeds into the CursorClient routing path
  // (cb_cursor_client.cc::CbCursorClient::SetCursor), which the
  // M5 R6 functional-test harness asserts is reached when a hover
  // over a `cursor: pointer` element changes the renderer-reported
  // cursor type to kHand.
  return window != nullptr;
}

gfx::Point CbHeadlessScreen::GetCursorScreenPoint() {
  if (last_pointer_state_) {
    const CbLastPointerSnapshot& snap =
        last_pointer_state_->last_pointer();
    if (!snap.at.is_null() && snap.in_widget) {
      return gfx::Point(static_cast<int>(snap.x),
                        static_cast<int>(snap.y));
    }
  }

  // Conservative fallback before the first successful browser-process
  // pointer dispatch, or after the client reports a pointer leave.
  return gfx::Point();
}

gfx::NativeWindow CbHeadlessScreen::GetWindowAtScreenPoint(
    const gfx::Point& point) {
  if (!root_window_) {
    return nullptr;
  }

  const display::Display display = GetPrimaryDisplay();
  if (!display.bounds().Contains(point)) {
    return nullptr;
  }

  // The worker is a single-root-window embedder. Returning the root
  // satisfies RenderWidgetHostViewAura's pre-SetCursor same-root gate;
  // it still independently asks root_window->GetEventHandlerForPoint()
  // before routing to CursorClient::SetCursor.
  return root_window_;
}

display::Display CbHeadlessScreen::GetDisplayNearestWindow(
    gfx::NativeWindow /*window*/) const {
  // Single-display embedder collapse of headless_screen.cc's
  // GetDisplayFromScreenRect lookup. The cb-chromium worker seeds
  // exactly one Display into ScreenBase::display_list() at
  // construction (cloud_browser_browser_main_parts.cc
  // PreEarlyInitialization sets up the 1280x720 default), so the
  // "nearest" question has a single trivially-correct answer
  // regardless of the `window` argument: that one display.
  //
  // GetPrimaryDisplay() is ScreenBase's public const accessor for
  // exactly that — display_list().GetPrimaryDisplayIterator() with
  // the end()-check folded in (returns Display() on miss, which only
  // happens when display_list_ is empty; main_parts seeds it before
  // any code that reaches us can run, so the miss path is unreachable
  // in normal operation).
  //
  // This delegate intentionally ignores the `window` parameter. In a
  // multi-display embedder the canonical pattern would be
  // GetDisplayFromScreenRect(display_list().displays(),
  //                          window->GetBoundsInScreen())
  // with a primary-display fallback (see chromium upstream
  // headless/lib/browser/headless_screen.cc GetDisplayNearestWindow);
  // for the single-display worker that collapses to the same return
  // value the fallback would produce, so we skip the lookup entirely.
  // A future multi-display revision (no current ticket — the worker
  // is single-window/display by design) would restore the upstream
  // pattern here.
  return GetPrimaryDisplay();
}

}  // namespace cloud_browser

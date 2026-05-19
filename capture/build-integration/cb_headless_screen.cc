// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "capture/build-integration/cb_headless_screen.h"

namespace cloud_browser {

CbHeadlessScreen::CbHeadlessScreen() = default;
CbHeadlessScreen::~CbHeadlessScreen() = default;

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
  // R1 default. See cb_headless_screen.h class-level comment for the
  // wiring-frontier rationale — CbLastPointerState (M4 R10) is the
  // intended source, but its writer (CbInputDispatchMouse) is not
  // currently runtime-wired into main_parts (the input-DC observer
  // is CbInputLoggingDelegate per CV2-75 R1). Per lesson (j), this
  // commit advances ONE ring (Screen registration / cursor-gate);
  // routing the last-pointer through is a separate ring that the
  // CbInputDispatchMouse runtime-wire follow-up will resolve.
  //
  // Returning gfx::Point(0,0) matches the upstream ScreenBase stub's
  // observable behaviour (which was the installed Screen up until
  // this subclass landed), so any consumer that previously tolerated
  // the stub continues to tolerate this.
  return gfx::Point();
}

}  // namespace cloud_browser

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cloud-browser/capture/build-integration/cb_last_pointer.h"

namespace cloud_browser {

CbLastPointerState::CbLastPointerState() = default;
CbLastPointerState::~CbLastPointerState() = default;

void CbLastPointerState::Update(float widget_x,
                                float widget_y,
                                uint32_t buttons_blink,
                                base::TimeTicks at) {
  snap_.x = widget_x;
  snap_.y = widget_y;
  snap_.buttons_blink = buttons_blink;
  snap_.at = at;
  // A successful forward into chromium intrinsically re-establishes
  // pointer presence in the widget — this covers the leave-then-move
  // recovery path so a client that sent mouse_leave and then
  // mouse_move (without an intervening mouse_enter) ends up with the
  // correct in_widget=true reading.
  snap_.in_widget = true;
}

void CbLastPointerState::MarkLeft() {
  // Keep x / y / buttons_blink / at unchanged so a consumer painting
  // the remote cursor can freeze it on the last in-widget position
  // (the "stop drawing where the cursor was" alternative would
  // require the consumer to remember the last position itself —
  // duplicating state we already hold here).
  //
  // Idempotent: a redundant leave is a no-op.
  snap_.in_widget = false;
}

void CbLastPointerState::MarkEntered() {
  // Symmetric to MarkLeft — flips the bit, leaves the rest alone.
  // The next Update() also sets in_widget=true so a client that
  // omits the explicit mouse_enter envelope and goes straight to
  // mouse_move still ends up correct; this seam is for the v1.1
  // client that wants the explicit edge.
  snap_.in_widget = true;
}

bool HandlePointerLeaveEnvelope(const base::DictValue& /*data*/,
                                CbLastPointerState* state) {
  if (!state) {
    return false;
  }
  // v1.0: data carries no required fields. The TODO in the header
  // tracks the v1.1 "carry boundary coords + Update first" path so
  // a future client can paint the cursor sliding to the boundary
  // before it disappears, instead of freezing mid-widget.
  state->MarkLeft();
  return true;
}

bool HandlePointerEnterEnvelope(const base::DictValue& /*data*/,
                                CbLastPointerState* state) {
  if (!state) {
    return false;
  }
  state->MarkEntered();
  return true;
}

}  // namespace cloud_browser

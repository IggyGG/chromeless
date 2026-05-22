// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "capture/build-integration/cb_cursor_xy_join.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "capture/build-integration/cb_cursor_client.h"
#include "capture/build-integration/cb_input_dispatch_mouse.h"
#include "ui/base/cursor/mojom/cursor_type.mojom.h"

namespace cloud_browser {

CbCursorXyJoin::CbCursorXyJoin(CbCursorClient* cursor_client,
                               CbInputDispatchMouse* mouse_dispatch)
    : cursor_client_(cursor_client), mouse_dispatch_(mouse_dispatch) {
  // R1 cursor source is structurally required — without it there's
  // no upstream stream to subscribe to. M4 R3's snapshot source is
  // optional during bring-up; see ctor doc in the header.
  CHECK(cursor_client_);
}

CbCursorXyJoin::~CbCursorXyJoin() {
  // Detach our callback from the cursor client so it doesn't fire
  // into a freed `this`. R1's contract: passing an empty callback
  // detaches. CbAuraPlatformData's destruction order also clears
  // the cursor client itself before host_ tears down, but we don't
  // want to rely on outer destruction order — explicit detach is
  // cheap and removes a class of UAF on shutdown.
  if (initialized_ && cursor_client_) {
    cursor_client_->SetCursorChangeCallback(CursorChangeCallback());
  }
}

void CbCursorXyJoin::Initialize() {
  if (initialized_) {
    // Idempotent per the header contract — replacing a callback
    // with an identical bind is wasted work, so skip.
    return;
  }
  // base::Unretained is safe here: R3 owns the registration and
  // detaches in its dtor (see above), and the cursor client never
  // fires the callback after Detach. The cursor client is also
  // guaranteed to outlive R3 by the lifetime requirements documented
  // on the ctor (CbAuraPlatformData owns both and destroys R3
  // before nulling the cursor client). If those invariants change,
  // promote to a weak ptr.
  cursor_client_->SetCursorChangeCallback(base::BindRepeating(
      &CbCursorXyJoin::OnCursorChange, base::Unretained(this)));
  initialized_ = true;
}

void CbCursorXyJoin::SetEmitCallback(JoinedCursorCallback callback) {
  emit_callback_ = std::move(callback);
  // Once R2 wires up, the next OnCursorChange will flush — there's
  // no replay of pre-registration events because cursor edges are
  // edge-triggered and the operator side already has the previous
  // type as its idle baseline.
  //
  // TODO(M5-R3-emit-replay): re-evaluate when R2 lands. If the
  // operator UI needs to recover after a DC reconnect, a "one-shot
  // current state on subscribe" semantic would belong HERE (R3 has
  // both inputs ready) rather than in R2.
}

void CbCursorXyJoin::OnCursorChangeForTesting(ui::mojom::CursorType type,
                                              bool visible) {
  OnCursorChange(type, visible);
}

void CbCursorXyJoin::OnCursorChange(ui::mojom::CursorType type, bool visible) {
  JoinedCursorState joined;
  joined.type = type;
  joined.visible = visible;
  joined.joined_at = base::TimeTicks::Now();
  FillPositionFromSnapshot(joined);

  if (emit_callback_.is_null()) {
    if (!warned_no_emit_callback_) {
      // One-shot warning. R2 not yet wired up is expected during
      // bring-up; spamming the log on every cursor edge would
      // bury actual signal. Subsequent edges silently drop.
      LOG(WARNING) << "CbCursorXyJoin: cursor edge with no emit "
                      "callback registered; dropping (will not "
                      "warn again).";
      warned_no_emit_callback_ = true;
    }
    return;
  }
  emit_callback_.Run(joined);
}

void CbCursorXyJoin::FillPositionFromSnapshot(JoinedCursorState& out) const {
  if (!mouse_dispatch_) {
    if (!warned_no_mouse_dispatch_) {
      LOG(WARNING) << "CbCursorXyJoin: mouse_dispatch is null; cursor "
                      "joins will carry has_pointer_history=false "
                      "(will not warn again). Expected during M4 "
                      "wiring bring-up; remove this seam once M4 R3 "
                      "is integrated everywhere R3 is.";
      // Const-cast is safe: warning state is conceptually mutable.
      // If we add a few more of these, switch the fields to mutable
      // bool and drop the cast.
      const_cast<CbCursorXyJoin*>(this)->warned_no_mouse_dispatch_ = true;
    }
    // out fields already defaulted: x=0, y=0, buttons=0, at default,
    // has_pointer_history=false.
    return;
  }
  const CbLastPointerSnapshot& snap = mouse_dispatch_->last_pointer();
  out.x = snap.x;
  out.y = snap.y;
  out.buttons_blink = snap.buttons_blink;
  out.last_motion_at = snap.at;
  // Default-constructed TimeTicks compares as "no events yet" — M4
  // R3 leaves `at` default until the first successful forward.
  out.has_pointer_history = !snap.at.is_null();
}

}  // namespace cloud_browser

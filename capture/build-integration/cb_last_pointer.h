// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbLastPointer — M4 R10 canonical "where did the pointer last go"
// state holder. Provides:
//   * the CbLastPointerSnapshot value type that M4 R3
//     (CbInputDispatchMouse) writes on every successful mouse_*
//     forward and that M5 R3 (CbCursorXyJoin) reads on every cursor-
//     change edge;
//   * a CbLastPointerState class that owns one snapshot, exposes a
//     const accessor, and offers Update / MarkLeft / MarkEntered
//     mutators so the writers (M4 R3 mouse path, and any future
//     leave/enter envelope source) all touch the field through the
//     same seam;
//   * a tiny envelope-shape helper (HandlePointerLeaveEnvelope) so
//     M4 R3's composite delegate can route a `mouse_leave` envelope
//     into the state holder without re-implementing the field set.
//
// R10 scope (CV2-50):
//   * the canonical CbLastPointerSnapshot definition (M4 R3 declared
//     a sibling-shaped stub on its own branch; merge-time fixup
//     described below);
//   * the leave-disposition decision: when the controlling client
//     reports the cursor has left the streamed-WebContents region,
//     KEEP the last in-widget coordinates but flip in_widget=false
//     so consumers can choose to fade / hide / mark-stale instead of
//     painting at the boundary. This matches the M5 design memo's
//     "freeze, don't snap" preference for cursor egress;
//   * an entry counterpart (MarkEntered) so a future v1.1 client
//     that emits `mouse_enter` envelopes between drag-out / drag-in
//     gestures has a symmetric seam — the next Update() also sets
//     in_widget=true so MarkEntered() isn't strictly required, but
//     keeping both halves explicit lets consumers treat re-entry as
//     a separate edge from move.
//
// Non-goals for R10:
//   * deciding WHEN the cursor has left the streamed-WebContents.
//     Two equally-valid sources are anticipated:
//       a) client-side `mouse_leave` envelope from the input bridge
//          (a hint that the operator's cursor moved out of the
//          streamed area on their screen);
//       b) server-side aura WindowDelegate::OnMouseExited / a
//          RenderWidgetHost observer on widget-bounds crossings.
//     R10 only provides the seam; M4 R3 + the composite delegate (or
//     a future R8) owns the actual envelope routing decision. The
//     envelope-shape helper below handles (a); (b) is deferred.
//   * persistence across renderer / WebContents swaps. The state
//     holder is per-CbInputDispatchMouse-instance, which is itself
//     per-FSVC / per-controlled-WebContents. When the controlled
//     WebContents flips, M4 R2 (the resolver) is expected to recreate
//     the dispatch path; the snapshot is implicitly reset.
//
// Lifetime / threading:
//   * Plain C++ object, no chromium thread affinity. The expected
//     caller is M4 R3 on BrowserThread::UI; M5 R3 reads the snapshot
//     on the same thread. R10 does not introduce locking — if a
//     future cross-thread reader appears (unlikely; SetCursor and
//     mouse dispatch both live on BrowserThread::UI), it MUST add
//     synchronization at THAT seam, not retroactively here.
//
// Merge picture (when this R10 branch and the M4 R3 branch both land
// on integration/native-peer):
//   1. M4 R3 currently declares its own CbLastPointerSnapshot struct
//      in cb_input_dispatch_mouse.h. At merge time:
//      * remove that struct definition from cb_input_dispatch_mouse.h;
//      * `#include "cloud-browser/capture/build-integration/
//        cb_last_pointer.h"` instead;
//      * replace M4 R3's `CbLastPointerSnapshot last_pointer_;` field
//        with `CbLastPointerState last_pointer_state_;`;
//      * replace M4 R3's three direct `last_pointer_.{x,y,...}
//        assignments` (one per Dispatch*) with one
//        `last_pointer_state_.Update(widget.x, widget.y,
//        held_buttons_blink_, event_time);`
//      * change M4 R3's `const CbLastPointerSnapshot& last_pointer()
//        const` accessor to `return last_pointer_state_.last_pointer();`
//        so M5 R3's `mouse_dispatch_->last_pointer()` call site is
//        untouched.
//   2. The composite delegate that R8 (or a future M4 wiring rev)
//      assembles routes a `mouse_leave` envelope through
//      HandlePointerLeaveEnvelope(data, &last_pointer_state_).
//
// No other M4 / M5 consumer touches the snapshot field set, so this
// is a self-contained refactor.

#ifndef CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_LAST_POINTER_H_
#define CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_LAST_POINTER_H_

#include <cstdint>

#include "base/time/time.h"
#include "base/values.h"

namespace cloud_browser {

// Snapshot of the last-forwarded pointer event. Consumed by M5 R3
// (cursor x,y join) on cursor-change edges and reserved for future
// readers (e.g. a cursor egress overlay that paints the remote
// pointer position on the captured frame).
//
// Field semantics:
//   x, y          — widget-space DIPs (post device-scale-factor
//                   conversion), matching the units chromium's
//                   compositor uses so a paint consumer doesn't need
//                   to re-apply DSF.
//   buttons_blink — blink::WebInputEvent::Modifiers-format button-down
//                   bits (kLeftButtonDown / kRightButtonDown /
//                   kMiddleButtonDown / kBackButtonDown /
//                   kForwardButtonDown). NOT the protocol bitmask.
//   at            — monotonic timestamp of the last successful
//                   forward. Default-constructed = "no events
//                   forwarded yet". Consumers SHOULD test
//                   `!at.is_null()` before treating coords as
//                   meaningful (M5 R3 currently uses exactly that
//                   predicate to set its `has_pointer_history` bit).
//   in_widget     — R10 addition. True iff the controlling client's
//                   cursor is currently understood to be inside the
//                   streamed WebContents' viewport. Defaults true so
//                   that the snapshot's "no events yet" state
//                   (`at.is_null()` with `in_widget=true`) is
//                   distinguishable from a post-leave state
//                   (`at.is_set()` with `in_widget=false`). Consumers
//                   that want a single bit may compute
//                   `has_pointer_history && in_widget`.
struct CbLastPointerSnapshot {
  float x = 0.f;
  float y = 0.f;
  uint32_t buttons_blink = 0;
  base::TimeTicks at;
  bool in_widget = true;
};

// CbLastPointerState — owns one CbLastPointerSnapshot, mediates
// updates, and exposes a const accessor.
//
// Why this class instead of a bare struct field on the dispatcher:
// the leave-disposition semantic ("keep coords, flip in_widget")
// is fiddly enough that codifying it in one place beats sprinkling
// `last_pointer_.in_widget = false;` across multiple call sites.
// The class is also small enough that the indirection cost is nil.
class CbLastPointerState {
 public:
  CbLastPointerState();
  ~CbLastPointerState();

  CbLastPointerState(const CbLastPointerState&) = delete;
  CbLastPointerState& operator=(const CbLastPointerState&) = delete;

  // Accessor read by M5 R3 and any future consumer. Stable reference
  // for the lifetime of `this`; the returned snapshot mutates in
  // place under Update / MarkLeft / MarkEntered calls so a consumer
  // that captures `const auto& snap = state.last_pointer();` sees
  // subsequent writes — caller should re-read on every join, not
  // cache field values.
  const CbLastPointerSnapshot& last_pointer() const { return snap_; }

  // Called by M4 R3 on every successful ForwardMouseEvent /
  // ForwardWheelEvent. Sets x / y / buttons_blink / at and also sets
  // in_widget=true — a successful forward to chromium intrinsically
  // means the pointer is "in" the widget at this moment, regardless
  // of any prior MarkLeft() state. This matches the leave-then-move
  // recovery path: if a client sent `mouse_leave` and the next
  // envelope is a `mouse_move`, the move re-establishes presence.
  void Update(float widget_x,
              float widget_y,
              uint32_t buttons_blink,
              base::TimeTicks at);

  // Called when the controlling client reports the cursor has left
  // the streamed-WebContents region. Keeps x / y / buttons_blink /
  // at unchanged so a consumer can freeze a displayed cursor on the
  // last in-widget position. Flips in_widget=false so the same
  // consumer can distinguish "still inside, last reading was a
  // moment ago" from "the operator has moved away, this position is
  // stale".
  //
  // No-op if in_widget is already false — the contract is
  // idempotent so a redundant leave envelope (or a leave following a
  // forward-resolution failure that already implicitly invalidated
  // the snapshot) doesn't churn observers.
  void MarkLeft();

  // Symmetric counterpart. Flips in_widget back to true without
  // touching the coords or timestamp — useful for a v1.1 client
  // that wants to signal re-entry before sending the first
  // post-entry mouse_move. The next Update() also sets
  // in_widget=true so the explicit MarkEntered seam is optional.
  void MarkEntered();

 private:
  CbLastPointerSnapshot snap_;
};

// Envelope-shape helper for the client-side leave signal. Decodes a
// `mouse_leave` envelope's data dict and dispatches into `state`.
//
// Protocol shape (v1.0 — see docs/protocols/input-channel.md, to be
// extended by R10):
//   { "type": "mouse_leave" }
// No required data fields. A future revision may carry the boundary
// coords ({"x": int, "y": int}) so the snapshot can be UPDATED to
// the exit point before being flipped in_widget=false; the helper
// reads those fields if present but tolerates their absence
// (current behaviour: leave snapshot coords at whatever the last
// successful forward set, then flip in_widget).
//
// Returns true on success (any leave envelope that didn't explode
// is "successful" — there's no malformed-leave to reject), false if
// state is null (defensive — caller bug). The bool is provided so
// the composite delegate can chain dispatchers without an extra
// std::optional layer.
//
// TODO(M4-R10-leave-envelope-coords): once the client side adopts
// the v1.1 "carry the boundary coords" extension, switch the helper
// to call state->Update(...) for the coord half BEFORE flipping
// in_widget, so consumers that paint the cursor see it slide to the
// boundary then disappear instead of freezing in place mid-widget.
bool HandlePointerLeaveEnvelope(const base::DictValue& data,
                                CbLastPointerState* state);

// Symmetric helper for a v1.1 `mouse_enter` envelope. Same return
// semantics as HandlePointerLeaveEnvelope. Currently a thin wrapper
// around state->MarkEntered() — exists so the composite delegate can
// route `mouse_enter` through a uniform Handle* helper shape.
bool HandlePointerEnterEnvelope(const base::DictValue& data,
                                CbLastPointerState* state);

}  // namespace cloud_browser

#endif  // CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_LAST_POINTER_H_

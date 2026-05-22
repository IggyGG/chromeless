// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbCursorXyJoin — M5 R3 coordinator that joins M5 R1's cursor-type
// stream (kPointer / kHand / kIBeam / kWait / kCustom / …) with M4 R3's
// last-dispatched pointer coordinates into a single JoinedCursorState
// envelope that M5 R2 then formats + emits on the cursor DataChannel.
//
// Why R3 exists (CV2-21):
//   chromium's aura::client::CursorClient::SetCursor signature is
//   `void SetCursor(gfx::NativeCursor)` — it carries the cursor TYPE
//   but NOT the cursor POSITION. The position the cursor was just
//   moved to is, by construction, the pointer position chromium last
//   processed; the renderer's `mouseover` -> `pointerover` ->
//   `EventDispatcher::EnsureMouseLocationAndModifiers` chain feeds
//   that position into the same SetCursor call.
//
//   On the cb-chromium native path we control where the pointer
//   went last — M4 R3 (CbInputDispatchMouse) is the only writer of
//   mouse moves into chromium, and it publishes a
//   `CbLastPointerSnapshot { x, y, buttons_blink, at }` for exactly
//   this consumer. R3's job is to read that snapshot at SetCursor
//   time, package it with the type/visible bits from R1's callback,
//   and hand the joined record to R2.
//
//   This avoids the alternative — querying the gfx::Screen or
//   running a separate ui::EventSink observer purely to recover
//   cursor coords — which would either pull in display-server
//   dependencies the headless cb-chromium pod doesn't have, or
//   duplicate the per-event coord state M4 R3 is already maintaining
//   correctly. One source of truth for pointer position; R3 just
//   joins it with the type stream that lives on a different chain.
//
// Position-source guarantees / sharp edges:
//   * The snapshot M4 R3 publishes is the LAST SUCCESSFULLY FORWARDED
//     mouse event — a dispatch that failed resolution (no active
//     WebContents) leaves `at` untouched. R3 forwards has_pointer
//     history=false when `at` is default-constructed so R2 / the
//     wire consumer can decide whether to render at (0,0) or skip.
//   * Coordinates are widget-space DIPs (post-DSF). At the FSVC
//     1280x720 DSF=1 path the protocol-space coords are identical
//     to widget-space DIPs; on a future DSF!=1 path R2 (or the
//     cursor channel consumer) is responsible for converting back to
//     protocol-space if it cares — the snapshot semantics are
//     defined by M4 R3, not by R3, and the cleanest seam is to
//     forward what M4 R3 published.
//   * R1's callback fires on type changes AND visibility changes
//     (ShowCursor / HideCursor). R3 forwards both as joined records;
//     visibility transitions matter to the operator UI even when
//     the type didn't change.
//   * Click + drag does NOT produce a SetCursor on every move;
//     chromium calls SetCursor only when the renderer asks for a
//     different cursor. Position updates without a type change are
//     M5 R4's job (a separate mouse-move observer that fires every
//     N ms or every dispatched move), NOT R3's. R3 only joins on
//     cursor-change edges.
//
// Dependency posture / lifetime:
//   * Constructed with raw pointers to M5 R1's CbCursorClient and
//     M4 R3's CbInputDispatchMouse. Both are caller-owned and MUST
//     outlive this object. CbAuraPlatformData owns the cursor
//     client; M4 R2 + wiring eventually owns the mouse dispatch;
//     R3 sits between them with no ownership.
//   * R1's `CursorChangeCallback` accepts a single registered
//     callback. R3 SHOULD be the one and only registrant for the
//     cursor egress path; if a future R5 (custom-image cursor) or
//     unrelated consumer also wants the stream, R3 fans it out
//     internally rather than racing R1's single-slot setter.
//
// R2 / R3 contract:
//   R3 hands R2 a JoinedCursorState through a
//   `JoinedCursorCallback`. R2 owns the wire envelope shape, the DC
//   serialization, and the DC emission path. R3 does NOT touch the
//   DC, the envelope JSON, or any wire format.
//
// Non-goals for R3:
//   * Wire envelope formatting (R2, CV2-20).
//   * DataChannel emission (R2 / downstream, CV2-22).
//   * Custom-image cursor bytes (R5, CV2-24).
//   * Periodic position-only updates (R4 / a separate move observer).
//   * Cross-thread fan-out — R3 runs on BrowserThread::UI alongside
//     R1's callback and R3's snapshot accessor; the join is a single
//     synchronous read of an immutable-after-construction snapshot
//     field.

#ifndef CAPTURE_BUILD_INTEGRATION_CB_CURSOR_XY_JOIN_H_
#define CAPTURE_BUILD_INTEGRATION_CB_CURSOR_XY_JOIN_H_

#include <cstdint>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "ui/base/cursor/mojom/cursor_type.mojom-forward.h"

namespace cloud_browser {

class CbCursorClient;
class CbInputDispatchMouse;

// JoinedCursorState — the record R3 produces on every cursor-change
// edge, handed to R2 for envelope formatting + emission.
//
// Field semantics:
//   type                — ui::mojom::CursorType the renderer last
//                         asked for (kPointer / kHand / kIBeam / …).
//                         Mirrors the first arg of R1's
//                         CursorChangeCallback unmodified.
//   visible             — current cursor visibility, mirrors R1's
//                         second arg. Toggles independently of
//                         `type` in response to ShowCursor /
//                         HideCursor.
//   x, y                — widget-space DIPs of the last
//                         successfully-forwarded pointer event from
//                         M4 R3. See "Position-source guarantees"
//                         above for the DSF / protocol-space note.
//   buttons_blink       — blink::WebInputEvent::Modifiers-format
//                         button-down bits from the same snapshot
//                         (kLeftButtonDown / kRightButtonDown /
//                         kMiddleButtonDown / kBackButtonDown /
//                         kForwardButtonDown). R2 can use this to
//                         emit a "cursor over a draggable element
//                         while a button is held" hint; v1 envelope
//                         may ignore.
//   last_motion_at      — monotonic timestamp of the M4 R3 snapshot.
//                         Default-constructed iff
//                         has_pointer_history==false.
//   joined_at           — monotonic timestamp R3 stamped at join
//                         time. Lets R2 / downstream measure the
//                         join-to-emit latency separately from the
//                         move-to-cursor-change latency.
//   has_pointer_history — false iff M4 R3 has not yet successfully
//                         forwarded any pointer event (the snapshot
//                         is default-constructed). When false,
//                         x / y / buttons_blink / last_motion_at
//                         are all zero / default. R2 SHOULD choose
//                         between dropping the record and emitting
//                         a "type-only, no position" envelope —
//                         that's an envelope-shape decision, hence
//                         R2's call.
struct JoinedCursorState {
  ui::mojom::CursorType type{};
  bool visible = true;
  float x = 0.f;
  float y = 0.f;
  uint32_t buttons_blink = 0;
  base::TimeTicks last_motion_at;
  base::TimeTicks joined_at;
  bool has_pointer_history = false;
};

// Callback R2 (or any future consumer) registers to receive every
// joined cursor record. Fired synchronously on BrowserThread::UI
// from inside R1's CursorChangeCallback dispatch — R2's handler
// MUST NOT block (post to a worker thread if heavy work is
// required, e.g. JSON serialization beyond trivial cost).
using JoinedCursorCallback =
    base::RepeatingCallback<void(const JoinedCursorState& joined)>;

class CbCursorXyJoin {
 public:
  // |cursor_client| — M5 R1's CbCursorClient. R3 registers its own
  //                   CursorChangeCallback on this in
  //                   Initialize(); caller MUST NOT also register
  //                   one (single-slot per R1's contract).
  // |mouse_dispatch| — M4 R3's CbInputDispatchMouse. R3 reads
  //                   `last_pointer()` on every join; may be null
  //                   during M4 wiring bring-up, in which case
  //                   every join carries has_pointer_history=false
  //                   and a WARNING is logged once.
  //
  // Lifetime: both pointers are caller-owned and MUST outlive this
  // object. Pre-condition: cursor_client must not be null.
  CbCursorXyJoin(CbCursorClient* cursor_client,
                 CbInputDispatchMouse* mouse_dispatch);

  CbCursorXyJoin(const CbCursorXyJoin&) = delete;
  CbCursorXyJoin& operator=(const CbCursorXyJoin&) = delete;

  ~CbCursorXyJoin();

  // Wires R3's CursorChangeCallback onto |cursor_client|. Separate
  // from the ctor so the platform-data class can construct R3
  // before deciding to register it (mirrors how R1's hook is
  // installed by CbAuraPlatformData after its own ctor body runs).
  // Idempotent: calling twice replaces the registration with itself
  // (no observable change, no leaked callback).
  void Initialize();

  // R2's hook. Pass an empty callback to detach. Replacing a
  // previously-set callback is allowed; the new callback receives
  // every subsequent join and the old one stops receiving anything.
  // Single-slot by design — if a future consumer needs to fan out,
  // it should sit in front of this seam, not behind it.
  void SetEmitCallback(JoinedCursorCallback callback);

  // Test seam. Invoked by cb_cursor_xy_join_test.cc to drive
  // OnCursorChange directly without standing up a real
  // CbCursorClient + aura window stack. Production code MUST NOT
  // call this — production drives the join through the registered
  // CursorChangeCallback path.
  void OnCursorChangeForTesting(ui::mojom::CursorType type, bool visible);

 private:
  // R1 CursorChangeCallback handler. Reads M4 R3's snapshot,
  // assembles a JoinedCursorState, fires `emit_callback_` if set.
  // No-op (apart from a once-per-instance WARNING log on the first
  // miss) if `emit_callback_` is empty — R3 SHOULD be initialized
  // before R2 wires up, but order of bring-up across modules isn't
  // guaranteed.
  void OnCursorChange(ui::mojom::CursorType type, bool visible);

  // Reads M4 R3's snapshot and fills the position-related fields of
  // |out|. Sets has_pointer_history to mirror snapshot.at !=
  // TimeTicks(). If `mouse_dispatch_` is null, leaves position
  // fields at their defaults and has_pointer_history=false.
  void FillPositionFromSnapshot(JoinedCursorState& out) const;

  // M5 R1 cursor source. Caller-owned, must outlive `this`. Never
  // null (enforced by ctor CHECK).
  const raw_ptr<CbCursorClient> cursor_client_;

  // M4 R3 pointer source. Caller-owned. May be null during M4
  // wiring bring-up — every join then carries
  // has_pointer_history=false.
  //
  // TODO(M5-R3-mouse-dispatch-required): once M4 R3 + wiring lands,
  // tighten this to non-null and drop the once-per-instance
  // "mouse_dispatch is null" warning path. Aim is for this to be a
  // CHECK at construction time once both modules are in.
  const raw_ptr<CbInputDispatchMouse> mouse_dispatch_;

  // R2's downstream sink. Empty until SetEmitCallback() is called.
  JoinedCursorCallback emit_callback_;

  // Tracks whether we've logged the "no emit callback registered"
  // and "mouse_dispatch is null" warnings so we don't spam.
  bool warned_no_emit_callback_ = false;
  bool warned_no_mouse_dispatch_ = false;

  // Tracks whether Initialize() has been called so we don't re-
  // register on each call (R1's contract allows the replacement,
  // but a self-replace is wasted work and clouds tracing).
  bool initialized_ = false;
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CB_CURSOR_XY_JOIN_H_

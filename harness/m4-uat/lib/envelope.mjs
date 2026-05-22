// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// harness/m4-uat/lib/envelope.mjs — v1 input envelope builder.
//
// Single source of truth: docs/protocols/input-channel.md.
// Mirrors the C++ InputEnvelope struct in
// capture/build-integration/cb_input_dispatch.h (M4 R1 sink). When
// either side adds a new type, this builder needs the new constructor
// and the type tag in ENVELOPE_TYPES.
//
// TODO(M4-R9-envelope-schema-test): a unit test that round-trips every
// envelope through the C++ decoder once the M4 R1 sink builds. Today
// the contract is enforced by inspection — the field names in this
// file match the table in docs/protocols/input-channel.md byte-for-
// byte, and the C++ struct mirrors that same table.

// All v1 envelope types from input-channel.md, in spec order.
export const ENVELOPE_TYPES = Object.freeze({
  // R3 — mouse + wheel
  MOUSE_MOVE: 'mouse_move',
  MOUSE_BUTTON: 'mouse_button',
  MOUSE_WHEEL: 'mouse_wheel',
  // R4 — keyboard
  KEY_DOWN: 'key_down',
  KEY_UP: 'key_up',
  // R5 — IME composition
  COMPOSITION_START: 'composition_start',
  COMPOSITION_UPDATE: 'composition_update',
  COMPOSITION_END: 'composition_end',
  COMPOSITION_CANCEL: 'composition_cancel',
  // R8 — clipboard (paste is bridge→native; copy_request is local→bridge)
  CLIPBOARD_PASTE: 'clipboard_paste',
  CLIPBOARD_COPY_REQUEST: 'clipboard_copy_request',
  // R7 — drag-and-drop
  DRAG_START: 'drag_start',
  DRAG_OVER: 'drag_over',
  DROP: 'drop',
  DRAG_END: 'drag_end',
  // R6 — touch
  TOUCH_START: 'touch_start',
  TOUCH_MOVE: 'touch_move',
  TOUCH_END: 'touch_end',
  TOUCH_CANCEL: 'touch_cancel',
  // R10 — pointer-leave (v1.1, no spec entry yet; harness picks a name
  // that the M4 R10 last-pointer state holder will key on).
  // TODO(M4-R9-leave-envelope-spec): when the spec finalises the name,
  // update both this constant and the R10 state holder.
  MOUSE_LEAVE: 'mouse_leave',
});

// Module-level sequence counter so a single test session emits
// strictly-increasing seq values across all envelopes, matching the
// per-channel monotonicity the server expects.
let seqCounter = 0;

export function resetSeq() { seqCounter = 0; }

export function buildEnvelope(type, data = {}, { t, seq } = {}) {
  return {
    v: 1,
    type,
    t: typeof t === 'number' ? t : Date.now(),
    seq: typeof seq === 'number' ? seq : seqCounter++,
    data,
  };
}

// Typed helpers — each one corresponds to one row in the spec table.
// Keeping them as discrete builders makes scenarios read straight
// across the spec.

export const env = Object.freeze({
  mouseMove({ x, y }) {
    return buildEnvelope(ENVELOPE_TYPES.MOUSE_MOVE, { x, y });
  },

  mouseButton({ button, action, x, y }) {
    return buildEnvelope(ENVELOPE_TYPES.MOUSE_BUTTON, { button, action, x, y });
  },

  mouseWheel({ dx, dy, mode = 0, delta_mode = 'pixel', phase = null,
               momentum = false, x, y }) {
    return buildEnvelope(ENVELOPE_TYPES.MOUSE_WHEEL,
      { dx, dy, mode, delta_mode, phase, momentum, x, y });
  },

  keyDown({ code, key, mods = 0 }) {
    return buildEnvelope(ENVELOPE_TYPES.KEY_DOWN, { code, key, mods });
  },

  keyUp({ code, key, mods = 0 }) {
    return buildEnvelope(ENVELOPE_TYPES.KEY_UP, { code, key, mods });
  },

  compositionStart({ data = '', rect = null } = {}) {
    const payload = { data };
    if (rect) payload.rect = rect;
    return buildEnvelope(ENVELOPE_TYPES.COMPOSITION_START, payload);
  },

  compositionUpdate({ data, selection_start, selection_end, candidate_list }) {
    const payload = { data };
    if (typeof selection_start === 'number') payload.selection_start = selection_start;
    if (typeof selection_end === 'number') payload.selection_end = selection_end;
    if (Array.isArray(candidate_list)) payload.candidate_list = candidate_list;
    return buildEnvelope(ENVELOPE_TYPES.COMPOSITION_UPDATE, payload);
  },

  compositionEnd({ data }) {
    return buildEnvelope(ENVELOPE_TYPES.COMPOSITION_END, { data });
  },

  compositionCancel() {
    return buildEnvelope(ENVELOPE_TYPES.COMPOSITION_CANCEL, {});
  },

  clipboardCopyRequest() {
    return buildEnvelope(ENVELOPE_TYPES.CLIPBOARD_COPY_REQUEST, {});
  },

  clipboardPaste({ text }) {
    return buildEnvelope(ENVELOPE_TYPES.CLIPBOARD_PASTE, { text });
  },

  dragStart({ x, y, types, items }) {
    return buildEnvelope(ENVELOPE_TYPES.DRAG_START, { x, y, types, items });
  },

  dragOver({ x, y }) {
    return buildEnvelope(ENVELOPE_TYPES.DRAG_OVER, { x, y });
  },

  drop({ x, y, types, items }) {
    return buildEnvelope(ENVELOPE_TYPES.DROP, { x, y, types, items });
  },

  dragEnd({ success }) {
    return buildEnvelope(ENVELOPE_TYPES.DRAG_END, { success });
  },

  touchStart({ identifier, x, y, radius_x = 1, radius_y = 1, force = 0, twist = 0 }) {
    return buildEnvelope(ENVELOPE_TYPES.TOUCH_START,
      { identifier, x, y, radius_x, radius_y, force, twist });
  },

  touchMove({ identifier, x, y, radius_x = 1, radius_y = 1, force = 0, twist = 0 }) {
    return buildEnvelope(ENVELOPE_TYPES.TOUCH_MOVE,
      { identifier, x, y, radius_x, radius_y, force, twist });
  },

  touchEnd({ identifier }) {
    return buildEnvelope(ENVELOPE_TYPES.TOUCH_END, { identifier });
  },

  touchCancel({ identifier }) {
    return buildEnvelope(ENVELOPE_TYPES.TOUCH_CANCEL, { identifier });
  },

  // R10 — mouse_leave: not in the v1.1 spec yet but the M4 R10 last-
  // pointer state holder is keyed on it. Until the spec finalises,
  // we send an empty {} payload — the C++ side reads through
  // HandlePointerLeaveEnvelope and ignores all fields.
  mouseLeave() {
    return buildEnvelope(ENVELOPE_TYPES.MOUSE_LEAVE, {});
  },
});

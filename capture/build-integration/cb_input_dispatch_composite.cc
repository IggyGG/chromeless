// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cloud-browser/capture/build-integration/cb_input_dispatch_composite.h"

#include <optional>
#include <string>
#include <utility>

#include "base/logging.h"

namespace cloud_browser {

namespace {

// Deep-clone an InputEnvelope. base::DictValue (= base::Value::Dict) is
// move-only by chromium policy; we materialise a per-dispatcher copy by
// calling Clone() on the dict + plain-copying the scalar fields. Used
// by the fan-out in OnInputEvent — see the header doc "Envelope
// cloning" for the lesson-(g) rationale.
//
// Cost: a single Clone() call per envelope per dispatcher (except the
// last, which receives the moved original). The dict is small in
// production (single-event payload of a handful of scalar fields), so
// this is microseconds-per-dispatch overhead. The
// TODO(M4-R1-typed-handlers) interface refactor eliminates the
// fan-out + the clones in one pass.
InputEnvelope CloneEnvelope(const InputEnvelope& src) {
  InputEnvelope out;
  out.version = src.version;
  out.type = src.type;
  out.t = src.t;
  out.seq = src.seq;
  out.data = src.data.Clone();
  return out;
}

}  // namespace

CbInputDispatchCompositeDelegate::CbInputDispatchCompositeDelegate(
    WebContentsResolver* resolver)
    : mouse_(std::make_unique<CbInputDispatchMouse>(resolver)),
      keyboard_(std::make_unique<CbInputDispatchKeyboard>(resolver,
                                                          &shared_modifiers_)),
      ime_(std::make_unique<CbInputDispatchIme>(resolver)),
      touch_(std::make_unique<CbInputDispatchTouch>(resolver)),
      // R7 (drag) takes a pointer to R3 (mouse) to read
      // held_modifiers_blink() — see header doc on the ownership chain
      // + TODO(M4-R7-shared-modifier-extraction) for the future
      // consolidation that will let R7 read directly from
      // shared_modifiers_ instead.
      drag_(std::make_unique<CbInputDispatchDrag>(resolver, mouse_.get())),
      clipboard_(std::make_unique<CbInputDispatchClipboard>(
          resolver,
          &shared_modifiers_)) {
  LOG(INFO) << "CV2-81: CbInputDispatchCompositeDelegate ctor — 6 typed "
               "dispatchers (R3 mouse, R4 keyboard, R5 IME, R6 touch, "
               "R7 drag, R8 clipboard) wired";
}

CbInputDispatchCompositeDelegate::~CbInputDispatchCompositeDelegate() {
  // unique_ptr field-destruction order is reverse-declaration: clipboard,
  // drag, touch, ime, keyboard, mouse. R7's raw pointer to R3 is read
  // only on OnInputEvent — once the composite is in its destructor we
  // are off the input DC observer chain (main_parts UnregisterObserver
  // ran first) so no dispatch can race the teardown.
  LOG(INFO) << "CV2-81: CbInputDispatchCompositeDelegate dtor";
}

// Protocol modifier bits (docs/protocols/input-channel.md): SHIFT=1 CTRL=2
// ALT=4 META=8. Meta counts because a macOS viewer copies with Cmd+C and the
// client forwards the modifier it saw.
bool CbInputDispatchCompositeDelegate::IsCopyGesture(
    const InputEnvelope& envelope) {
  if (envelope.type == "clipboard_copy_request") {
    return true;
  }
  if (envelope.type != "key_down") {
    return false;
  }
  const std::string* code = envelope.data.FindString("code");
  const std::optional<int> mods = envelope.data.FindInt("mods");
  return code && *code == "KeyC" && mods && ((*mods & 2) || (*mods & 8));
}

void CbInputDispatchCompositeDelegate::OnInputEvent(InputEnvelope envelope) {
  if (on_copy_gesture_ && IsCopyGesture(envelope)) {
    on_copy_gesture_.Run();
  }
  // Each typed dispatcher's OnInputEvent type-switches on envelope.type
  // and silently drops types it doesn't own. The dispatcher set is
  // mutually exclusive on envelope.type, so this fan-out produces at
  // most one chromium-side dispatch per envelope.
  //
  // InputEnvelope's `data` field is non-copyable (base::Value::Dict
  // deletes its copy ctor by chromium policy — see header doc
  // "Envelope cloning"). We materialise a fresh clone for each of the
  // first five dispatchers and std::move the original into the last
  // one (clipboard), reducing six copies to five Clone() + one move.
  if (mouse_) {
    mouse_->OnInputEvent(CloneEnvelope(envelope));
  }
  if (keyboard_) {
    keyboard_->OnInputEvent(CloneEnvelope(envelope));
  }
  if (ime_) {
    ime_->OnInputEvent(CloneEnvelope(envelope));
  }
  if (touch_) {
    touch_->OnInputEvent(CloneEnvelope(envelope));
  }
  if (drag_) {
    drag_->OnInputEvent(CloneEnvelope(envelope));
  }
  if (clipboard_) {
    clipboard_->OnInputEvent(std::move(envelope));
  }
}

void CbInputDispatchCompositeDelegate::OnInputEventDecodeError(
    const std::string& reason,
    const std::string& raw_payload_preview) {
  // The default implementation in CbInputDispatchDelegate is a no-op;
  // the typed dispatchers inherit that default today. Forwarding is
  // structurally complete + harmless; once any dispatcher overrides
  // this hook (e.g. for a per-type decode-error metric), the fan-out
  // is already in place. Strings are passed by const& so no copies.
  if (mouse_) {
    mouse_->OnInputEventDecodeError(reason, raw_payload_preview);
  }
  if (keyboard_) {
    keyboard_->OnInputEventDecodeError(reason, raw_payload_preview);
  }
  if (ime_) {
    ime_->OnInputEventDecodeError(reason, raw_payload_preview);
  }
  if (touch_) {
    touch_->OnInputEventDecodeError(reason, raw_payload_preview);
  }
  if (drag_) {
    drag_->OnInputEventDecodeError(reason, raw_payload_preview);
  }
  if (clipboard_) {
    clipboard_->OnInputEventDecodeError(reason, raw_payload_preview);
  }
}

void CbInputDispatchCompositeDelegate::OnInputEventUnknownType(
    const std::string& type,
    int64_t seq) {
  // See OnInputEventDecodeError above for the rationale on fan-out
  // despite no-op defaults.
  if (mouse_) {
    mouse_->OnInputEventUnknownType(type, seq);
  }
  if (keyboard_) {
    keyboard_->OnInputEventUnknownType(type, seq);
  }
  if (ime_) {
    ime_->OnInputEventUnknownType(type, seq);
  }
  if (touch_) {
    touch_->OnInputEventUnknownType(type, seq);
  }
  if (drag_) {
    drag_->OnInputEventUnknownType(type, seq);
  }
  if (clipboard_) {
    clipboard_->OnInputEventUnknownType(type, seq);
  }
}

}  // namespace cloud_browser

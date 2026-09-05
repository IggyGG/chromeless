// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbInputDispatchCompositeDelegate — M4 (CV2-81) runtime-wire glue that
// fans CbInputDispatchDelegate::OnInputEvent out to the six typed M4
// dispatchers (R3 mouse, R4 keyboard, R5 IME, R6 touch, R7 drag, R8
// clipboard). Replaces the R1 CbInputLoggingDelegate stand-in as the
// observer the M4 R1 CbInputDispatch ships envelopes to.
//
// Why this exists
//
//   * M4 R1's CbInputDispatchDelegate has a single OnInputEvent
//     virtual (cb_input_dispatch.h:99). Each typed dispatcher R3..R8
//     implements OnInputEvent as a type-switch — it acts on the
//     envelope types it owns and silently drops the rest (per each
//     dispatcher's TODO(M4-R{n}-composite-delegate) marker). The
//     composite delegate is the canonical place to fan one envelope
//     to all six dispatchers.
//
//   * CV2-75 R1 wired CbInputLoggingDelegate into main_parts to keep
//     the M4 R1 plumbing exercisable end-to-end before the typed
//     dispatchers landed. R2..R10 then landed as deadweight source —
//     compiled into the source_sets but never instantiated. CV2-81
//     swaps the logger out for this composite, finally closing the
//     M4 R2..R10 -> main_parts wiring frontier.
//
//   * The class is transitional. The CbInputDispatchDelegate interface
//     today exposes a single OnInputEvent fanned across all envelope
//     types; when that interface gains per-type virtuals (see
//     TODO(M4-R1-typed-handlers) in cb_input_dispatch.h:93), each
//     dispatcher's type-switch collapses to a single OnX override and
//     this composite can be replaced by direct multi-observer fan-out.
//     Until then, the fan-out lives here.
//
// Shared state
//
//   * CbHeldModifierState — the four-named-modifier (Shift / Ctrl /
//     Alt / Meta) bitmask. Today R3 (mouse) owns its own held_modifiers_
//     blink_ member while R4 (keyboard) + R8 (clipboard) read a separate
//     CbHeldModifierState pointer; R7 (drag) reaches R3's accessor
//     directly. The composite owns ONE CbHeldModifierState instance
//     and injects it into R4 + R8. R3 and R7 keep their existing
//     paths until the TODO(M4-R7-shared-modifier-extraction) /
//     TODO(M4-R3-shared-modifier-extraction) consolidation lands.
//
//   * brought_to_front_ — R3 / R6 / R7 each maintain an INDEPENDENT
//     bring-to-front latch. On first dispatch per type, each
//     independently calls WebContents::WasShown() + Focus() on the
//     active WebContents. Up to 3 redundant activations may occur —
//     non-fatal, but Phase 2 will consolidate per
//     TODO(M4-R{3,6,7}-share-activation-latch) in each dispatcher.
//
// Envelope cloning (lesson-(g) finding from CV2-81 implementation)
//
//   * The base CbInputDispatchDelegate::OnInputEvent virtual takes
//     InputEnvelope by value. InputEnvelope contains a base::DictValue
//     (= base::Value::Dict) member whose copy constructor is DELETED
//     by chromium ("Value does not support C++ copy semantics to make
//     it harder to accidentally copy large values" — see base/values.h
//     class doc). The struct is therefore implicitly move-only, NOT
//     copyable.
//   * For the fan-out, we materialise per-dispatcher copies by deep-
//     cloning `data` via base::Value::Dict::Clone(); the scalar fields
//     (version, type, t, seq) are trivially copyable. The original
//     `envelope` is std::move'd into the LAST dispatcher in the fan-out
//     to skip one clone. Six dispatchers therefore produce five
//     Clone() calls + one move per OnInputEvent.
//   * Clone cost: base::Value::Dict::Clone() is a recursive deep copy.
//     Production envelopes are small (single-event dicts with a handful
//     of scalar fields), so the overhead is microseconds per dispatch.
//     The TODO(M4-R1-typed-handlers) interface refactor eliminates
//     both the fan-out AND the clones in one go (per-type virtuals
//     receive their decoded payload directly, never the envelope).
//
// Lifetime
//
//   * Owned by cloud_browser_browser_main_parts as a unique_ptr.
//     Composite owns the six typed dispatchers via unique_ptr; the
//     embedder constructs the composite once per capture session.
//   * The resolver pointer is caller-owned (main_parts owns the
//     resolver too); composite holds it raw. Resolver MUST outlive
//     the composite.
//   * Teardown: composite goes first (LIFO) before input_dispatch_
//     (which holds a raw pointer back to us as the delegate). See
//     main_parts.cc PostMainMessageLoopRun.

#ifndef CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_COMPOSITE_H_
#define CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_COMPOSITE_H_

#include <memory>
#include <string>
#include <utility>

#include "base/functional/callback.h"

#include "cloud-browser/capture/build-integration/cb_input_dispatch.h"
#include "cloud-browser/capture/build-integration/cb_input_dispatch_clipboard.h"
#include "cloud-browser/capture/build-integration/cb_input_dispatch_drag.h"
#include "cloud-browser/capture/build-integration/cb_input_dispatch_ime.h"
#include "cloud-browser/capture/build-integration/cb_input_dispatch_keyboard.h"
#include "cloud-browser/capture/build-integration/cb_input_dispatch_mouse.h"
#include "cloud-browser/capture/build-integration/cb_input_dispatch_touch.h"

namespace cloud_browser {

// Composite delegate that fans OnInputEvent to all 6 typed dispatchers
// (R3-R8).
//
// This class is transitional: it implements the single OnInputEvent
// virtual of CbInputDispatchDelegate by deep-cloning the envelope into
// each typed dispatcher's OnInputEvent (each dispatcher internally
// type-switches and ignores types it doesn't own). When
// CbInputDispatchDelegate adds per-type virtuals
// (TODO(M4-R1-typed-handlers) in cb_input_dispatch.h:93), replace each
// dispatcher's OnInputEvent type-switch with a single per-type
// override; this composite class can then be replaced by direct
// multi-observer fan-out and the clones disappear.
//
// R3 / R6 / R7 each maintain an independent brought_to_front_ latch.
// On first dispatch per type, each independently calls WasShown() +
// Focus() on the active WebContents. Up to 3 redundant activations
// may occur — non-fatal but a Phase 2 consolidation opportunity
// (per-session latch owned here or in the resolver).
class CbInputDispatchCompositeDelegate : public CbInputDispatchDelegate {
 public:
  // |resolver| is the M4 R2 active-WebContents resolver, owned by
  // main_parts. Composite holds it raw + threads it into all six
  // typed dispatchers' constructors. MUST outlive this object.
  explicit CbInputDispatchCompositeDelegate(WebContentsResolver* resolver);

  CbInputDispatchCompositeDelegate(const CbInputDispatchCompositeDelegate&) =
      delete;
  CbInputDispatchCompositeDelegate& operator=(
      const CbInputDispatchCompositeDelegate&) = delete;

  ~CbInputDispatchCompositeDelegate() override;

  // CbInputDispatchDelegate. Fans the envelope to each of the six
  // typed dispatchers sequentially. Each dispatcher type-switches on
  // envelope.type and silently drops types it doesn't own.
  //
  // The envelope's `data` field (base::DictValue) is non-copyable;
  // we materialise per-dispatcher copies via Clone() on the first
  // five dispatchers and std::move into the last one. See the header
  // doc above ("Envelope cloning") for the lesson-(g) finding.
  void OnInputEvent(InputEnvelope envelope) override;

  // Forward the decode-error + unknown-type hooks to each dispatcher
  // so they can surface metrics if they choose. Default implementations
  // in CbInputDispatchDelegate are no-ops; the typed dispatchers
  // inherit those defaults today, so this fan-out is currently
  // free of side effects but keeps the contract complete.
  void OnInputEventDecodeError(const std::string& reason,
                               const std::string& raw_payload_preview) override;
  void OnInputEventUnknownType(const std::string& type, int64_t seq) override;

  // Test seams — expose the typed dispatchers so unit tests can poke
  // at per-dispatcher state (e.g. CbInputDispatchMouse::last_pointer()
  // for R10's snapshot assertions) without re-instantiating the
  // composite. Never used by production code.
  CbInputDispatchMouse* mouse_dispatch() { return mouse_.get(); }
  CbInputDispatchMouse* mouse_for_testing() { return mouse_dispatch(); }
  const CbLastPointerState* last_pointer_state() const {
    return mouse_ ? mouse_->last_pointer_state() : nullptr;
  }
  CbInputDispatchKeyboard* keyboard_for_testing() { return keyboard_.get(); }
  CbInputDispatchIme* ime_for_testing() { return ime_.get(); }
  CbInputDispatchTouch* touch_for_testing() { return touch_.get(); }
  CbInputDispatchDrag* drag_for_testing() { return drag_.get(); }
  CbInputDispatchClipboard* clipboard_for_testing() { return clipboard_.get(); }

  // CV2-CLIPBOARD: notified on every viewer COPY gesture — a
  // `clipboard_copy_request` envelope, or a `key_down` of KeyC with Ctrl or
  // Meta held. CbClipboardRelay arms its forward window from this, which is
  // what lets it forward a copy the viewer asked for while ignoring clipboard
  // writes no one asked for. Detected here rather than inside the clipboard
  // and keyboard dispatchers because this is the one place every envelope
  // passes, and it keeps the two dispatchers unaware of the relay.
  void SetOnCopyGesture(base::RepeatingClosure cb) {
    on_copy_gesture_ = std::move(cb);
  }

 private:
  // True for the two envelope shapes a copy gesture takes on the wire.
  static bool IsCopyGesture(const InputEnvelope& envelope);
  base::RepeatingClosure on_copy_gesture_;

  // Shared modifier state injected into R4 (keyboard) + R8 (clipboard).
  // R3 still owns its own held_modifiers_blink_ member; R7 reads R3's
  // accessor. The TODO(M4-R7-shared-modifier-extraction) consolidation
  // will collapse R3's storage into this instance and update R7 to
  // read from here directly.
  //
  // Declared BEFORE the typed-dispatcher unique_ptr members so the
  // address &shared_modifiers_ is valid when those ctors run — C++
  // destruction is reverse-declaration order, so dispatchers are torn
  // down first and the shared state outlives them.
  CbHeldModifierState shared_modifiers_;

  // Typed dispatchers, constructed in dependency order (R3 first
  // because R7 needs a pointer to it). Destroyed in reverse order
  // by unique_ptr's LIFO field-destruction.
  std::unique_ptr<CbInputDispatchMouse> mouse_;
  std::unique_ptr<CbInputDispatchKeyboard> keyboard_;
  std::unique_ptr<CbInputDispatchIme> ime_;
  std::unique_ptr<CbInputDispatchTouch> touch_;
  std::unique_ptr<CbInputDispatchDrag> drag_;
  std::unique_ptr<CbInputDispatchClipboard> clipboard_;
};

}  // namespace cloud_browser

#endif  // CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_COMPOSITE_H_

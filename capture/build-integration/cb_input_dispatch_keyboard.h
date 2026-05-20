// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbInputDispatchKeyboard — M4 R4 typed delegate for key_down / key_up
// envelopes coming out of M4 R1's CbInputDispatch (after the signaling-
// thread → BrowserThread::UI hop).
//
// The keyboard path is the spec's "native equivalent" of the Go
// input-bridge/main.go CDP dispatcher, but driven through chromium's
// internal `RenderWidgetHost::ForwardKeyboardEvent` API with
// `input::NativeWebKeyboardEvent` instead of
// `Input.dispatchKeyEvent`. The two paths are intentionally
// semantically parity-aligned — the same envelope sequence MUST
// produce the same observable DOM event sequence (keydown / keypress /
// input / keyup) in either backend. See capture/input-bridge/main.go's
// Dispatch() `key_down`/`key_up` arm for the wire-format reference.
//
// R4 scope (CV2-44):
//   * key_down / key_up native dispatch via NativeWebKeyboardEvent
//   * cross-envelope shared modifier-state mutation (Shift/Ctrl/Alt/
//     Meta) — R4 OWNS the writes for the four named modifier keys;
//     mouse dispatch (R3) reads on every event to satisfy the
//     `key_down Shift / mouse_button down / mouse_button up / key_up
//     Shift` shift-click invariant
//   * text-synthesis: for printable single-codepoint `key` values
//     AND for the special keys whose textarea-insertion behaviour
//     depends on a non-empty text payload (Enter / Tab / Backspace),
//     a follow-on kChar NativeWebKeyboardEvent is dispatched after
//     the kRawKeyDown so the renderer fires `input` events and
//     inserts the character (matches the Go bridge's `params["text"]`
//     CDP payload semantics — without the kChar event chromium's
//     renderer treats e.g. Enter in a plain textarea as a navigation
//     keypress rather than character insertion)
//   * unicode / IME text synthesis: when `key` is multi-codepoint
//     (e.g. CJK pre-composed glyphs, emoji ZWJ sequences) the kChar
//     path still fires; clients sending true IME composition use the
//     composition_* envelopes which are M4 R5's territory, not R4's
//   * envelope-carried `mods` field OR'd into the held-mods bitmask
//     on the wire for the dispatched event so a client that
//     hand-rolls explicit modifier tracking can augment the shared
//     state without disturbing it
//
// Non-goals for R4:
//   * mouse / wheel (R3), IME composition_* (R5), clipboard_paste
//     (R6), drag (R7), touch (R8) — those are owned by their
//     respective ranks
//   * AutoRepeat handling — chromium's input pipeline handles the
//     repeat-state derivation from successive kRawKeyDown events on
//     the same windows_key_code. R4 passes through; R3-style state
//     for the keyboard side would be a v2 spec concern
//   * Dead-key / accent composition not driven by an explicit IME
//     envelope — those depend on the OS input method routing and
//     belong with M4 R5's composition path
//   * Synthesising `key_press` envelopes — the v1 protocol unifies
//     down/up only; the renderer derives keypress from the
//     down+char pair, which is what we already produce
//
// Dependency on M4 R2 (active streamed-WebContents resolver):
//   R4 dispatches to the WebContents the FSVC is currently capturing
//   from. R3's WebContentsResolver interface is the single source of
//   truth; R4 holds a pointer to the same interface and tolerates
//   null exactly like R3 (LOG(WARNING) per envelope so the operator
//   sees the gap pre-R2).
//
// Dependency on M4 R3 (mouse dispatch):
//   R3 currently OWNS the storage for the four named modifier bits
//   (`held_modifiers_blink_` on CbInputDispatchMouse). For the R4
//   draft we model the state as a tiny shared helper struct
//   (CbHeldModifierState) that R4 mutates via a pointer. At fold-in
//   the controller will:
//     (a) replace R3's inline `held_modifiers_blink_` member with a
//         pointer to the same shared CbHeldModifierState, and
//     (b) construct the shared instance once in
//         cloud_browser_browser_main_parts.cc and inject the same
//         pointer into both R3 and R4.
//   See TODO(M4-R4-shared-modifier-extraction). Until then R4 will
//   compile standalone against this header; the integration test
//   that exercises both paths simultaneously (Plane CV2 wiring
//   ticket) is what catches a missing fold-in.
//
// All public methods are invoked on BrowserThread::UI (enforced by
// M4 R1's PostTask). The class is NOT thread-safe — it mutates the
// shared modifier state on each key_down/key_up and chromium's
// keyboard injection API is itself UI-thread-only.

#ifndef CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_KEYBOARD_H_
#define CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_KEYBOARD_H_

#include <cstdint>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "cloud-browser/capture/build-integration/cb_input_dispatch.h"
#include "cloud-browser/capture/build-integration/cb_input_dispatch_mouse.h"

namespace content {
class RenderWidgetHost;
class WebContents;
}  // namespace content

namespace cloud_browser {

// Shared cross-rank modifier state. The four blink::WebInputEvent::
// Modifiers bits for Shift / Control / Alt / Meta, OR'd into every
// dispatched mouse and keyboard event so a Shift-held mouse click
// fires with the shift modifier even though the wire envelope for
// the click does not carry one.
//
// Lifetime: created once in cloud_browser_browser_main_parts.cc, the
// same instance is injected into both CbInputDispatchMouse (R3) and
// CbInputDispatchKeyboard (R4). Reads + writes happen exclusively on
// BrowserThread::UI; no locking required.
//
// TODO(M4-R4-shared-modifier-extraction): R3 was drafted with an
// inline `held_modifiers_blink_` member on CbInputDispatchMouse with
// a SetHeldModifiersBlink() setter. The fold-in to integration/
// native-peer should:
//   * keep CbHeldModifierState as the single source of truth here,
//   * delete the inline member + setter from CbInputDispatchMouse,
//     and replace with a `CbHeldModifierState* shared_modifiers_`
//     pointer that R3 reads in OR-into-modifiers code paths,
//   * inject the same pointer into both R3 and R4 from
//     cloud_browser_browser_main_parts.cc when M4 wiring lands.
// The "default-construct one of these here" path keeps the R4 draft
// unit-testable without R3 having been refactored yet.
struct CbHeldModifierState {
  // blink::WebInputEvent::Modifiers bits — kShiftKey / kControlKey /
  // kAltKey / kMetaKey. Zero = no modifiers held.
  uint32_t blink_modifiers = 0;
};

class CbInputDispatchKeyboard : public CbInputDispatchDelegate {
 public:
  // |resolver| supplies the active WebContents for each dispatch;
  // may be null during M4 R2 development (R4 will log + skip
  // dispatch). Lifetime: caller-owned, must outlive this object.
  //
  // |shared_modifiers| is the cross-rank modifier state shared with
  // R3 (mouse). MUST be non-null in production; constructed once in
  // cloud_browser_browser_main_parts.cc and shared. Lifetime:
  // caller-owned, must outlive this object.
  CbInputDispatchKeyboard(WebContentsResolver* resolver,
                          CbHeldModifierState* shared_modifiers);

  CbInputDispatchKeyboard(const CbInputDispatchKeyboard&) = delete;
  CbInputDispatchKeyboard& operator=(const CbInputDispatchKeyboard&) = delete;

  ~CbInputDispatchKeyboard() override;

  // CbInputDispatchDelegate. R4 only acts on key_down / key_up
  // envelopes; unknown / non-keyboard types are silently dropped
  // here (the composite delegate that M4 wiring assembles will
  // route them to R3 / R5 / R6 / R7 / R8 instead).
  //
  // TODO(M4-R4-composite-delegate): once R8 lands the composite
  // delegate, swap this from "filter and ignore" to a pre-typed
  // method (OnKeyDown / OnKeyUp) so non-keyboard envelopes never
  // reach this class.
  void OnInputEvent(InputEnvelope envelope) override;

  // Direct accessor for tests + diagnostics. Returns the current
  // blink-modifiers bitmask the shared state reports.
  uint32_t held_modifiers_blink() const {
    return shared_modifiers_ ? shared_modifiers_->blink_modifiers : 0;
  }

 private:
  // Per-type dispatch handler. `is_down` toggles the kRawKeyDown vs
  // kKeyUp event type; both share the modifier-state mutation path
  // (down sets, up clears) and the text-synthesis branch (kChar
  // fires only after kRawKeyDown).
  void DispatchKey(const base::DictValue& data,
                   bool is_down,
                   base::TimeTicks event_time);

  // Maps the v1 `key` + `code` payload onto one of the four named
  // modifier blink bits — kShiftKey / kControlKey / kAltKey /
  // kMetaKey — or 0 for any other key. Matches the Go bridge's
  // keyToProtocolMod(), accepting either the KeyboardEvent.key form
  // ("Shift") or the .code form ("ShiftLeft") because clients vary
  // in which they populate.
  static uint32_t KeyToBlinkModifier(const std::string& key,
                                     const std::string& code);

  // Maps a v1 protocol modifier bitmask (Shift=1 / Ctrl=2 / Alt=4 /
  // Meta=8 — see input-bridge/main.go modShift etc.) onto the
  // equivalent blink::WebInputEvent::Modifiers bits. Used so a
  // client-supplied envelope-level `mods` field can augment the
  // shared cross-rank state without overwriting it.
  static uint32_t ProtocolModsToBlink(int protocol_mods);

  // Maps a v1 `code` payload (KeyboardEvent.code — "KeyA", "Enter",
  // "ArrowLeft", …) onto the corresponding chromium ui::DomCode +
  // dom_key_code + windows_key_code triple needed to populate a
  // NativeWebKeyboardEvent. Returns true on success.
  //
  // R4 ships this table in the .cc; it covers the printable ASCII
  // keys + Enter / Tab / Backspace / Arrow* / Home / End / PageUp /
  // PageDown / Function keys / Escape / Delete / Insert. Unknown
  // codes return false and the dispatcher falls back to a pure
  // text-synthesis path (kChar only, no kRawKeyDown) so a client
  // sending exotic codes still produces a visible character.
  //
  // TODO(M4-R4-scancode-table-complete): the v1 client today only
  // emits a subset of KeyboardEvent.code values; verify the table
  // matches the test_harness/streamer.html keyboard-event reporter
  // before fold-in. Anything missing logs a single VLOG(1) so
  // operators can grep for "CbInputDispatchKeyboard: unmapped code="
  // and extend the table in-tree.
  struct ScancodeMapping {
    int windows_key_code = 0;
    int dom_code = 0;
    int dom_key = 0;
  };
  static bool MapCodeToScancode(const std::string& code,
                                ScancodeMapping* out);

  // Returns the character-insertion text for a `key_down` whose
  // renderer-side effect depends on a non-empty text payload.
  // Covers the printable single-codepoint case (the renderer infers
  // text from windows_key_code for these, but kChar with the
  // explicit text matches the Go bridge's parity guarantee) AND the
  // three special keys whose textarea insertion needs a CR / TAB /
  // BS even though they have a windows_key_code. Returns empty
  // string for navigation keys (Arrow*, Home, End, PageUp, PageDown,
  // F1–F12) so those do NOT trigger a kChar follow-on.
  static std::string SynthesizedTextFor(const std::string& key);

  // M4 R2 resolver. Caller-owned; can be null pre-R2.
  // CV2-81 first-compile-link fix-forward iteration 2 (lesson-(g.3-meta)
  // Completeness-of-class-sweep applied): this pre-existing file was
  // pulled into the cloud_browser_worker target transitively via
  // composite.h → clipboard.h → keyboard.h; iteration 1's grep-sweep
  // was narrow-scoped to error-visible files and missed this file.
  // Full-glob sweep on iteration 2 caught it.
  const raw_ptr<WebContentsResolver> resolver_;

  // Cross-rank modifier state shared with R3. Caller-owned; can be
  // null only in unit-test setups that don't exercise the modifier
  // path (R4's test ctor accepts null and skips the OR-in).
  // Note: mutable T (not const-T) — R4 OR-mutates the shared state;
  // contrast with clipboard.h's `const raw_ptr<const CbHeldModifierState>`
  // which is read-only. Semantic intent preserved per (g.4-precision).
  const raw_ptr<CbHeldModifierState> shared_modifiers_;
};

}  // namespace cloud_browser

#endif  // CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_KEYBOARD_H_

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cloud-browser/capture/build-integration/cb_input_dispatch_keyboard.h"

#include <string>
#include <utility>

#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
// CV2-81 attempt-5 fix-forward (lesson-(g.1) Mutation-shape, 5th
// instance this campaign): chromium-7727 moved
// native_web_keyboard_event.h from content/public/browser/ to
// components/input/. Same family as Wave 1's 83f21af
// native_widget_types.h → native_ui_types.h rename.
//
// CV2-81 attempt-7 fix-forward (lesson-(g.1) Mutation-shape, locus
// twin of the header move): the SAME chromium-7727 change also
// re-homed the type from namespace content:: to namespace input::.
// The use sites below construct input::NativeWebKeyboardEvent.
#include "components/input/native_web_keyboard_event.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"

namespace cloud_browser {

namespace {

// Protocol v1 modifier bits — see input-bridge/main.go modShift etc.
// Duplicated here so this anonymous namespace stays self-contained.
constexpr int kProtoShift = 1 << 0;
constexpr int kProtoCtrl  = 1 << 1;
constexpr int kProtoAlt   = 1 << 2;
constexpr int kProtoMeta  = 1 << 3;

}  // namespace

// ---------------------------------------------------------------------
// CbInputDispatchKeyboard
// ---------------------------------------------------------------------

CbInputDispatchKeyboard::CbInputDispatchKeyboard(
    WebContentsResolver* resolver,
    CbHeldModifierState* shared_modifiers)
    : resolver_(resolver), shared_modifiers_(shared_modifiers) {
  // shared_modifiers_ may be null only in the R4 unit-test paths
  // that don't exercise modifier mutation; production wiring always
  // injects a non-null pointer. Do NOT DCHECK — that would block
  // standalone draft testing.
}

CbInputDispatchKeyboard::~CbInputDispatchKeyboard() = default;

void CbInputDispatchKeyboard::OnInputEvent(InputEnvelope envelope) {
  // Already on BrowserThread::UI (M4 R1 hopped before invoking us).
  // Type-dispatch and ignore non-keyboard envelopes — the composite
  // delegate that wraps R3+R4+R5+R6+R7+R8 routes those elsewhere.
  const base::TimeTicks now = base::TimeTicks::Now();
  if (envelope.type == "key_down") {
    DispatchKey(envelope.data, /*is_down=*/true, now);
  } else if (envelope.type == "key_up") {
    DispatchKey(envelope.data, /*is_down=*/false, now);
  }
  // else: silently ignore; not R4's type.
}

void CbInputDispatchKeyboard::DispatchKey(const base::DictValue& data,
                                          bool is_down,
                                          base::TimeTicks event_time) {
  if (!resolver_) {
    LOG(WARNING) << "CbInputDispatchKeyboard: " << (is_down ? "key_down" : "key_up")
                 << " dropped — no WebContentsResolver wired (M4 R2 pending)";
    return;
  }
  content::WebContents* wc = resolver_->GetActiveWebContents();
  if (!wc) {
    LOG(WARNING) << "CbInputDispatchKeyboard: " << (is_down ? "key_down" : "key_up")
                 << " dropped — no active WebContents";
    return;
  }
  auto* rwhv = wc->GetRenderWidgetHostView();
  content::RenderWidgetHost* rwh = rwhv ? rwhv->GetRenderWidgetHost() : nullptr;
  if (!rwh) {
    LOG(WARNING) << "CbInputDispatchKeyboard: " << (is_down ? "key_down" : "key_up")
                 << " dropped — no RWH";
    return;
  }

  // Protocol shape: { "code": string, "key": string, "mods": int }
  // — see input-bridge/main.go keyData. Both `key` and `code` are
  // optional in practice (clients vary); we tolerate either being
  // absent but require at least one to produce a meaningful event.
  const std::string* key_ptr = data.FindString("key");
  const std::string* code_ptr = data.FindString("code");
  const std::optional<int> mods_envelope = data.FindInt("mods");
  if (!key_ptr && !code_ptr) {
    LOG(WARNING) << "CbInputDispatchKeyboard: envelope missing both key and code";
    return;
  }
  const std::string key = key_ptr ? *key_ptr : std::string();
  const std::string code = code_ptr ? *code_ptr : std::string();

  // Cross-rank modifier state machine. Mirrors the Go bridge's
  // `key_down Shift / mouse_button / key_up Shift` shift-click
  // invariant so the native and CDP backends agree on which
  // modifiers are held when a mouse_button arrives between a
  // key_down and key_up for one of the four named modifier keys.
  const uint32_t mod_bit = KeyToBlinkModifier(key, code);
  if (mod_bit && shared_modifiers_) {
    if (is_down) {
      shared_modifiers_->blink_modifiers |= mod_bit;
    } else {
      shared_modifiers_->blink_modifiers &= ~mod_bit;
    }
  }

  // Compose the modifiers field the way the Go bridge does: the
  // shared cross-rank state OR'd with the envelope-level mods bits
  // a hand-rolled client may have supplied. This way explicit
  // per-envelope tracking augments without overwriting.
  uint32_t modifiers_blink =
      shared_modifiers_ ? shared_modifiers_->blink_modifiers : 0;
  if (mods_envelope) {
    modifiers_blink |= ProtocolModsToBlink(*mods_envelope);
  }

  // Scancode mapping. A miss is non-fatal — we fall through to a
  // text-only path (kChar dispatch with windows_key_code=0) so a
  // unicode glyph or unmapped code still produces a visible
  // character in the focused input.
  ScancodeMapping scancode;
  const bool have_scancode = MapCodeToScancode(code, &scancode);
  if (!have_scancode) {
    VLOG(1) << "CbInputDispatchKeyboard: unmapped code=\"" << code
            << "\" key=\"" << key << "\"; falling through to text-synthesis";
  }

  // kRawKeyDown / kKeyUp event for the scancode side. Skipped when
  // there is no scancode (pure text-synthesis path).
  if (have_scancode) {
    input::NativeWebKeyboardEvent native(
        is_down ? blink::WebInputEvent::Type::kRawKeyDown
                : blink::WebInputEvent::Type::kKeyUp,
        static_cast<int>(modifiers_blink),
        event_time);
    native.windows_key_code = scancode.windows_key_code;
    native.native_key_code = scancode.windows_key_code;
    native.dom_code = scancode.dom_code;
    native.dom_key = scancode.dom_key;
    // text + unmodified_text are populated on the kChar follow-on
    // event below; the kRawKeyDown carries the scancode only,
    // matching how chromium's OS input pipeline shapes the pair.
    rwh->ForwardKeyboardEvent(native);
  }

  // Text-synthesis path. Mirrors the Go bridge's `params["text"]`
  // CDP semantics: for printable characters AND the three special
  // keys that produce text in textareas (Enter / Tab / Backspace),
  // dispatch a kChar event so the renderer fires `input` events and
  // inserts the character. Without this, e.g. Enter in a plain
  // textarea is treated as a navigation keypress (does nothing);
  // pages don't see the newline.
  //
  // kChar fires only on `key_down` — `key_up` never produces a char
  // event in DOM semantics.
  if (is_down) {
    const std::string text = SynthesizedTextFor(key);
    if (!text.empty()) {
      input::NativeWebKeyboardEvent char_event(
          blink::WebInputEvent::Type::kChar,
          static_cast<int>(modifiers_blink),
          event_time);
      char_event.windows_key_code =
          have_scancode ? scancode.windows_key_code : 0;
      char_event.native_key_code =
          have_scancode ? scancode.windows_key_code : 0;
      char_event.dom_code = have_scancode ? scancode.dom_code : 0;
      char_event.dom_key = have_scancode ? scancode.dom_key : 0;

      // NativeWebKeyboardEvent's text + unmodified_text are
      // fixed-size UTF-16 arrays (blink::WebKeyboardEvent::kTextLengthCap).
      // Convert the UTF-8 input to UTF-16 and copy up to the cap.
      std::u16string utf16_text = base::UTF8ToUTF16(text);
      const size_t copy_n = std::min<size_t>(
          utf16_text.size(),
          static_cast<size_t>(blink::WebKeyboardEvent::kTextLengthCap));
      for (size_t i = 0; i < copy_n; ++i) {
        char_event.text[i] = utf16_text[i];
        char_event.unmodified_text[i] = utf16_text[i];
      }
      // Cap-bounded null-termination — the remainder of the array
      // is already zero-initialised by the ctor.
      if (copy_n < static_cast<size_t>(blink::WebKeyboardEvent::kTextLengthCap)) {
        char_event.text[copy_n] = 0;
        char_event.unmodified_text[copy_n] = 0;
      }

      rwh->ForwardKeyboardEvent(char_event);
    }
  }
}

// ---------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------

uint32_t CbInputDispatchKeyboard::KeyToBlinkModifier(const std::string& key,
                                                    const std::string& code) {
  using Mods = blink::WebInputEvent::Modifiers;
  // KeyboardEvent.key form first — the spec-preferred field on
  // modern browsers.
  if (key == "Shift")   return Mods::kShiftKey;
  if (key == "Control") return Mods::kControlKey;
  if (key == "Alt")     return Mods::kAltKey;
  if (key == "Meta" || key == "OS" || key == "Super") {
    return Mods::kMetaKey;
  }
  // Fallback to KeyboardEvent.code for clients that emit code only.
  if (code == "ShiftLeft" || code == "ShiftRight") {
    return Mods::kShiftKey;
  }
  if (code == "ControlLeft" || code == "ControlRight") {
    return Mods::kControlKey;
  }
  if (code == "AltLeft" || code == "AltRight") {
    return Mods::kAltKey;
  }
  if (code == "MetaLeft" || code == "MetaRight" ||
      code == "OSLeft" || code == "OSRight") {
    return Mods::kMetaKey;
  }
  return 0;
}

uint32_t CbInputDispatchKeyboard::ProtocolModsToBlink(int protocol_mods) {
  using Mods = blink::WebInputEvent::Modifiers;
  uint32_t out = 0;
  if (protocol_mods & kProtoShift) out |= Mods::kShiftKey;
  if (protocol_mods & kProtoCtrl)  out |= Mods::kControlKey;
  if (protocol_mods & kProtoAlt)   out |= Mods::kAltKey;
  if (protocol_mods & kProtoMeta)  out |= Mods::kMetaKey;
  return out;
}

bool CbInputDispatchKeyboard::MapCodeToScancode(const std::string& code,
                                                ScancodeMapping* out) {
  // TODO(M4-R4-scancode-table-complete): this table is the minimum
  // set the v1 test_harness/streamer.html actually emits — printable
  // ASCII letters/digits, the three textarea-producing specials, the
  // four arrows, plus Escape/Delete/Insert/Home/End/PageUp/PageDown
  // and F1–F12. Anything outside this set returns false, the caller
  // logs once, and we fall through to a text-only synthesis. For the
  // real M4 wiring we should pull chromium's
  // ui/events/keycodes/dom/dom_code_data.inc verbatim; for the draft
  // we deliberately keep the table small so the compile dependency
  // surface stays bounded.

  // windows_key_code values are Microsoft Virtual-Key codes — the
  // same numeric space chromium uses internally (see
  // ui/events/keycodes/keyboard_codes.h VKEY_*).

  // Letters A–Z. Protocol code "KeyA" .. "KeyZ"; VKEY_A = 0x41.
  if (code.size() == 4 && code.compare(0, 3, "Key") == 0 &&
      code[3] >= 'A' && code[3] <= 'Z') {
    out->windows_key_code = 0x41 + (code[3] - 'A');
    out->dom_code = 0;  // populated by chromium when needed
    out->dom_key = 0;
    return true;
  }
  // Digits 0–9. Protocol code "Digit0" .. "Digit9"; VKEY_0 = 0x30.
  if (code.size() == 6 && code.compare(0, 5, "Digit") == 0 &&
      code[5] >= '0' && code[5] <= '9') {
    out->windows_key_code = 0x30 + (code[5] - '0');
    out->dom_code = 0;
    out->dom_key = 0;
    return true;
  }
  // The three textarea-text producers.
  if (code == "Enter")     { out->windows_key_code = 0x0D; return true; }
  if (code == "Tab")       { out->windows_key_code = 0x09; return true; }
  if (code == "Backspace") { out->windows_key_code = 0x08; return true; }
  // Whitespace + escape + delete + insert.
  if (code == "Space")     { out->windows_key_code = 0x20; return true; }
  if (code == "Escape")    { out->windows_key_code = 0x1B; return true; }
  if (code == "Delete")    { out->windows_key_code = 0x2E; return true; }
  if (code == "Insert")    { out->windows_key_code = 0x2D; return true; }
  // Navigation cluster.
  if (code == "Home")      { out->windows_key_code = 0x24; return true; }
  if (code == "End")       { out->windows_key_code = 0x23; return true; }
  if (code == "PageUp")    { out->windows_key_code = 0x21; return true; }
  if (code == "PageDown")  { out->windows_key_code = 0x22; return true; }
  // Arrows.
  if (code == "ArrowLeft")  { out->windows_key_code = 0x25; return true; }
  if (code == "ArrowUp")    { out->windows_key_code = 0x26; return true; }
  if (code == "ArrowRight") { out->windows_key_code = 0x27; return true; }
  if (code == "ArrowDown")  { out->windows_key_code = 0x28; return true; }
  // Modifier scancodes — clients that send the modifier key as a
  // standalone event need the windows_key_code set for the renderer
  // to record the key in event.code; the held-state machine above
  // is separate (it mutates the shared modifiers bitmask).
  if (code == "ShiftLeft")    { out->windows_key_code = 0xA0; return true; }
  if (code == "ShiftRight")   { out->windows_key_code = 0xA1; return true; }
  if (code == "ControlLeft")  { out->windows_key_code = 0xA2; return true; }
  if (code == "ControlRight") { out->windows_key_code = 0xA3; return true; }
  if (code == "AltLeft")      { out->windows_key_code = 0xA4; return true; }
  if (code == "AltRight")     { out->windows_key_code = 0xA5; return true; }
  if (code == "MetaLeft" || code == "OSLeft")   { out->windows_key_code = 0x5B; return true; }
  if (code == "MetaRight" || code == "OSRight") { out->windows_key_code = 0x5C; return true; }
  // F1–F12. Protocol code "F1" .. "F12"; VKEY_F1 = 0x70.
  if (code.size() == 2 && code[0] == 'F' &&
      code[1] >= '1' && code[1] <= '9') {
    out->windows_key_code = 0x70 + (code[1] - '1');
    return true;
  }
  if (code.size() == 3 && code[0] == 'F' && code[1] == '1' &&
      code[2] >= '0' && code[2] <= '2') {
    out->windows_key_code = 0x79 + (code[2] - '0');
    return true;
  }
  return false;
}

std::string CbInputDispatchKeyboard::SynthesizedTextFor(const std::string& key) {
  // The three textarea-insertion specials. Mirrors the Go bridge's
  // keyTextMap exactly so the kChar payload matches the CDP
  // backend's `params["text"]` field.
  if (key == "Enter")     return "\r";
  if (key == "Tab")       return "\t";
  if (key == "Backspace") return "\b";
  // Navigation / function / modifier keys produce no text — return
  // empty so the caller skips the kChar dispatch. The explicit list
  // is the inverse: anything that ISN'T a single printable
  // codepoint (incl. multi-byte UTF-8 for CJK / emoji) is rejected
  // by the `key.empty()` guard below; the navigation keys hit it
  // because they're multi-character ASCII names.
  if (key.empty()) {
    return std::string();
  }
  // Single-codepoint ASCII printable.
  if (key.size() == 1) {
    const unsigned char c = static_cast<unsigned char>(key[0]);
    if (c >= 0x20 && c < 0x7F) {
      return key;
    }
    // Control chars (other than the three specials above) → no text.
    return std::string();
  }
  // Multi-byte sequences. Heuristic: if the first byte's top two bits
  // are 1 (UTF-8 leading byte) AND the string does not match any of
  // the known multi-char navigation key names ("ArrowLeft", "Home",
  // "Enter" already handled, etc.), treat as a unicode codepoint
  // (CJK char, emoji, dead-key composition output) and pass through.
  // Multi-char ASCII names like "ArrowLeft", "Home", "PageUp",
  // "Escape", "F12", "Shift", "Control", "Alt", "Meta", "Tab"
  // (already short-circuited above) all start with ASCII letters
  // whose top bit is 0; this is a clean discriminator.
  const unsigned char first = static_cast<unsigned char>(key[0]);
  if (first >= 0x80) {
    return key;
  }
  // Multi-char ASCII names — no text-synthesis. The scancode side
  // handled the navigation/F-key/modifier semantics.
  return std::string();
}

}  // namespace cloud_browser

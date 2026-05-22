// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cloud-browser/capture/build-integration/cb_input_dispatch_clipboard.h"

#include "base/logging.h"
// CV2-81 attempt-5 fix-forward (lesson-(g.1) Mutation-shape, 5th
// instance this campaign): chromium-7727 moved
// native_web_keyboard_event.h from content/public/browser/ to
// components/input/. Sibling to keyboard.cc:14 fix.
//
// CV2-81 attempt-7 fix-forward (lesson-(g.1) Mutation-shape, locus
// twin of the header move): the SAME chromium-7727 change also
// re-homed the type from namespace content:: to namespace input::
// (components/input/native_web_keyboard_event.h declares
// `namespace input { struct NativeWebKeyboardEvent ... }`). The use
// sites below construct input::NativeWebKeyboardEvent accordingly.
#include "components/input/native_web_keyboard_event.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/render_widget_host.h"
// CV2-81 attempt-7 fix-forward (lesson-(g.3) Completeness-shape): this
// TU does rwhv->GetRenderWidgetHost() in DispatchCopy(), a member
// access that needs the COMPLETE content::RenderWidgetHostView type —
// content/public/browser/web_contents.h only forward-declares it.
// The sibling input-dispatch TUs (mouse/keyboard/touch/drag/ime)
// already carry this include; clipboard.cc was the lone gap, so
// attempt-6 failed here with "member access into incomplete type".
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"

namespace cloud_browser {

namespace {

// VKEY_C from ui/events/keycodes/keyboard_codes.h. Hard-coded
// because R8 only synthesises this one key — pulling in the full
// scancode table from R4's MapCodeToScancode() would couple R8 to
// R4's internal helper for no benefit. If a future Phase-2 swap
// to `wc->Copy()` lands (see TODO(M4-R8-direct-clipboard-api) in
// the header), this constant disappears entirely.
constexpr int kVkeyC = 0x43;

}  // namespace

CbInputDispatchClipboard::CbInputDispatchClipboard(
    WebContentsResolver* resolver,
    const CbHeldModifierState* shared_modifiers)
    : resolver_(resolver), shared_modifiers_(shared_modifiers) {
  // Both pointers may be null in standalone R8 unit-test paths;
  // production wiring always injects non-null. Do NOT DCHECK —
  // that would block standalone draft testing exactly the way R4
  // documents.
}

CbInputDispatchClipboard::~CbInputDispatchClipboard() = default;

void CbInputDispatchClipboard::OnInputEvent(InputEnvelope envelope) {
  // Already on BrowserThread::UI (M4 R1 hopped before invoking us).
  // Filter on type and ignore everything that isn't ours; the
  // composite delegate that wraps R3+R4+R5+R6+R7+R8 routes other
  // envelopes elsewhere.
  if (envelope.type != "clipboard_copy_request") {
    return;
  }

  if (!resolver_) {
    LOG(WARNING) << "CbInputDispatchClipboard: clipboard_copy_request "
                    "dropped — no WebContentsResolver wired (M4 R2 pending)";
    return;
  }
  content::WebContents* wc = resolver_->GetActiveWebContents();
  if (!wc) {
    LOG(WARNING) << "CbInputDispatchClipboard: clipboard_copy_request "
                    "dropped — no active WebContents";
    return;
  }

  // Diagnostic: if Ctrl was already held client-side (the user
  // really did press Ctrl and then issue the copy gesture, for
  // example), our synthesised Ctrl-down is functionally a no-op on
  // the renderer side. Not an error — just useful telemetry when
  // chasing "why didn't my copy fire" reports.
  if (shared_modifiers_ &&
      (shared_modifiers_->blink_modifiers &
       blink::WebInputEvent::Modifiers::kControlKey) != 0) {
    VLOG(1) << "CbInputDispatchClipboard: clipboard_copy_request arrived "
               "with Ctrl already held client-side (seq=" << envelope.seq
            << ") — synth Ctrl-down is redundant but harmless";
  }

  DispatchCopy(wc, base::TimeTicks::Now());
}

void CbInputDispatchClipboard::DispatchCopy(content::WebContents* wc,
                                            base::TimeTicks event_time) {
  // Walk WC → RVH → RWH every call (matches R4 — never cache; a
  // cross-document navigation swaps the RWH and a cached pointer
  // would land the event on a destroyed widget).
  auto* rwhv = wc->GetRenderWidgetHostView();
  content::RenderWidgetHost* rwh = rwhv ? rwhv->GetRenderWidgetHost() : nullptr;
  if (!rwh) {
    LOG(WARNING) << "CbInputDispatchClipboard: clipboard_copy_request "
                    "dropped — no RWH";
    return;
  }

  // Synthesised Ctrl+C — see the "Implementation choice" section
  // in the header for why we picked this over `wc->Copy()`.
  //
  // We OR the Ctrl bit into the per-event modifiers only — we do
  // NOT touch CbHeldModifierState. A subsequent mouse_button or
  // any other event that arrives between our key_down and key_up
  // must NOT see Ctrl pinned in its modifier bitmask, because the
  // client never reported holding Ctrl on the wire.
  //
  // Note also that even the kRawKeyDown alone is enough to fire
  // the editor command on the renderer side (chromium's keyboard-
  // command translator runs in the renderer at kRawKeyDown time,
  // not at kKeyUp time). The kKeyUp event is dispatched anyway so
  // the page's `keyup` listener fires symmetrically with the
  // `keydown` — pages that maintain their own "is Ctrl held" state
  // (some code editors do) would otherwise believe Ctrl is stuck
  // down.

  const int modifiers_blink =
      static_cast<int>(blink::WebInputEvent::Modifiers::kControlKey);

  // kRawKeyDown — triggers the editor command on the renderer.
  {
    input::NativeWebKeyboardEvent native(
        blink::WebInputEvent::Type::kRawKeyDown,
        modifiers_blink,
        event_time);
    native.windows_key_code = kVkeyC;
    native.native_key_code = kVkeyC;
    // dom_code / dom_key are intentionally left at 0 — for a
    // synthesised editor-command keystroke we don't need the
    // full DOM-code/key surface (which would require pulling in
    // ui/events/keycodes/dom/dom_code.h purely for the "KeyC"
    // identifier). Matches the Go bridge, which omits these
    // fields too.
    //
    // TODO(M4-R8-dom-code-completeness): if a downstream test
    // asserts on event.code === "KeyC" for the synthesised pair,
    // pull dom_code = ui::DomCode::US_C and dom_key =
    // ui::DomKey::Constant<'C'>::Character. Not part of the
    // wire-protocol invariant.
    rwh->ForwardKeyboardEvent(native);
  }

  // kKeyUp — pages with `keyup` listeners track the up edge.
  {
    input::NativeWebKeyboardEvent native(
        blink::WebInputEvent::Type::kKeyUp,
        modifiers_blink,
        event_time);
    native.windows_key_code = kVkeyC;
    native.native_key_code = kVkeyC;
    rwh->ForwardKeyboardEvent(native);
  }

  // No kChar event. The Go bridge does not emit one either: a
  // Ctrl+C produces no `input` event because the character is not
  // inserted into a text field. R4's SynthesizedTextFor() would
  // (correctly) skip it anyway when Ctrl is in the modifier
  // bitmask, but R8 short-circuits the kChar path entirely so we
  // don't accidentally insert 'c' into a focused textarea.

  VLOG(1) << "CbInputDispatchClipboard: synthesised Ctrl+C "
             "(kRawKeyDown+kKeyUp) — clipboard delta will flow back "
             "via M6 R2 clipboard DC";
}

}  // namespace cloud_browser

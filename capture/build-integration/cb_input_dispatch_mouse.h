// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbInputDispatchMouse — M4 R3 typed delegate for mouse_move /
// mouse_button / mouse_wheel envelopes coming out of M4 R1's
// CbInputDispatch (after the signaling-thread → BrowserThread::UI hop).
//
// The mouse path is the spec's "native equivalent" of the Go
// input-bridge/main.go CDP dispatcher, but driven through chromium's
// internal `RenderWidgetHost::ForwardMouseEvent` /
// `ForwardWheelEvent` APIs instead of `Input.dispatchMouseEvent`. The
// two paths are intentionally semantically parity-aligned — the same
// envelope sequence MUST produce the same observable DOM event
// sequence (mousedown / mouseup / click / dblclick / wheel) in either
// backend. See capture/input-bridge/main.go's Dispatch() for the wire-
// format reference.
//
// R3 scope (CV2-43):
//   * mouse_move, mouse_button, mouse_wheel native dispatch
//   * held-button state (drag detection: mousemove during a drag MUST
//     carry buttons!=0)
//   * held-modifier state (shared with M4 R4 keyboard — R3 OWNS the
//     storage; R4 mutates via SetHeldModifiers on key_down/key_up)
//   * click-ramp (single → double → triple): 500ms window, 5px slop,
//     ramp preserved on `up` so dblclick fires after the second pair
//   * wheel-phase machine: protocol phase=start synthesises a
//     kPhaseBegan zero-delta event, phase=end synthesises a
//     kPhaseEnded zero-delta event so chromium's compositor flushes
//     momentum decay; intermediate events are kPhaseChanged
//   * delta_mode precedence (string field overrides numeric `mode`)
//     and line=16px / page=800px scaling — matches the pre-T62 Go
//     bridge for parity, Phase 2 will replace with line-height-aware
//     scrolling
//   * content-space → widget-space coord map including device-scale-
//     factor (Finding 6 from the M4 design memo: protocol coords are
//     content-space px post-object-fit; the FSVC captures the host
//     window @ 1280x720 so DSF=1 paths are no-ops, DSF!=1 paths must
//     divide)
//   * once-per-active-widget bring-to-front / activation analogue
//     (Page.bringToFront CDP equivalent) — chromium without focus
//     does not deliver synthetic input to the renderer's expected
//     focus chain
//   * publishes a last-forwarded-pointer snapshot consumed by M4 R10
//     (cursor egress overlay) — the snapshot is the LAST event we
//     successfully forwarded, not the last we received
//
// Non-goals for R3:
//   * keyboard / IME / drag / touch (M4 R4 / R5 / R6 / R7 / R8 own
//     those)
//   * scroll-into-view, smooth scrolling, momentum simulation —
//     chromium's compositor handles those once the wheel phase
//     transitions are correct
//   * pointer capture / pointer lock — separate envelope types
//     (deferred to a v2 spec)
//
// Dependency on M4 R2 (active streamed-WebContents resolver):
//   R3 dispatches to the WebContents that the FSVC is currently
//   capturing from. That resolution is M4 R2's job; this file uses a
//   `WebContentsResolver` interface (forward-declared) so R3 can
//   compile / unit-test without R2 having landed yet. The R2 owner
//   will supply the concrete implementation and the wiring in
//   cloud_browser_browser_main_parts.cc; until then, the resolver
//   pointer can be null and R3 logs a WARNING per envelope.
//
// All public methods are invoked on BrowserThread::UI (enforced by
// M4 R1's PostTask). The class is NOT thread-safe — it holds raw
// mutable state for the click-ramp / held-buttons / held-modifiers /
// wheel-phase machines, and chromium's input APIs are themselves
// UI-thread-only.

#ifndef CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_MOUSE_H_
#define CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_MOUSE_H_

#include <cstdint>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "cloud-browser/capture/build-integration/cb_input_dispatch.h"
#include "cloud-browser/capture/build-integration/cb_last_pointer.h"

namespace content {
class RenderWidgetHost;
class WebContents;
}  // namespace content

namespace cloud_browser {

// WebContentsResolver — M4 R2 interface. R3 forward-declares it here
// so the .cc can compile without R2 sources. The real interface lives
// in capture/build-integration/cb_active_webcontents_resolver.h (M4
// R2 will land that file alongside the resolver implementation).
//
// Contract:
//   * GetActiveWebContents() returns the WebContents that the FSVC is
//     currently capturing from, or nullptr if no capture is active.
//   * Must be safe to call from BrowserThread::UI.
//   * Lifetime: caller-owned, must outlive any CbInputDispatchMouse
//     that references it.
//
// TODO(M4-R3-r2-interface): when R2 lands, replace this forward
// declaration with `#include "cloud-browser/capture/build-integration/
// cb_active_webcontents_resolver.h"` and drop the inline interface.
class WebContentsResolver {
 public:
  virtual ~WebContentsResolver() = default;
  virtual content::WebContents* GetActiveWebContents() = 0;
};

// (CbLastPointerSnapshot is defined canonically in cb_last_pointer.h —
// M4 R10. R3 delegates its inline snapshot storage to
// CbLastPointerState; see member `last_pointer_state_` below.)

class CbInputDispatchMouse : public CbInputDispatchDelegate {
 public:
  // |resolver| supplies the active WebContents for each dispatch; may
  // be null during M4 R2 development (R3 will log + skip dispatch).
  // Lifetime: caller-owned, must outlive this object.
  explicit CbInputDispatchMouse(WebContentsResolver* resolver);

  CbInputDispatchMouse(const CbInputDispatchMouse&) = delete;
  CbInputDispatchMouse& operator=(const CbInputDispatchMouse&) = delete;

  ~CbInputDispatchMouse() override;

  // CbInputDispatchDelegate. R3 only acts on mouse_* envelopes;
  // unknown / non-mouse types are silently dropped here (the
  // composite delegate that M4 wiring assembles will route them to
  // R4 / R5 / R7 / R8 instead).
  //
  // TODO(M4-R3-composite-delegate): once R8 lands the composite
  // delegate, swap this from "filter and ignore" to a pre-typed
  // method (OnMouseMove / OnMouseButton / OnMouseWheel) so non-mouse
  // envelopes never reach this class.
  void OnInputEvent(InputEnvelope envelope) override;

  // Modifier state shared with M4 R4 (keyboard). R3 owns the storage;
  // R4 calls SetHeldModifiersBlink() on key_down/key_up with the
  // updated mask. The mask uses blink::WebInputEvent::Modifiers bits
  // (kShiftKey / kControlKey / kAltKey / kMetaKey).
  //
  // Why R3 owns this: the spec calls out that mouse_button events
  // need to carry the shift/ctrl/alt/meta state from the most recent
  // key_down without a matching key_up (so `key_down Shift /
  // mouse_button down / mouse_button up / key_up Shift` produces a
  // shift-click). The natural home for that storage is the side that
  // reads it on every dispatch — i.e. R3.
  uint32_t held_modifiers_blink() const { return held_modifiers_blink_; }
  void SetHeldModifiersBlink(uint32_t modifiers) {
    held_modifiers_blink_ = modifiers;
  }

  // Snapshot consumed by M4 R10 / M5 R3. Returns the LAST successfully
  // forwarded pointer; a forward that failed (no active WebContents,
  // dropped envelope) does not update this. R10 reads on the UI
  // thread. Delegates to the CbLastPointerState held by R3.
  const CbLastPointerSnapshot& last_pointer() const {
    return last_pointer_state_.last_pointer();
  }

  // Test seam — flips the bring-to-front-once latch without an actual
  // dispatch. Used by cb_input_dispatch_mouse_test.cc to verify
  // subsequent dispatches do NOT re-activate.
  void ResetBringToFrontLatchForTesting() { brought_to_front_ = false; }

 private:
  // Per-type dispatch handlers. Each pulls fields out of `data` per
  // the protocol shape in docs/protocols/input-channel.md.
  void DispatchMouseMove(const base::DictValue& data,
                         base::TimeTicks event_time);
  void DispatchMouseButton(const base::DictValue& data,
                           base::TimeTicks event_time);
  void DispatchMouseWheel(const base::DictValue& data,
                          base::TimeTicks event_time);

  // Once-per-instance activation analogue (Page.bringToFront). Called
  // lazily on first dispatch so we don't pay the cost during M4 R2
  // resolver bring-up. Chromium-side: WebContents::WasShown() +
  // RenderWidgetHostView::Focus() are the equivalent calls; we wrap
  // both in a single helper because either alone is insufficient
  // (RWH focus without WasShown leaves the page in the
  // hidden-document state; WasShown without RWH focus leaves the
  // renderer accepting events but routing them to a defocused frame).
  //
  // TODO(M4-R3-bring-to-front-method): pick between WasShown+Focus
  // and a single content::WebContents::Activate() once we know
  // which surfaces the right OnVisibilityChanged signals to extension
  // / DevTools clients. The Aura platform data path (BUGS-529 second-
  // layer fix in cb_aura_platform_data.cc) sets up the focus chain
  // we rely on here; verify that chain is alive before this method
  // is reached.
  void EnsureBroughtToFront(content::WebContents* wc);

  // Coordinate map. content_x / content_y are protocol-space px
  // (post object-fit, matching the FSVC capture size). Returns the
  // widget-space DIPs the WebMouseEvent expects in
  // position_in_widget.
  //
  // Implementation reads RenderWidgetHostView::GetDeviceScaleFactor()
  // and divides; for DSF=1 (the common case at 1280x720 FSVC) it is
  // a no-op cast.
  //
  // TODO(M4-R3-dsf-source): verify whether
  // RenderWidgetHostView::GetDeviceScaleFactor() returns the correct
  // value when the embedder is running headless in cb-chromium. Aura
  // host display info might be the more accurate source. Re-check
  // once R2 is in place and a first end-to-end click is wired.
  struct WidgetPoint {
    float x;
    float y;
  };
  WidgetPoint ContentToWidget(content::RenderWidgetHost* rwh,
                              int content_x, int content_y);

  // Click-ramp state. Updated only on mouse_button `down` (ramp logic
  // runs there); `up` reads but does not reset the count so a rapid
  // down/up/down/up sequence elevates to clickCount=2 on the second
  // down and chromium fires dblclick on the second up.
  struct ClickRamp {
    // Protocol button index (0=left, 1=middle, 2=right, 3=back,
    // 4=forward) — kept in protocol units so the compare in
    // DispatchMouseButton is trivial.
    int protocol_button = -1;
    // Last `down` position in protocol coords (NOT widget DIPs) for
    // the 5px slop compare. Storing in protocol coords avoids any
    // DSF-rounding mismatch between successive clicks.
    int x = 0;
    int y = 0;
    base::TimeTicks at;
    // Current count; 0 = "no prior click in window".
    int count = 0;
  };
  ClickRamp last_click_;

  // Browser convention — matches input-bridge/main.go and the chromium
  // upstream constants kDoubleClickTimeMs / kDoubleClickRangePx (which
  // are private; see ui/events/event.cc).
  //
  // TODO(M4-R3-chromium-constants): swap to the upstream constants
  // when M4 lands in a chromium tree we can compile against —
  // duplicating them here keeps R3 unit-testable without dragging in
  // ui/events. Document the source of truth in input-channel.md.
  static constexpr base::TimeDelta kClickRampWindow = base::Milliseconds(500);
  static constexpr int kClickRampSlopPx = 5;

  // Wheel scaling — protocol line=16px, page=800px. Mirrors the Go
  // bridge's pre-T62 behaviour for parity.
  static constexpr float kWheelLinePx = 16.f;
  static constexpr float kWheelPagePx = 800.f;

  // Held-button state. blink::WebInputEvent::Modifiers bits for the
  // five mouse buttons (kLeftButtonDown / kRightButtonDown /
  // kMiddleButtonDown / kBackButtonDown / kForwardButtonDown). Read
  // by mouse_move to maintain drag-detector invariants, mutated by
  // mouse_button down/up.
  uint32_t held_buttons_blink_ = 0;

  // Held-modifier state shared with M4 R4. blink::WebInputEvent::
  // Modifiers bits — kShiftKey / kControlKey / kAltKey / kMetaKey.
  // R4 sets/clears via SetHeldModifiersBlink on key_down/key_up; R3
  // ORs it into every mouse event's modifiers field.
  uint32_t held_modifiers_blink_ = 0;

  // Wheel-phase machine. v1.0 clients (no `phase` field) keep
  // wheel_in_gesture_ false and every event dispatches as
  // kPhaseChanged with the carried delta. v1.1 clients drive the
  // state through phase=start / phase=changed / phase=end.
  bool wheel_in_gesture_ = false;

  // Bring-to-front latch. Flipped on the first successful dispatch
  // that resolves a non-null WebContents; never reset in production
  // (test-only seam above).
  bool brought_to_front_ = false;

  // Last-forwarded pointer for R10 / M5 R3. Updated only on a
  // successful ForwardMouseEvent / ForwardWheelEvent — a dispatch
  // that failed resolution (resolver returned null, RWH had no view)
  // leaves this untouched so R10 doesn't paint a "ghost" cursor from
  // a dropped envelope. Storage canonically lives in R10's
  // CbLastPointerState (cb_last_pointer.h); R3 mutates via Update().
  CbLastPointerState last_pointer_state_;

  // M4 R2 resolver. Caller-owned; can be null pre-R2.
  // CV2-81 first-compile-link fix-forward (lesson-(g.4) Discipline-shape).
  // build-czar grep-sweep caught this site beyond the 10-error build cutoff;
  // had the sweep been limited to the visible 6 violations, a third-pass FAIL
  // on this same lint family would have surfaced one iteration later.
  const raw_ptr<WebContentsResolver> resolver_;
};

}  // namespace cloud_browser

#endif  // CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_MOUSE_H_

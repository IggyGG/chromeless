// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbInputDispatchTouch — M4 R6 typed delegate for touch_start /
// touch_move / touch_end / touch_cancel envelopes coming out of M4 R1's
// CbInputDispatch (after the signaling-thread → BrowserThread::UI hop).
//
// The touch path is the native equivalent of input-bridge/main.go's
// touch CDP dispatcher, but driven through chromium's internal
// `RenderWidgetHostImpl::ForwardTouchEventWithLatencyInfo` /
// `blink::WebTouchEvent` instead of `Input.dispatchTouchEvent`. The
// two paths are intentionally semantically parity-aligned — the same
// envelope sequence MUST produce the same observable DOM
// touchstart/touchmove/touchend/touchcancel + click-synthesis sequence
// in either backend. See capture/input-bridge/main.go's `touch_*`
// cases (lines 1613-1690) for the wire-format reference.
//
// R6 scope (CV2-46):
//   * touch_start / touch_move / touch_end / touch_cancel native
//     dispatch via blink::WebTouchEvent
//   * active-touch-points state machine keyed by per-finger
//     `identifier` (protocol identifier semantics: stable across the
//     lifetime of one touch, reusable after end/cancel)
//   * one-envelope-per-finger fan-in: each envelope updates exactly
//     one point's state; all other active points carry state
//     kStateStationary in the dispatched WebTouchEvent
//   * touchPoints-after-this-event-applies convention (matches
//     CDP/puppeteer): touch_end removes the finger from the map
//     BEFORE the WebTouchEvent is assembled, so the final lift sends
//     an event with touches_length == 0 (the released point's
//     state=kStateReleased is recorded on the changed_touches slot
//     instead — see DispatchTouchEnd)
//   * unknown-identifier guard for move/end/cancel: a no-op +
//     WARNING log (matches the Go bridge's per-spec MUST behaviour)
//   * kTouchesLengthCap (16) overflow on touch_start: drop the new
//     finger + log a metric; the protocol does not cap, but
//     blink::WebTouchEvent does. v1 clients in practice send ≤10.
//   * content-space → widget-space coord map including device-scale-
//     factor (matches M4 R3's mouse coord path; protocol coords are
//     content-space px post-object-fit; the FSVC captures the host
//     window @ 1280x720 so DSF=1 is a no-op)
//   * once-per-active-widget bring-to-front / activation analogue
//     (Page.bringToFront equivalent) — chromium without focus does
//     not deliver synthetic touch to the renderer's expected focus
//     chain. Shared semantics with M4 R3 mouse but kept in this
//     delegate (touch and mouse may arrive in either order; whichever
//     fires first owns the latch flip)
//
// Non-goals for R6:
//   * mouse / wheel / keyboard / IME / drag (M4 R3 / R4 / R5 / R7
//     own those)
//   * pointer events / pointer capture / pointer lock (separate
//     envelope types; v2 spec)
//   * touch-driven scroll-gesture synthesis — chromium's compositor
//     handles touch → scroll once the WebTouchEvent stream is
//     correct; we do not pre-synthesize WebGestureEvent.
//   * touch-modifiers (the protocol does not carry shift/ctrl/alt
//     bits on touch envelopes; matches DOM TouchEvent which has no
//     modifiers field). v2 may add this for keyboard+touch chord
//     gestures.
//   * force-touch / 3D-touch threshold synthesis — protocol
//     `force` is forwarded verbatim into WebTouchPoint.force;
//     downstream interpretation is chromium's.
//
// Dependency on M4 R2 (active streamed-WebContents resolver):
//   R6 dispatches to the WebContents that the FSVC is currently
//   capturing from. That resolution is M4 R2's job; this file uses
//   the same `WebContentsResolver` interface as M4 R3 (declared in
//   cb_input_dispatch_mouse.h) so R6 can compile / unit-test without
//   R2 having landed yet. When the resolver returns null, R6 logs a
//   WARNING and updates state machine state anyway — dropping the
//   state update on a transient resolver miss would leave the
//   active-points map inconsistent and break the next correctly-
//   resolved dispatch's "touches array after this event applies"
//   contract. (Mouse R3 is allowed to drop on resolver miss because
//   its state — click ramp, held buttons — recovers naturally on
//   the next button event. Touch state has no such recovery path.)
//
// All public methods are invoked on BrowserThread::UI (enforced by
// M4 R1's PostTask). The class is NOT thread-safe — it holds raw
// mutable state for the active-points map, and chromium's input
// APIs are themselves UI-thread-only.

#ifndef CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_TOUCH_H_
#define CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_TOUCH_H_

#include <cstdint>
#include <string>
#include <unordered_map>

#include "base/time/time.h"
#include "cloud-browser/capture/build-integration/cb_input_dispatch.h"
#include "cloud-browser/capture/build-integration/cb_input_dispatch_mouse.h"
#include "third_party/blink/public/common/input/web_touch_event.h"

namespace content {
class RenderWidgetHost;
class WebContents;
}  // namespace content

namespace cloud_browser {

// TODO(M4-R6-shared-resolver): when M4 R2 (active WebContents
// resolver) lands, the `WebContentsResolver` interface will move out
// of cb_input_dispatch_mouse.h into its own header
// (cb_active_webcontents_resolver.h). Both this file and the mouse
// delegate currently transit through the mouse header — that
// indirection goes away at the same time.

// In-bridge representation of one active finger. Mirrors the protocol
// touch_start payload shape, kept in protocol units (content-space px,
// degrees, [0,1] force) so the snapshot is comparable directly to
// what the wire delivered. Conversion to blink units happens during
// WebTouchEvent assembly.
struct CbActiveTouchPoint {
  // Per-finger identifier; stable across the lifetime of one touch.
  // Map key — duplicated here for convenience when iterating values.
  int identifier = 0;
  // Content-space px (protocol units, pre device-scale-factor).
  int x = 0;
  int y = 0;
  // Contact ellipse radii in px. Spec default = 1 if client omits
  // (Webkit historical default; matches what input-bridge/main.go's
  // max1() helper produces from the missing field).
  int radius_x = 1;
  int radius_y = 1;
  // 0.0–1.0 mirror of Touch.force; default 0 if not reported.
  float force = 0.f;
  // Rotation in degrees clockwise; mirrors Touch.rotationAngle;
  // default 0 if not reported.
  int twist = 0;
  // Monotonic time the last update for this finger was observed on
  // the UI thread. Used as WebTouchPoint.event_time on subsequent
  // stationary repeats so chromium does not see the same point
  // jitter back to "now" while another finger moves.
  base::TimeTicks last_event_time;
};

class CbInputDispatchTouch : public CbInputDispatchDelegate {
 public:
  // |resolver| supplies the active WebContents for each dispatch; may
  // be null during M4 R2 development (R6 will log + still update the
  // state map — see "Dependency on M4 R2" note in the file header).
  // Lifetime: caller-owned, must outlive this object.
  explicit CbInputDispatchTouch(WebContentsResolver* resolver);

  CbInputDispatchTouch(const CbInputDispatchTouch&) = delete;
  CbInputDispatchTouch& operator=(const CbInputDispatchTouch&) = delete;

  ~CbInputDispatchTouch() override;

  // CbInputDispatchDelegate. R6 only acts on touch_* envelopes;
  // unknown / non-touch types are silently dropped here (the
  // composite delegate that M4 wiring assembles will route them to
  // R3 / R4 / R5 / R7 instead).
  //
  // TODO(M4-R6-composite-delegate): once M4 R8 lands the composite
  // delegate, swap this from "filter and ignore" to a pre-typed
  // method (OnTouchStart / OnTouchMove / OnTouchEnd / OnTouchCancel)
  // so non-touch envelopes never reach this class.
  void OnInputEvent(InputEnvelope envelope) override;

  // Read-only access to the active points. Exposed for the M4 R10
  // overlay so the operator can see what the server thinks the
  // active fingers are. UI-thread only; the map is the live one,
  // not a snapshot, so callers must not retain references.
  const std::unordered_map<int, CbActiveTouchPoint>& active_points() const {
    return active_points_;
  }

  // Test seam — flips the bring-to-front-once latch without an actual
  // dispatch. Mirrors CbInputDispatchMouse's equivalent so the test
  // for the composite delegate (M4 R8) can verify that whichever
  // delegate dispatches first flips the latch and the other does
  // NOT re-activate.
  void ResetBringToFrontLatchForTesting() { brought_to_front_ = false; }

  // Test seam — observe the last assembled WebTouchEvent (before
  // forwarding) without standing up a real RenderWidgetHost. Used by
  // cb_input_dispatch_touch_test.cc to assert the touches[] layout
  // for the after-this-event-applies contract on the last-finger-up
  // edge case.
  const blink::WebTouchEvent& last_assembled_event_for_testing() const {
    return last_assembled_event_for_testing_;
  }

 private:
  // Per-type dispatch handlers. Each pulls fields out of `data` per
  // the protocol shape in docs/protocols/input-channel.md §
  // "touch_start / touch_move / touch_end / touch_cancel".
  void DispatchTouchStart(const base::Value::Dict& data,
                          base::TimeTicks event_time);
  void DispatchTouchMove(const base::Value::Dict& data,
                         base::TimeTicks event_time);
  void DispatchTouchEnd(const base::Value::Dict& data,
                        base::TimeTicks event_time,
                        bool cancel);

  // Assemble a blink::WebTouchEvent of the given type from the
  // current active_points_ snapshot. The point identified by
  // `changed_identifier` is stamped with `changed_state` (kStatePressed
  // / kStateMoved / kStateReleased / kStateCancelled); every other
  // active point is stamped kStateStationary.
  //
  // For touch_end / touch_cancel, the caller MUST have removed the
  // released finger from active_points_ BEFORE calling — but ALSO
  // supplies the released finger's last-known CbActiveTouchPoint as
  // `released_point` so the WebTouchEvent's touches[] can carry it
  // with kStateReleased / kStateCancelled. This matches the
  // CDP/puppeteer "touchPoints after this event applies" contract.
  // For start / move, pass nullptr for released_point.
  //
  // Returns false on kTouchesLengthCap overflow (touch_start with
  // >= 16 active points); the caller should drop the envelope and
  // log a metric. Otherwise populates last_assembled_event_for_
  // testing_ and returns true.
  bool AssembleTouchEvent(blink::WebInputEvent::Type type,
                          int changed_identifier,
                          blink::WebTouchPoint::State changed_state,
                          const CbActiveTouchPoint* released_point,
                          base::TimeTicks event_time,
                          content::RenderWidgetHost* rwh,
                          blink::WebTouchEvent* out);

  // Forward to RenderWidgetHostImpl. Returns true on success (RWH
  // resolved + dispatched); false if no RWH was reachable. Caller
  // logs the warning.
  //
  // TODO(M4-R6-impl-vs-public-api): chromium's public
  // content::RenderWidgetHost does NOT expose ForwardTouchEvent
  // directly — the public surface for synthetic input is
  // RenderWidgetHostInputEventRouter (browser-side gesture routing).
  // For now we cast to RenderWidgetHostImpl via the impl-side header,
  // matching M4 R3's mouse path. If chromium's public API grows a
  // ForwardTouchEventWithLatencyInfo before integration, swap.
  bool ForwardWebTouchEvent(content::RenderWidgetHost* rwh,
                            const blink::WebTouchEvent& event);

  // Once-per-instance activation analogue (Page.bringToFront). Same
  // semantics as M4 R3's mouse path — see that header for the
  // WasShown + Focus rationale.
  //
  // TODO(M4-R6-shared-bring-to-front): when the composite delegate
  // lands (M4 R8), this should consult a shared latch owned by the
  // composite so touch and mouse don't double-fire WasShown on the
  // first event regardless of which envelope type arrives first.
  void EnsureBroughtToFront(content::WebContents* wc);

  // Coordinate map identical to M4 R3's. Pulled into this file so
  // R3 and R6 can evolve independently; the implementation calls
  // RenderWidgetHostView::GetDeviceScaleFactor() and divides.
  struct WidgetPoint {
    float x;
    float y;
  };
  WidgetPoint ContentToWidget(content::RenderWidgetHost* rwh,
                              int content_x, int content_y);

  // Stamp a single touch point's blink-side fields from an active
  // map entry plus the (already-resolved) widget-space DIPs.
  void FillWebTouchPoint(const CbActiveTouchPoint& src,
                         WidgetPoint widget_pos,
                         blink::WebTouchPoint::State state,
                         blink::WebTouchPoint* out);

  // Active touch points keyed by per-finger identifier. The
  // WebTouchEvent touches[] field is rebuilt from this on every
  // dispatch — chromium expects all currently-active points on every
  // event, with state=kStateStationary on the ones that did not
  // change.
  //
  // Sizing: blink::WebTouchEvent::kTouchesLengthCap is 16. The
  // protocol does not cap, but a touch_start with the map already
  // at 16 is dropped (logged + metric). In practice v1 clients send
  // ≤10 (the WebKit TouchEvent spec cap).
  std::unordered_map<int, CbActiveTouchPoint> active_points_;

  // Bring-to-front latch. Same semantics as the M4 R3 mouse path:
  // flipped on the first successful dispatch that resolves a non-
  // null WebContents; never reset in production (test-only seam
  // above).
  bool brought_to_front_ = false;

  // Last assembled WebTouchEvent — test-only observation seam. We
  // assemble into this directly inside AssembleTouchEvent so the
  // test can introspect even when ForwardWebTouchEvent fails (e.g.
  // null resolver path).
  blink::WebTouchEvent last_assembled_event_for_testing_;

  // M4 R2 resolver. Caller-owned; can be null pre-R2.
  WebContentsResolver* const resolver_;
};

}  // namespace cloud_browser

#endif  // CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_TOUCH_H_

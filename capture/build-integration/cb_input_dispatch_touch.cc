// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cloud-browser/capture/build-integration/cb_input_dispatch_touch.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "base/logging.h"
#include "base/values.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_touch_event.h"
#include "third_party/blink/public/common/input/web_touch_point.h"
#include "ui/gfx/geometry/point_f.h"

// TODO(M4-R6-impl-include): RenderWidgetHostImpl lives in
// content/browser/, which is not a public chromium target. M4 R3 made
// the same choice for ForwardMouseEvent and we mirror it here. If the
// build complains about content_browser visibility, the fix is the
// same: add a //content/browser:cb_friends visibility allowlist entry
// for cloud-browser/* or carve a public helper in
// content/public/browser/. Document the choice in the M4 closeout.
#include "content/browser/renderer_host/render_widget_host_impl.h"

namespace cloud_browser {

namespace {

// Maximum simultaneous touch points blink can carry on a single
// WebTouchEvent. Mirror of blink::WebTouchEvent::kTouchesLengthCap;
// duplicated here so this file doesn't depend on the constant having
// internal linkage rules we can't predict across chromium revs.
//
// TODO(M4-R6-blink-cap-constant): if the upstream constant becomes
// reachable, use it directly and delete this local copy.
constexpr size_t kBlinkTouchPointsCap = 16;
static_assert(kBlinkTouchPointsCap ==
                  static_cast<size_t>(blink::WebTouchEvent::kTouchesLengthCap),
              "local kBlinkTouchPointsCap must match blink upstream");

// Clamps a protocol radius value to the spec default of 1 when the
// client omits the field. Mirrors input-bridge/main.go's max1() helper
// so the two backends produce identical WebTouchPoint geometry from
// the same envelope.
int RadiusOrDefault(int raw) {
  return raw > 0 ? raw : 1;
}

// Pull an int field from a base::Value::Dict with a fallback default.
// FindInt returns std::optional; we collapse it to the value or the
// default. Used for optional protocol fields (radius_x / radius_y /
// twist) where a missing key is normal.
int FindIntOr(const base::Value::Dict& d, std::string_view key, int fallback) {
  std::optional<int> v = d.FindInt(key);
  return v.value_or(fallback);
}

// Pull a double field with fallback; same shape as FindIntOr above.
// Used for `force` (spec is 0.0–1.0).
double FindDoubleOr(const base::Value::Dict& d,
                    std::string_view key,
                    double fallback) {
  std::optional<double> v = d.FindDouble(key);
  return v.value_or(fallback);
}

// Build a CbActiveTouchPoint from the per-finger `data` dict of a
// touch_start / touch_move envelope. Required fields are `identifier`,
// `x`, `y`; the caller is expected to have validated identifier
// already (it gates the active-points map lookup). If x/y are missing
// returns std::nullopt and the caller logs + drops the envelope.
std::optional<CbActiveTouchPoint> ParseTouchPointData(
    const base::Value::Dict& data,
    base::TimeTicks event_time) {
  std::optional<int> id = data.FindInt("identifier");
  std::optional<int> x = data.FindInt("x");
  std::optional<int> y = data.FindInt("y");
  if (!id.has_value() || !x.has_value() || !y.has_value()) {
    return std::nullopt;
  }
  CbActiveTouchPoint p;
  p.identifier = *id;
  p.x = *x;
  p.y = *y;
  p.radius_x = RadiusOrDefault(FindIntOr(data, "radius_x", 1));
  p.radius_y = RadiusOrDefault(FindIntOr(data, "radius_y", 1));
  p.force = static_cast<float>(FindDoubleOr(data, "force", 0.0));
  p.twist = FindIntOr(data, "twist", 0);
  p.last_event_time = event_time;
  return p;
}

// touch_end / touch_cancel carries only `identifier`. Extracted here
// so the dispatch handler stays focused on state transitions.
std::optional<int> ParseTouchEndIdentifier(const base::Value::Dict& data) {
  return data.FindInt("identifier");
}

}  // namespace

// ---------------------------------------------------------------------
// CbInputDispatchTouch — public surface
// ---------------------------------------------------------------------

CbInputDispatchTouch::CbInputDispatchTouch(WebContentsResolver* resolver)
    : resolver_(resolver) {}

CbInputDispatchTouch::~CbInputDispatchTouch() = default;

void CbInputDispatchTouch::OnInputEvent(InputEnvelope envelope) {
  // base::TimeTicks::Now() is the dispatch-time stamp. The protocol's
  // envelope `t` field is client-side wall-clock ms-since-epoch and
  // is not suitable for chromium event timing (clock skew, latency,
  // and pause-during-suspend all break the math). chromium's input
  // pipeline keys off TimeTicks; matching M4 R3 here.
  const base::TimeTicks now = base::TimeTicks::Now();

  if (envelope.type == "touch_start") {
    DispatchTouchStart(envelope.data, now);
  } else if (envelope.type == "touch_move") {
    DispatchTouchMove(envelope.data, now);
  } else if (envelope.type == "touch_end") {
    DispatchTouchEnd(envelope.data, now, /*cancel=*/false);
  } else if (envelope.type == "touch_cancel") {
    DispatchTouchEnd(envelope.data, now, /*cancel=*/true);
  } else {
    // Not a touch envelope — silently drop. The composite delegate
    // (M4 R8) will route by type so this branch goes away.
  }
}

// ---------------------------------------------------------------------
// touch_start
// ---------------------------------------------------------------------

void CbInputDispatchTouch::DispatchTouchStart(const base::Value::Dict& data,
                                              base::TimeTicks event_time) {
  std::optional<CbActiveTouchPoint> parsed = ParseTouchPointData(data,
                                                                 event_time);
  if (!parsed.has_value()) {
    LOG(WARNING) << "touch_start missing required identifier/x/y; drop";
    return;
  }

  // Re-`touch_start` for an identifier we already track is treated as
  // an implicit cancel + start of a fresh finger on the same id. The
  // Go bridge silently overwrites; we do the same but log so the
  // operator can tell when a client is misbehaving.
  if (active_points_.contains(parsed->identifier)) {
    LOG(WARNING) << "touch_start for identifier already active (id="
                 << parsed->identifier << "); overwriting prior state";
  }

  // kTouchesLengthCap overflow guard. Only meaningful when we are
  // about to grow the set — if the id is already present we are
  // overwriting, not growing.
  if (!active_points_.contains(parsed->identifier) &&
      active_points_.size() >= kBlinkTouchPointsCap) {
    LOG(WARNING) << "touch_start would exceed blink kTouchesLengthCap="
                 << kBlinkTouchPointsCap << "; dropping id="
                 << parsed->identifier;
    // TODO(M4-R6-overflow-metric): wire a counter so the operator
    // can see how often clients send >16 simultaneous touches. v1
    // clients should never do this; if it shows up in prod a real
    // client is misbehaving.
    return;
  }

  const int id = parsed->identifier;
  active_points_[id] = *parsed;

  content::WebContents* wc =
      resolver_ ? resolver_->GetActiveWebContents() : nullptr;
  if (!wc) {
    LOG(WARNING) << "touch_start: no active WebContents from resolver; "
                 << "state map updated, dispatch skipped";
    return;
  }

  EnsureBroughtToFront(wc);

  auto* rwhv = wc->GetRenderWidgetHostView();
  content::RenderWidgetHost* rwh = rwhv ? rwhv->GetRenderWidgetHost() : nullptr;
  if (!rwh) {
    LOG(WARNING) << "touch_start: WebContents has no RenderWidgetHost; "
                 << "state map updated, dispatch skipped";
    return;
  }

  blink::WebTouchEvent event;
  if (!AssembleTouchEvent(blink::WebInputEvent::Type::kTouchStart, id,
                          blink::WebTouchPoint::State::kStatePressed,
                          /*released_point=*/nullptr, event_time, rwh,
                          &event)) {
    // AssembleTouchEvent only returns false on cap overflow, which
    // we already guarded above; log defensively in case the
    // assembler's accounting drifts from ours.
    LOG(ERROR) << "touch_start: AssembleTouchEvent unexpectedly failed";
    return;
  }
  ForwardWebTouchEvent(rwh, event);
}

// ---------------------------------------------------------------------
// touch_move
// ---------------------------------------------------------------------

void CbInputDispatchTouch::DispatchTouchMove(const base::Value::Dict& data,
                                             base::TimeTicks event_time) {
  std::optional<CbActiveTouchPoint> parsed = ParseTouchPointData(data,
                                                                 event_time);
  if (!parsed.has_value()) {
    LOG(WARNING) << "touch_move missing required identifier/x/y; drop";
    return;
  }

  auto it = active_points_.find(parsed->identifier);
  if (it == active_points_.end()) {
    // Spec: "The server MUST treat touch_move/touch_end/touch_cancel
    // for an identifier it has not seen touch_start for as a no-op +
    // warning." Mirror input-bridge/main.go.
    LOG(WARNING) << "touch_move for unknown identifier (id="
                 << parsed->identifier << "); drop";
    return;
  }

  it->second = *parsed;

  content::WebContents* wc =
      resolver_ ? resolver_->GetActiveWebContents() : nullptr;
  if (!wc) {
    LOG(WARNING) << "touch_move: no active WebContents from resolver; "
                 << "state map updated, dispatch skipped";
    return;
  }
  auto* rwhv = wc->GetRenderWidgetHostView();
  content::RenderWidgetHost* rwh = rwhv ? rwhv->GetRenderWidgetHost() : nullptr;
  if (!rwh) {
    LOG(WARNING) << "touch_move: no RenderWidgetHost; dispatch skipped";
    return;
  }

  blink::WebTouchEvent event;
  if (!AssembleTouchEvent(blink::WebInputEvent::Type::kTouchMove,
                          parsed->identifier,
                          blink::WebTouchPoint::State::kStateMoved,
                          /*released_point=*/nullptr, event_time, rwh,
                          &event)) {
    LOG(ERROR) << "touch_move: AssembleTouchEvent failed";
    return;
  }
  ForwardWebTouchEvent(rwh, event);
}

// ---------------------------------------------------------------------
// touch_end / touch_cancel
// ---------------------------------------------------------------------

void CbInputDispatchTouch::DispatchTouchEnd(const base::Value::Dict& data,
                                            base::TimeTicks event_time,
                                            bool cancel) {
  std::optional<int> id_opt = ParseTouchEndIdentifier(data);
  if (!id_opt.has_value()) {
    LOG(WARNING) << "touch_" << (cancel ? "cancel" : "end")
                 << " missing required identifier; drop";
    return;
  }
  const int id = *id_opt;

  auto it = active_points_.find(id);
  if (it == active_points_.end()) {
    LOG(WARNING) << "touch_" << (cancel ? "cancel" : "end")
                 << " for unknown identifier (id=" << id << "); drop";
    return;
  }

  // Snapshot the released finger before erasing — the WebTouchEvent
  // we assemble below MUST include the released point with
  // state=kStateReleased / kStateCancelled even though it is no
  // longer in the active map. (DOM TouchEvent.changedTouches contract:
  // the changed list includes the removed point on touchend.)
  const CbActiveTouchPoint released = it->second;
  active_points_.erase(it);

  content::WebContents* wc =
      resolver_ ? resolver_->GetActiveWebContents() : nullptr;
  if (!wc) {
    LOG(WARNING) << "touch_" << (cancel ? "cancel" : "end")
                 << ": no active WebContents from resolver; state map "
                 << "updated, dispatch skipped";
    return;
  }
  auto* rwhv = wc->GetRenderWidgetHostView();
  content::RenderWidgetHost* rwh = rwhv ? rwhv->GetRenderWidgetHost() : nullptr;
  if (!rwh) {
    LOG(WARNING) << "touch_" << (cancel ? "cancel" : "end")
                 << ": no RenderWidgetHost; dispatch skipped";
    return;
  }

  const blink::WebInputEvent::Type type =
      cancel ? blink::WebInputEvent::Type::kTouchCancel
             : blink::WebInputEvent::Type::kTouchEnd;
  const blink::WebTouchPoint::State point_state =
      cancel ? blink::WebTouchPoint::State::kStateCancelled
             : blink::WebTouchPoint::State::kStateReleased;

  blink::WebTouchEvent event;
  if (!AssembleTouchEvent(type, id, point_state, &released, event_time, rwh,
                          &event)) {
    LOG(ERROR) << "touch_" << (cancel ? "cancel" : "end")
               << ": AssembleTouchEvent failed";
    return;
  }
  ForwardWebTouchEvent(rwh, event);
}

// ---------------------------------------------------------------------
// WebTouchEvent assembly
// ---------------------------------------------------------------------

bool CbInputDispatchTouch::AssembleTouchEvent(
    blink::WebInputEvent::Type type,
    int changed_identifier,
    blink::WebTouchPoint::State changed_state,
    const CbActiveTouchPoint* released_point,
    base::TimeTicks event_time,
    content::RenderWidgetHost* rwh,
    blink::WebTouchEvent* out) {
  DCHECK(out);

  // Total point count for this event: every still-active finger plus
  // the released one (if any). The released finger has already been
  // removed from active_points_ by the caller for end/cancel; on
  // start/move released_point is null and we use the active map size
  // directly.
  const size_t total = active_points_.size() + (released_point ? 1u : 0u);
  if (total > kBlinkTouchPointsCap) {
    return false;
  }

  *out = blink::WebTouchEvent(
      type, blink::WebInputEvent::kNoModifiers, event_time);
  out->touches_length = static_cast<unsigned>(total);
  out->dispatch_type = blink::WebInputEvent::DispatchType::kBlocking;
  out->moved_beyond_slop_region = (type == blink::WebInputEvent::Type::kTouchMove);

  // Fill touches[] from the active map first, then append the
  // released point at the tail if present. Order within touches[]
  // is not spec-mandated by chromium — blink keys per-finger
  // identity off WebTouchPoint.id, not array index — but a stable
  // iteration order makes test traces comparable across runs.
  size_t i = 0;
  for (const auto& [id, point] : active_points_) {
    if (i >= kBlinkTouchPointsCap) {
      // Belt-and-braces: total bound above should already prevent
      // this; guard so any accounting drift fails closed.
      break;
    }
    WidgetPoint wp = ContentToWidget(rwh, point.x, point.y);
    const bool is_changed = (id == changed_identifier);
    FillWebTouchPoint(
        point, wp,
        is_changed ? changed_state
                   : blink::WebTouchPoint::State::kStateStationary,
        &out->touches[i]);
    ++i;
  }
  if (released_point && i < kBlinkTouchPointsCap) {
    WidgetPoint wp = ContentToWidget(rwh, released_point->x,
                                     released_point->y);
    FillWebTouchPoint(*released_point, wp, changed_state, &out->touches[i]);
    ++i;
  }

  last_assembled_event_for_testing_ = *out;
  return true;
}

void CbInputDispatchTouch::FillWebTouchPoint(
    const CbActiveTouchPoint& src,
    WidgetPoint widget_pos,
    blink::WebTouchPoint::State state,
    blink::WebTouchPoint* out) {
  // WebTouchPoint inherits WebPointerProperties; we set the touch-
  // specific fields plus the pointer-properties basics chromium uses
  // for hit testing.
  out->id = src.identifier;
  out->state = state;
  out->pointer_type = blink::WebPointerProperties::PointerType::kTouch;
  out->position_in_widget = gfx::PointF(widget_pos.x, widget_pos.y);
  // Screen-space position. We don't have the screen offset of the
  // captured widget here (chromium computes that lazily for synthetic
  // events); the FSVC framing is window-local so identifying
  // position_in_screen with position_in_widget matches what a
  // real touch on the captured rect would deliver.
  //
  // TODO(M4-R6-screen-space): once M4 R2 exposes the captured-window
  // origin, add it here so multi-monitor configurations dispatch
  // with correct screen coords.
  out->position_in_screen = out->position_in_widget;
  out->radius_x = static_cast<float>(src.radius_x);
  out->radius_y = static_cast<float>(src.radius_y);
  out->rotation_angle = static_cast<float>(src.twist);
  out->force = src.force;
}

// ---------------------------------------------------------------------
// Forwarding + ancillary
// ---------------------------------------------------------------------

bool CbInputDispatchTouch::ForwardWebTouchEvent(
    content::RenderWidgetHost* rwh,
    const blink::WebTouchEvent& event) {
  if (!rwh) {
    return false;
  }
  // RenderWidgetHostImpl owns the touch-event ack queue + ack timeout
  // that the public RenderWidgetHost API hides. Casting matches the
  // M4 R3 mouse path. See TODO(M4-R6-impl-vs-public-api) in the
  // header for the long-term plan.
  auto* impl = content::RenderWidgetHostImpl::From(rwh);
  if (!impl) {
    return false;
  }
  impl->ForwardTouchEventWithLatencyInfo(event, ui::LatencyInfo());
  return true;
}

void CbInputDispatchTouch::EnsureBroughtToFront(content::WebContents* wc) {
  if (brought_to_front_ || !wc) {
    return;
  }
  // Same pair as M4 R3: WasShown drops the document out of the
  // hidden-state, Focus() routes the input event to the focused
  // frame. Either alone is insufficient.
  //
  // TODO(M4-R6-shared-bring-to-front): see header.
  wc->WasShown();
  if (auto* view = wc->GetRenderWidgetHostView()) {
    view->Focus();
  }
  brought_to_front_ = true;
}

CbInputDispatchTouch::WidgetPoint CbInputDispatchTouch::ContentToWidget(
    content::RenderWidgetHost* rwh,
    int content_x,
    int content_y) {
  // Match M4 R3's coord path. DSF=1 at the FSVC's 1280x720 host
  // capture, so this is a no-op there; the divide is for hi-DPI
  // captures we don't ship in M4 but the math is honest about.
  float dsf = 1.f;
  if (rwh) {
    if (auto* view = rwh->GetView()) {
      dsf = view->GetDeviceScaleFactor();
      if (dsf <= 0.f) {
        dsf = 1.f;
      }
    }
  }
  return WidgetPoint{static_cast<float>(content_x) / dsf,
                     static_cast<float>(content_y) / dsf};
}

}  // namespace cloud_browser

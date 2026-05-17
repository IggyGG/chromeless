// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cloud-browser/capture/build-integration/cb_input_dispatch_mouse.h"

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
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "ui/gfx/geometry/point_f.h"

namespace cloud_browser {

namespace {

// Protocol → blink::WebPointerProperties::Button mapping. Mirrors
// input-bridge/main.go's protocolButtonToCDP but in native types.
//
// Protocol indices (from docs/protocols/input-channel.md):
//   0 left, 1 middle, 2 right, 3 back, 4 forward
blink::WebPointerProperties::Button ProtocolButtonToBlink(int protocol_button) {
  using Button = blink::WebPointerProperties::Button;
  switch (protocol_button) {
    case 0:
      return Button::kLeft;
    case 1:
      return Button::kMiddle;
    case 2:
      return Button::kRight;
    case 3:
      return Button::kBack;
    case 4:
      return Button::kForward;
    default:
      return Button::kNoButton;
  }
}

// Protocol button index → blink::WebInputEvent::Modifiers held-button
// bit. Returns 0 for an unknown button.
uint32_t ProtocolButtonToBlinkHeldBit(int protocol_button) {
  using Mods = blink::WebInputEvent::Modifiers;
  switch (protocol_button) {
    case 0:
      return Mods::kLeftButtonDown;
    case 1:
      return Mods::kMiddleButtonDown;
    case 2:
      return Mods::kRightButtonDown;
    case 3:
      return Mods::kBackButtonDown;
    case 4:
      return Mods::kForwardButtonDown;
    default:
      return 0;
  }
}

// Active button for a mouse_move based on held-button bits — chromium's
// drag detector inspects WebMouseEvent::button (singular) in addition
// to the modifiers field, so we surface the first held button if any.
// Matches the Go bridge's left > right > middle priority order.
blink::WebPointerProperties::Button ButtonFromHeldBits(uint32_t held_bits) {
  using Button = blink::WebPointerProperties::Button;
  using Mods = blink::WebInputEvent::Modifiers;
  if (held_bits & Mods::kLeftButtonDown) {
    return Button::kLeft;
  }
  if (held_bits & Mods::kRightButtonDown) {
    return Button::kRight;
  }
  if (held_bits & Mods::kMiddleButtonDown) {
    return Button::kMiddle;
  }
  if (held_bits & Mods::kBackButtonDown) {
    return Button::kBack;
  }
  if (held_bits & Mods::kForwardButtonDown) {
    return Button::kForward;
  }
  return Button::kNoButton;
}

// Resolve the per-envelope wheel delta_mode → blink::ui::ScrollGranularity
// equivalent (kScrollByPixel / kScrollByLine / kScrollByPage). The
// spec mandates the `delta_mode` string overrides the numeric `mode`
// when both are present.
//
// Returns the scaling factor (pixel multiplier) to apply to dx/dy.
float WheelScalingFor(int mode, const std::string* delta_mode) {
  // `delta_mode` overrides `mode`.
  if (delta_mode) {
    if (*delta_mode == "pixel") {
      return 1.f;
    }
    if (*delta_mode == "line") {
      return 16.f;  // kWheelLinePx — duplicated here so this helper
                    // stays anonymous-namespace local.
    }
    if (*delta_mode == "page") {
      return 800.f;  // kWheelPagePx.
    }
    // Unknown delta_mode → fall through to numeric mode.
  }
  switch (mode) {
    case 0:
      return 1.f;
    case 1:
      return 16.f;
    case 2:
      return 800.f;
    default:
      return 1.f;  // Defensive — unknown modes treated as pixel.
  }
}

}  // namespace

// ---------------------------------------------------------------------
// CbInputDispatchMouse
// ---------------------------------------------------------------------

CbInputDispatchMouse::CbInputDispatchMouse(WebContentsResolver* resolver)
    : resolver_(resolver) {
  // resolver_ may be null during M4 R2 development; we tolerate that
  // and log per envelope so the operator can see the gap. DCHECK is
  // intentionally NOT placed here — that would block the R3 unit
  // test from constructing the dispatcher without R2.
}

CbInputDispatchMouse::~CbInputDispatchMouse() = default;

void CbInputDispatchMouse::OnInputEvent(InputEnvelope envelope) {
  // Already on BrowserThread::UI (M4 R1 hopped before invoking us).
  // Type-dispatch and ignore non-mouse envelopes — the composite
  // delegate that wraps R3+R4+R5+... routes those elsewhere.
  const base::TimeTicks now = base::TimeTicks::Now();
  if (envelope.type == "mouse_move") {
    DispatchMouseMove(envelope.data, now);
  } else if (envelope.type == "mouse_button") {
    DispatchMouseButton(envelope.data, now);
  } else if (envelope.type == "mouse_wheel") {
    DispatchMouseWheel(envelope.data, now);
  }
  // else: silently ignore; not R3's type.
}

void CbInputDispatchMouse::DispatchMouseMove(const base::Value::Dict& data,
                                             base::TimeTicks event_time) {
  if (!resolver_) {
    LOG(WARNING) << "CbInputDispatchMouse: mouse_move dropped — no "
                 << "WebContentsResolver wired (M4 R2 pending)";
    return;
  }
  content::WebContents* wc = resolver_->GetActiveWebContents();
  if (!wc) {
    LOG(WARNING) << "CbInputDispatchMouse: mouse_move dropped — no "
                 << "active WebContents";
    return;
  }
  auto* rwhv = wc->GetRenderWidgetHostView();
  content::RenderWidgetHost* rwh = rwhv ? rwhv->GetRenderWidgetHost() : nullptr;
  if (!rwh) {
    LOG(WARNING) << "CbInputDispatchMouse: mouse_move dropped — no RWH";
    return;
  }

  EnsureBroughtToFront(wc);

  // Protocol shape: { "x": int, "y": int } — see input-channel.md.
  const std::optional<int> x = data.FindInt("x");
  const std::optional<int> y = data.FindInt("y");
  if (!x || !y) {
    LOG(WARNING) << "CbInputDispatchMouse: mouse_move missing x/y";
    return;
  }
  const WidgetPoint widget = ContentToWidget(rwh, *x, *y);

  blink::WebMouseEvent event(
      blink::WebInputEvent::Type::kMouseMove,
      gfx::PointF(widget.x, widget.y),
      gfx::PointF(widget.x, widget.y),
      // button — chromium's drag detector consults this for
      // "is this move part of a drag". Pull from the first held
      // button (matches Go bridge priority).
      ButtonFromHeldBits(held_buttons_blink_),
      /*click_count=*/0,
      static_cast<int>(held_buttons_blink_ | held_modifiers_blink_),
      event_time);
  // pointer_type → kMouse is the default; left explicit when an
  // upstream constant lands. TODO(M4-R3-pointer-type).

  rwh->ForwardMouseEvent(event);

  last_pointer_.x = widget.x;
  last_pointer_.y = widget.y;
  last_pointer_.buttons_blink = held_buttons_blink_;
  last_pointer_.at = event_time;
}

void CbInputDispatchMouse::DispatchMouseButton(const base::Value::Dict& data,
                                               base::TimeTicks event_time) {
  if (!resolver_) {
    LOG(WARNING) << "CbInputDispatchMouse: mouse_button dropped — no "
                 << "WebContentsResolver wired (M4 R2 pending)";
    return;
  }
  content::WebContents* wc = resolver_->GetActiveWebContents();
  if (!wc) {
    LOG(WARNING) << "CbInputDispatchMouse: mouse_button dropped — no "
                 << "active WebContents";
    return;
  }
  auto* rwhv = wc->GetRenderWidgetHostView();
  content::RenderWidgetHost* rwh = rwhv ? rwhv->GetRenderWidgetHost() : nullptr;
  if (!rwh) {
    LOG(WARNING) << "CbInputDispatchMouse: mouse_button dropped — no RWH";
    return;
  }

  EnsureBroughtToFront(wc);

  // Protocol shape: { "button": int, "action": "down"|"up", "x": int,
  // "y": int }.
  const std::optional<int> button_proto = data.FindInt("button");
  const std::string* action = data.FindString("action");
  const std::optional<int> x = data.FindInt("x");
  const std::optional<int> y = data.FindInt("y");
  if (!button_proto || !action || !x || !y) {
    LOG(WARNING) << "CbInputDispatchMouse: mouse_button missing field";
    return;
  }
  const bool is_down = *action == "down";
  const bool is_up = *action == "up";
  if (!is_down && !is_up) {
    LOG(WARNING) << "CbInputDispatchMouse: mouse_button invalid action="
                 << *action;
    return;
  }

  const blink::WebPointerProperties::Button blink_button =
      ProtocolButtonToBlink(*button_proto);
  const uint32_t held_bit = ProtocolButtonToBlinkHeldBit(*button_proto);

  // Click-ramp + held-button updates. Mirrors input-bridge/main.go
  // exactly so the CDP and native backends produce parity click /
  // dblclick / tripleclick sequences against the same envelope
  // stream.
  int click_count = 1;
  if (is_down) {
    const base::TimeDelta elapsed = event_time - last_click_.at;
    const int dx = std::abs(*x - last_click_.x);
    const int dy = std::abs(*y - last_click_.y);
    if (last_click_.protocol_button == *button_proto &&
        elapsed <= kClickRampWindow &&
        dx <= kClickRampSlopPx && dy <= kClickRampSlopPx &&
        last_click_.count > 0) {
      click_count = last_click_.count + 1;
    }
    last_click_.protocol_button = *button_proto;
    last_click_.x = *x;
    last_click_.y = *y;
    last_click_.at = event_time;
    last_click_.count = click_count;
    held_buttons_blink_ |= held_bit;
  } else {
    // `up` — report the ramp count of the matching `down` so the
    // renderer fires `click` (count==1) / `dblclick` (count>=2).
    if (last_click_.protocol_button == *button_proto &&
        last_click_.count > 0) {
      click_count = last_click_.count;
    }
    held_buttons_blink_ &= ~held_bit;
  }

  const WidgetPoint widget = ContentToWidget(rwh, *x, *y);
  blink::WebMouseEvent event(
      is_down ? blink::WebInputEvent::Type::kMouseDown
              : blink::WebInputEvent::Type::kMouseUp,
      gfx::PointF(widget.x, widget.y),
      gfx::PointF(widget.x, widget.y),
      blink_button,
      click_count,
      static_cast<int>(held_buttons_blink_ | held_modifiers_blink_),
      event_time);

  rwh->ForwardMouseEvent(event);

  last_pointer_.x = widget.x;
  last_pointer_.y = widget.y;
  last_pointer_.buttons_blink = held_buttons_blink_;
  last_pointer_.at = event_time;
}

void CbInputDispatchMouse::DispatchMouseWheel(const base::Value::Dict& data,
                                              base::TimeTicks event_time) {
  if (!resolver_) {
    LOG(WARNING) << "CbInputDispatchMouse: mouse_wheel dropped — no "
                 << "WebContentsResolver wired (M4 R2 pending)";
    return;
  }
  content::WebContents* wc = resolver_->GetActiveWebContents();
  if (!wc) {
    LOG(WARNING) << "CbInputDispatchMouse: mouse_wheel dropped — no "
                 << "active WebContents";
    return;
  }
  auto* rwhv = wc->GetRenderWidgetHostView();
  content::RenderWidgetHost* rwh = rwhv ? rwhv->GetRenderWidgetHost() : nullptr;
  if (!rwh) {
    LOG(WARNING) << "CbInputDispatchMouse: mouse_wheel dropped — no RWH";
    return;
  }

  EnsureBroughtToFront(wc);

  // Protocol shape: { "dx": int, "dy": int, "mode": int, "x": int,
  // "y": int, ["delta_mode": string], ["phase": string],
  // ["momentum": bool] }.
  const std::optional<int> dx = data.FindInt("dx");
  const std::optional<int> dy = data.FindInt("dy");
  const std::optional<int> mode = data.FindInt("mode");
  const std::optional<int> x = data.FindInt("x");
  const std::optional<int> y = data.FindInt("y");
  if (!dx || !dy || !mode || !x || !y) {
    LOG(WARNING) << "CbInputDispatchMouse: mouse_wheel missing field";
    return;
  }
  const std::string* delta_mode = data.FindString("delta_mode");
  const std::string* phase = data.FindString("phase");

  const WidgetPoint widget = ContentToWidget(rwh, *x, *y);

  // Phase machine. v1.0 clients omit `phase`; we treat as
  // kPhaseChanged and never synth Begin/End. v1.1 clients drive
  // through start → changed* → end.
  blink::WebMouseWheelEvent::Phase blink_phase =
      blink::WebMouseWheelEvent::kPhaseChanged;
  bool synth_zero_delta = false;
  if (phase) {
    if (*phase == "start") {
      blink_phase = blink::WebMouseWheelEvent::kPhaseBegan;
      wheel_in_gesture_ = true;
      // Per spec: phase=start synthesises a zero-delta Begin so
      // chromium's compositor sets up its gesture tracking before
      // any actual delta arrives. The same envelope's dx/dy (if
      // non-zero) would be lost here, but in practice phase=start
      // envelopes carry dx=dy=0 — the next envelope (phase=changed
      // or phase omitted) carries the first real delta.
      synth_zero_delta = true;
    } else if (*phase == "end") {
      blink_phase = blink::WebMouseWheelEvent::kPhaseEnded;
      wheel_in_gesture_ = false;
      // Terminal zero-delta — flushes any momentum decay in the
      // compositor. AC: "phase=end emits terminal zero-delta".
      synth_zero_delta = true;
    } else if (*phase == "changed") {
      // First post-start `changed` would ideally remain kPhaseBegan
      // if we haven't synthesised a Begin yet; but since phase=start
      // already synthesised one above, kPhaseChanged is correct.
      blink_phase = blink::WebMouseWheelEvent::kPhaseChanged;
    }
    // Unknown phase strings fall through to kPhaseChanged.
  }

  const float scale = WheelScalingFor(*mode, delta_mode);
  const float scaled_dx = synth_zero_delta ? 0.f : (*dx * scale);
  const float scaled_dy = synth_zero_delta ? 0.f : (*dy * scale);

  blink::WebMouseWheelEvent event(
      blink::WebInputEvent::Type::kMouseWheel,
      static_cast<int>(held_buttons_blink_ | held_modifiers_blink_),
      event_time);
  event.SetPositionInWidget(widget.x, widget.y);
  event.SetPositionInScreen(widget.x, widget.y);
  event.delta_x = scaled_dx;
  event.delta_y = scaled_dy;
  // wheel_ticks_* carry the un-scaled (line/page count) intent for
  // chromium's per-platform smoothing. For pixel mode this equals
  // the delta; for line/page it's the raw integer count.
  event.wheel_ticks_x = (scale == 1.f) ? scaled_dx : static_cast<float>(*dx);
  event.wheel_ticks_y = (scale == 1.f) ? scaled_dy : static_cast<float>(*dy);
  event.phase = blink_phase;
  // momentum_phase: v1.1 protocol's `momentum` bool maps onto
  // blink's kPhaseChanged momentum phase. Default = kPhaseNone for
  // non-momentum events; flip to kPhaseChanged if the envelope
  // marks `momentum=true`. End-of-momentum is signaled by the same
  // phase=end envelope flow above.
  //
  // TODO(M4-R3-momentum-end): the spec doesn't (yet) carry a
  // separate momentum-end signal; current behaviour collapses
  // momentum-end into the regular phase=end. Re-check once a real
  // trackpad client drives this.
  const std::optional<bool> momentum = data.FindBool("momentum");
  if (momentum.value_or(false)) {
    event.momentum_phase = blink::WebMouseWheelEvent::kPhaseChanged;
  } else {
    event.momentum_phase = blink::WebMouseWheelEvent::kPhaseNone;
  }
  // delta_units. blink defaults to kScrollByPrecisePixel for
  // pixel deltas; for line/page we still report pixel because we
  // scale up to pixels above. A future revision should switch to
  // kScrollByPage / kScrollByLine + per-platform scaling so chromium
  // can honor the OS's "scroll one line" preference.
  //
  // TODO(M4-R3-delta-units): swap to per-mode delta_units once the
  // spec confirms whether the client or the bridge owns the scaling.
  event.delta_units = ui::ScrollGranularity::kScrollByPrecisePixel;

  rwh->ForwardWheelEvent(event);

  last_pointer_.x = widget.x;
  last_pointer_.y = widget.y;
  last_pointer_.buttons_blink = held_buttons_blink_;
  last_pointer_.at = event_time;
}

void CbInputDispatchMouse::EnsureBroughtToFront(content::WebContents* wc) {
  if (brought_to_front_) {
    return;
  }
  if (!wc) {
    return;
  }
  // WasShown() drives the visibility state; the renderer ignores
  // input on hidden documents. Focus() on the RWHV ensures the focus
  // chain (see cb_aura_platform_data.cc — BUGS-529 second-layer fix)
  // points at the WebContents we're about to dispatch into.
  wc->WasShown();
  if (auto* view = wc->GetRenderWidgetHostView()) {
    view->Focus();
  }
  brought_to_front_ = true;
}

CbInputDispatchMouse::WidgetPoint
CbInputDispatchMouse::ContentToWidget(content::RenderWidgetHost* rwh,
                                      int content_x, int content_y) {
  // Default: DSF=1, no-op cast. Aura host @ 1280x720 with the FSVC
  // capturing post-object-fit produces protocol coords that already
  // align with widget DIPs in the common path.
  float dsf = 1.f;
  if (auto* view = rwh ? rwh->GetView() : nullptr) {
    const float view_dsf = view->GetDeviceScaleFactor();
    // GetDeviceScaleFactor returns 0 if the view doesn't have a
    // backing screen yet (early-startup race). Default to 1 in that
    // case so the first dispatch doesn't divide by zero.
    if (view_dsf > 0.f) {
      dsf = view_dsf;
    }
  }
  return {
      static_cast<float>(content_x) / dsf,
      static_cast<float>(content_y) / dsf,
  };
}

}  // namespace cloud_browser

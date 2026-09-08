// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "capture/build-integration/cb_viewport_controller.h"

#include "capture/build-integration/cb_begin_frame_driver.h"  // CV2-RESIZE

#include <algorithm>

#include "base/logging.h"
#include "capture/build-integration/cb_aura_platform_data.h"
#include "capture/build-integration/cb_headless_screen.h"
#include "capture/framesink-capturer/cb_framesink_video_track_source.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "ui/display/display.h"  // display::Display in the ctor seed
#include "ui/aura/window_tree_host.h"
#include "ui/gfx/geometry/rect.h"

namespace cloud_browser {

namespace {

// Even-align down. I420 subsamples chroma 2x2; an odd dimension leaves a
// half-sampled edge row/column. Down rather than up so clamping can never
// push a request back OVER a ceiling it was just clamped to.
int EvenAlignDown(int v) {
  return v & ~1;
}

}  // namespace

// static
CbViewportSpec CbViewportController::Clamp(const CbViewportSpec& spec) {
  CbViewportSpec out;

  const int w = std::clamp(spec.size_dip.width(), kMinViewportDimension,
                           kMaxViewportDimension);
  const int h = std::clamp(spec.size_dip.height(), kMinViewportDimension,
                           kMaxViewportDimension);
  out.size_dip = gfx::Size(EvenAlignDown(w), EvenAlignDown(h));

  // DSF is pinned until input-coordinate normalization lands. A request
  // above the cap is honoured at the cap rather than rejected: the caller
  // gets a working (if non-HiDPI) session, and the returned spec tells
  // them what actually happened.
  out.device_scale_factor =
      std::clamp(spec.device_scale_factor, 1.0f, kMaxDeviceScaleFactor);

  return out;
}

CbViewportController::CbViewportController(
    CbHeadlessScreen* screen,
    CbAuraPlatformData* aura,
    CloudBrowserFrameSinkVideoTrackSource* track_source)
    : screen_(screen), aura_(aura), track_source_(track_source) {
  // Seed from what the embedder actually booted with, so current() is
  // truthful before the first Apply() rather than reporting a default
  // that may not match the tree.
  if (screen_) {
    const display::Display primary = screen_->GetPrimaryDisplay();
    current_.size_dip = primary.size();
    current_.device_scale_factor = primary.device_scale_factor();
  }
  LOG(INFO) << "CbViewportController: initial viewport "
            << current_.size_dip.ToString() << " @"
            << current_.device_scale_factor << "x";
}

CbViewportController::~CbViewportController() = default;

void CbViewportController::SetBeginFrameDriver(CbBeginFrameDriver* driver) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  begin_frame_driver_ = driver;
}

void CbViewportController::SetTrackSource(
    CloudBrowserFrameSinkVideoTrackSource* track_source) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  track_source_ = track_source;
}

void CbViewportController::SetTargetWebContents(
    content::WebContents* web_contents) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  web_contents_ = web_contents;
}

CbViewportSpec CbViewportController::Apply(const CbViewportSpec& spec,
                                           const std::string& reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Resolve the "keep current scale" sentinel BEFORE clamping — Clamp is
  // static and has no idea what the current scale is, and a clamp of 0.0
  // would floor it to 1.0, which is precisely the silent HiDPI-drop the
  // sentinel exists to prevent.
  CbViewportSpec resolved = spec;
  if (resolved.device_scale_factor == kViewportKeepCurrentScale) {
    resolved.device_scale_factor = current_.device_scale_factor;
  }

  const CbViewportSpec target = Clamp(resolved);

  if (target.size_dip != resolved.size_dip ||
      target.device_scale_factor != resolved.device_scale_factor) {
    LOG(WARNING) << "CbViewportController: clamped request "
                 << resolved.size_dip.ToString() << " @"
                 << resolved.device_scale_factor << "x -> "
                 << target.size_dip.ToString() << " @"
                 << target.device_scale_factor << "x (" << reason << ")";
  }

  if (target.size_dip == current_.size_dip &&
      target.device_scale_factor == current_.device_scale_factor) {
    return target;  // Nothing to do; every layer already agrees.
  }

  const gfx::Size size_px(
      static_cast<int>(target.size_dip.width() * target.device_scale_factor),
      static_cast<int>(target.size_dip.height() * target.device_scale_factor));

  LOG(INFO) << "CbViewportController: applying "
            << current_.size_dip.ToString() << " @"
            << current_.device_scale_factor << "x -> "
            << target.size_dip.ToString() << " @"
            << target.device_scale_factor << "x (pixels "
            << size_px.ToString() << ") reason=" << reason;

  // ── 0. The BeginFrame driver, BEFORE anything touches the Display. ──
  //
  // Steps 1-2 reconfigure viz's Display (UpdatePrimaryDisplay →
  // OnDisplayMetricsChanged, SetBoundsInPixels → Compositor::SetScaleAndSize
  // → Display::Resize). That drops the external BeginFrame in flight, and
  // the driver's stall watchdog would then re-issue into it 15 s later and
  // abort the GPU process. Measured 2026-09-07, three for three on fresh
  // guests; the browser restarted ~50 s after every connect from a real
  // window. The driver abandons the in-flight frame and restarts itself
  // once the Display has settled.
  if (begin_frame_driver_) {
    begin_frame_driver_->NotifyDisplayReconfigured(reason);
  }

  // ── 1. The display, FIRST. ──
  //
  // Everything downstream reads DSF and screen geometry off it —
  // RenderWidgetHostViewAura converts DIP<->pixels using the display its
  // window is on. Resizing the host before the display is updated makes
  // that conversion use the OLD scale for one pass, which at DSF != 1
  // produces a view sized wrong by exactly the ratio, then corrected on
  // the next frame: a visible one-frame jump.
  //
  // UpdatePrimaryDisplay fires OnDisplayMetricsChanged (see its header
  // comment for why that matters and why display_list() mutation does
  // not substitute).
  if (screen_) {
    screen_->UpdatePrimaryDisplay(gfx::Rect(size_px),
                                  target.device_scale_factor);
  }

  // ── 2. The aura host, in PIXELS. ──
  //
  // This moves the root window and the platform window under it. Note
  // this alone does NOT resize the child WebContents view: FillLayout's
  // OnWindowResized has a has_bounds_ latch that is already true (the
  // ctor sets bounds before installing the layout manager), so it never
  // fires. That is not a bug to fix here — upstream content_shell has
  // the identical latch and also resizes its content explicitly, in
  // ShellPlatformDelegate::ResizeWebContent. Step 3 is that step.
  if (aura_ && aura_->host()) {
    aura_->host()->SetBoundsInPixels(gfx::Rect(size_px));
  }

  // ── 3. The WebContents view, in DIP. ──
  //
  // The mirror of upstream's ResizeWebContent
  // (shell_platform_delegate_aura.cc:69 — `GetRenderWidgetHostView()
  // ->SetSize(content_size)`). This is what actually tells the renderer
  // its new viewport, so window.innerWidth and layout follow.
  if (web_contents_) {
    if (content::RenderWidgetHostView* rwhv =
            web_contents_->GetRenderWidgetHostView()) {
      rwhv->SetSize(target.size_dip);
    }
  }

  // ── 4. The capturer, in PIXELS, LAST. ──
  //
  // Last because it requests a refresh frame: doing it before the
  // compositor has the new size would capture the old surface at the new
  // constraints, i.e. one letterboxed or cropped frame on every resize.
  //
  // The encoder re-inits itself when a frame arrives at a new geometry.
  // That path used to dereference a null callback and take the browser
  // process with it — see the fix in capture/encoder/*, which is a hard
  // prerequisite for this line existing at all.
  if (track_source_) {
    track_source_->SetCaptureResolution(size_px);
  }

  current_ = target;
  return target;
}

}  // namespace cloud_browser

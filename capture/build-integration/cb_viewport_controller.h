// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbViewportController — one place that owns "how big is the browser".
//
// Before this, the answer was pinned in four places that could not
// disagree because none of them could change:
//
//   * the embedder seeded a 1280x720 display at DSF 1.0
//     (cloud_browser_browser_main_parts.cc kDefaultDisplayWidth/Height),
//   * the aura host was constructed at 1280x720,
//   * the FrameSink capturer pinned min==max at 1280x720,
//   * the guest's Xvfb was a different size again (1920x1080).
//
// So the stream was a fixed letterbox no matter how large the viewer's
// window was — the most visible "this is not a real browser" tell we have.
// This class makes that size movable, and moves all of it together.
//
// ─── WHY NOT THE OBVIOUS ALTERNATIVES ─────────────────────────────────
//
// NOT `Emulation.setDeviceMetricsOverride`. It is page-scoped, and the
// isolator deliberately holds a BROWSER-scope CDP session (it does not
// attach per page). It also has to be re-applied after every navigation,
// and — the disqualifier — it does not move the aura root, so hit-testing
// would drift out of agreement with the pixels the user is looking at.
// Clicks would land in the wrong place, which is worse than a letterbox.
//
// NOT `RenderWidgetHostView::SetSize` alone. That resizes the view and
// leaves the display and the aura root stale: three layers, two answers.
// `window.devicePixelRatio` and `screen.width` would keep reporting the
// old geometry.
//
// The mechanism is: update the display (with observer fan-out), resize
// the aura host, resize the view, then re-pin the capturer. In that
// order, for the reasons documented on Apply().
//
// ─── DIP vs PIXELS: THE THING TO GET RIGHT ────────────────────────────
//
// This is the easiest bug to write here, so the convention is explicit
// and total:
//
//   CbViewportSpec::size_dip     DIP    — what the page thinks it is
//   display bounds               PIXELS — SetScaleAndBounds takes pixels
//   WindowTreeHost::SetBoundsInPixels  PIXELS (it says so)
//   RenderWidgetHostView::SetSize      DIP
//   capturer resolution                PIXELS — it is the encoded frame
//
// pixels = dip * dsf. At DSF 1.0 every one of these is numerically the
// same, which is exactly why a DSF-1.0-only test suite cannot catch a
// mistake here. Ship DSF 1.0 first (see kMaxDeviceScaleFactor), then
// lift it with the input-coordinate work.
//
// ─── OWNERSHIP ────────────────────────────────────────────────────────
//
// Owned by CloudBrowserBrowserMainParts, which outlives every object
// this holds a raw pointer to and clears them in PostMainMessageLoopRun.
// All calls are on the UI thread.

#ifndef CAPTURE_BUILD_INTEGRATION_CB_VIEWPORT_CONTROLLER_H_
#define CAPTURE_BUILD_INTEGRATION_CB_VIEWPORT_CONTROLLER_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/sequence_checker.h"
#include "ui/gfx/geometry/size.h"

namespace content {
class WebContents;
}  // namespace content

namespace cloud_browser {

class CbAuraPlatformData;
class CbHeadlessScreen;
class CloudBrowserFrameSinkVideoTrackSource;

// Bounds on what a caller may ask for.
//
// The floor is not defensive politeness — a zero or negative dimension
// reaches x264/libvpx as a zero-sized encode, and the capturer's
// even-alignment would round a 1px request to 0.
//
// The ceiling exists because the guest is a Firecracker microVM with 2048
// MB and (today) ONE vCPU. A 4K software encode there does not degrade
// gracefully, it wedges: is_screencast() selects MAINTAIN_RESOLUTION, so
// libwebrtc drops frame rate rather than downscaling, and the session
// becomes a slideshow that never recovers. Physics is expected to apply
// its own per-tier cap on top of this; this is the backstop for when it
// does not.
inline constexpr int kMinViewportDimension = 200;
inline constexpr int kMaxViewportDimension = 2560;

// DSF stays pinned at 1.0 for now. The guest is authoritative for scale,
// which means the guest must also DIVIDE incoming pointer coordinates by
// it (the portal derives coordinates from video.videoWidth, i.e. pixels,
// while the input dispatchers expect DIP). Until that normalization lands
// in CbInputDispatchCompositeDelegate, a DSF != 1.0 would silently put
// every click in the wrong place — a worse failure than not supporting
// HiDPI at all, because it looks like it works.
inline constexpr float kMaxDeviceScaleFactor = 1.0f;

// Sentinel for "leave the device scale factor as it is".
//
// A caller that omits deviceScaleFactor means "resize, don't rescale" —
// NOT "reset to 1.0". Those differ the moment DSF != 1.0 is supported, and
// defaulting to 1.0 would silently drop a HiDPI session to non-HiDPI on
// every plain resize. Zero is the sentinel because it is not a legal
// scale, so it can never collide with a real request.
inline constexpr float kViewportKeepCurrentScale = 0.0f;

struct CbViewportSpec {
  // Logical size in DIP — the size the page reports as window.innerWidth
  // (modulo scrollbars) and the size input coordinates are expressed in.
  gfx::Size size_dip;

  // Device pixel ratio. Clamped to kMaxDeviceScaleFactor; see above.
  float device_scale_factor = 1.0f;
};

class CbViewportController {
 public:
  // Every pointer must outlive this object. |track_source| may be null
  // (capture not yet started); Apply() then skips the capturer step and
  // the next StartFrameSinkCapture picks up the stored resolution.
  CbViewportController(CbHeadlessScreen* screen,
                       CbAuraPlatformData* aura,
                       CloudBrowserFrameSinkVideoTrackSource* track_source);
  ~CbViewportController();

  CbViewportController(const CbViewportController&) = delete;
  CbViewportController& operator=(const CbViewportController&) = delete;

  // The capture pipeline is built after the viewport controller, so the
  // track source is injected once it exists. nullptr on teardown.
  void SetTrackSource(CloudBrowserFrameSinkVideoTrackSource* track_source);

  // The WebContents whose view follows the viewport. Re-pointed when the
  // active tab changes (SetActiveCapture). nullptr clears it.
  void SetTargetWebContents(content::WebContents* web_contents);

  // Apply |spec|, clamping it first. Returns the spec that was actually
  // applied, which may differ — callers should report the RETURNED value
  // back to whoever asked, so a clamped request is visible rather than
  // silently ignored. |reason| is for logging only.
  //
  // Idempotent: applying the current viewport is a cheap no-op at every
  // layer (each step self-checks), so an unconditional resize handler is
  // a fine caller.
  CbViewportSpec Apply(const CbViewportSpec& spec, const std::string& reason);

  // The viewport currently in effect.
  CbViewportSpec current() const { return current_; }

  // Clamp without applying — lets a caller (e.g. the CDP handler) report
  // what a request WOULD become before committing to it.
  static CbViewportSpec Clamp(const CbViewportSpec& spec);

 private:
  raw_ptr<CbHeadlessScreen> screen_;
  raw_ptr<CbAuraPlatformData> aura_;
  raw_ptr<CloudBrowserFrameSinkVideoTrackSource> track_source_;
  raw_ptr<content::WebContents> web_contents_ = nullptr;

  CbViewportSpec current_;

  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CB_VIEWPORT_CONTROLLER_H_

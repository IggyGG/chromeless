// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbHeadlessScreen — single-window display::ScreenBase subclass for the
// cb-chromium worker. Closes the M5 R1 cursor-routing gate that sits
// UPSTREAM of CbCursorClient::SetCursor: aura asks the installed
// display::Screen "is window under cursor?" before it calls into a
// CursorClient, and the upstream ScreenBase stubs return
// `false` / `gfx::Point()` (NOTIMPLEMENTED_LOG_ONCE). With a false
// answer, aura short-circuits the cursor routing path entirely — the
// CbCursorClient::SetCursor LOG line never fires, and (transitively)
// the M5 R6 cursor-DC emitter never receives a kHand / kIBeam edge.
//
// Why a sibling .cc/.h in this directory instead of a chromium-fork
// patch under //patches:
//   The two overridden methods are public virtuals on display::Screen
//   (see ui/display/screen.h) and the embedder can subclass ScreenBase
//   directly without touching upstream source. Patches under //patches/
//   are reserved for upstream sources the embedder cannot reach via
//   subclassing (BUILD.gn entries, content/* visibility, etc.).
//
// Why "headless" in the class name:
//   chromium has a precedent for this shape — headless/lib/browser/
//   headless_screen.{h,cc} — which similarly overrides the cursor-
//   related ScreenBase virtuals for a server-side single-window
//   embedder. The naming makes the inheritance + purpose obvious to a
//   reader who follows that breadcrumb. The cb-chromium worker IS in
//   effect a headless server-side embedder (Xvfb-backed, no real
//   display surface, single root WindowTreeHost). We deliberately do
//   NOT depend on //headless because that target carries a much wider
//   surface than we need (HeadlessScreenManager, multi-display
//   orientation, etc.), and pulling it in to grab one Screen subclass
//   would invert the "small, well-documented patch + dep surface"
//   principle in patches/README.md.
//
// Cross-references:
//   - ui/display/screen_base.{h,cc} — base class, holds the
//     DisplayList that CloudBrowserBrowserMainParts seeds with the
//     1280x720 default display.
//   - ui/display/screen.h — the interface contract.
//   - headless/lib/browser/headless_screen.{h,cc} — closest upstream
//     template (lightweight, single-window).
//   - content/shell/browser/shell_platform_data_aura.cc — the
//     heavier counterpart that uses ScreenAura; explicitly NOT what
//     we want, the comments there call out the multi-display
//     machinery as overkill for a single-window embedder.
//   - capture/build-integration/cb_cursor_client.{h,cc} — the
//     downstream CursorClient that this Screen subclass unblocks.
//   - capture/build-integration/cb_last_pointer.{h,cc} — M4 R10
//     last-known-pointer state consumed by GetCursorScreenPoint().
//
// R1 scope (CV2-78):
//   * IsWindowUnderCursor — return true for any non-null aura
//     window. The cb-chromium worker is a single-root-window model;
//     aura never asks this question about a window other than the
//     embedder's host_->window() (see cb_aura_platform_data.cc), so
//     "always true for non-null" is the correct shape, not a stub.
//   * GetCursorScreenPoint — returns the browser-process pointer
//     coordinate from CbInputDispatchMouse once the typed input
//     dispatcher is runtime-wired, falling back to gfx::Point(0,0)
//     before first input.
//   * GetWindowAtScreenPoint — returns the single Aura root window
//     for points inside the seeded display. Linux
//     RenderWidgetHostViewAura::UpdateCursorIfOverSelf() asks this
//     BEFORE it calls CursorClient::SetCursor; the ScreenBase stub
//     returns nullptr and short-circuits the per-event cursor route.
//
// Non-goals for R1:
//   * Multi-window selection (the worker is single-window by
//     construction; cb_aura_platform_data.cc constructs ONE
//     WindowTreeHost and that is the only Aura root that ever
//     exists in the process).
//   * GetLocalProcessWindowAtPoint — still not exercised by the
//     Linux cb-chromium worker cursor path.
//
// CV2-78 ring 8 follow-up (this revision adds GetDisplayNearestWindow):
//   The R1 header above explicitly anticipated this case — "If a
//   future ring surfaces a code path that needs them, override here."
//   The M5 R1 per-event verification rv6.b surfaced
//   `display::ScreenBase::GetDisplayNearestWindow` as a NOTIMPLEMENTED
//   log line firing at boot. Aura's cursor-routing path consults
//   Screen::GetDisplayNearestWindow before reaching IsWindowUnderCursor;
//   when the upstream stub returns a default-constructed Display (with
//   NOTIMPLEMENTED_LOG_ONCE), routing can short-circuit before the gate
//   the R1 commit opened. This override returns the 1280x720 default
//   display the embedder seeded in PreEarlyInitialization (the worker
//   is single-display by construction — main_parts seeds exactly one
//   Display into ScreenBase::display_list()), via the same
//   GetPrimaryDisplay() ScreenBase already exposes. Mirrors the
//   chromium upstream pattern (headless/lib/browser/headless_screen.cc)
//   collapsed to the single-display case.

#ifndef CAPTURE_BUILD_INTEGRATION_CB_HEADLESS_SCREEN_H_
#define CAPTURE_BUILD_INTEGRATION_CB_HEADLESS_SCREEN_H_

#include "ui/display/display.h"
#include "ui/display/screen_base.h"
#include "ui/gfx/geometry/point.h"
#include "base/memory/raw_ptr.h"
#include "cloud-browser/capture/build-integration/cb_last_pointer.h"
// CV2-78 Wave 1 first-compile-link fix-forward: chromium-7727 renamed
// ui/gfx/native_widget_types.h → ui/gfx/native_ui_types.h (same file,
// still declares gfx::NativeWindow + gfx::NativeView et al). The
// drafter used the historical header name without source-verifying;
// exactly the MEDIUM-confidence first-compile-link risk the CV2-78
// brief flagged. Verified by direct grep of chromium-src on T7: the
// `using NativeWindow = ...` declaration lives in native_ui_types.h.
// Same methodology lesson class as CV2-75 Ring 3 v2's
// rtc_use_pulse_audio → rtc_include_pulse_audio rename: propositional
// reference must be source-verified before action.
#include "ui/gfx/native_ui_types.h"

namespace cloud_browser {

class CbHeadlessScreen : public display::ScreenBase {
 public:
  CbHeadlessScreen();

  CbHeadlessScreen(const CbHeadlessScreen&) = delete;
  CbHeadlessScreen& operator=(const CbHeadlessScreen&) = delete;

  ~CbHeadlessScreen() override;

  // Supplies the browser-process pointer state written by M4's typed
  // mouse dispatcher. nullptr clears the source and restores the
  // conservative (0,0) fallback.
  void SetLastPointerSource(const CbLastPointerState* last_pointer_state);

  // Supplies the single Aura root window owned by CbAuraPlatformData.
  // nullptr clears the root during teardown.
  void SetRootWindow(gfx::NativeWindow root_window);

  // display::Screen via ScreenBase:

  // Returns true for any non-null Aura window. The cb-chromium worker
  // is a single-root-window embedder — aura::client::GetCursorClient
  // lookups originate from RenderWidgetHostViewAura whose root window
  // IS the embedder host_->window(); there is no other window the
  // question could meaningfully be asked about. Returning true
  // unconditionally lets aura proceed into the CursorClient routing
  // path (i.e. into CbCursorClient::SetCursor), which is the gate the
  // upstream ScreenBase stub (return false) was closing.
  //
  // A null window argument returns false defensively — aura should
  // never ask the question about a null window, but if it does, the
  // honest answer is that a non-existent window cannot be "under the
  // cursor".
  bool IsWindowUnderCursor(gfx::NativeWindow window) override;

  // Returns the latest in-widget pointer coordinate from M4's typed
  // mouse dispatcher when that source is installed and has observed a
  // successful pointer forward. Falls back to gfx::Point(0,0) before
  // first input and after pointer-leave.
  gfx::Point GetCursorScreenPoint() override;

  // Linux RenderWidgetHostViewAura::UpdateCursorIfOverSelf() first asks
  // Screen::GetWindowAtScreenPoint(cursor_screen_point), then rejects
  // cursor updates if the returned window is null or belongs to a
  // different root. Returning the cb-chromium worker's single Aura root
  // for in-display points opens that upstream gate while preserving the
  // single-window/display model.
  gfx::NativeWindow GetWindowAtScreenPoint(
      const gfx::Point& point) override;

  // Returns the single 1280x720 default display the embedder seeds
  // into ScreenBase::display_list() at construction. The cb-chromium
  // worker is single-display by construction (cloud_browser_browser_
  // main_parts.cc PreEarlyInitialization seeds exactly one Display);
  // any window the runtime has IS on that display.
  //
  // CV2-78 ring 8 — closes the upstream NOTIMPLEMENTED stub
  // `display::ScreenBase::GetDisplayNearestWindow` which returns a
  // default-constructed Display. Aura's cursor-routing path consults
  // this method early; the stub's empty-Display return can short-
  // circuit routing before reaching the IsWindowUnderCursor gate the
  // CV2-78 R1 commit opened.
  //
  // Implementation collapses upstream HeadlessScreen's
  // GetDisplayFromScreenRect lookup (multi-display) to a primary-
  // display return — see class-level comment for the single-display
  // rationale. Const-qualified to match `display::ScreenBase`'s
  // virtual; ScreenBase's GetPrimaryDisplay() is itself const and
  // public, so this is a one-liner delegate.
  display::Display GetDisplayNearestWindow(
      gfx::NativeWindow window) const override;

 private:
  raw_ptr<const CbLastPointerState> last_pointer_state_ = nullptr;
  gfx::NativeWindow root_window_ = nullptr;
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CB_HEADLESS_SCREEN_H_

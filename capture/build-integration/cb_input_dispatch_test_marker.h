// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbInputDispatchTestMarker — small build-integration test seam for
// the M4 R9 UAT harness (CV2-49).
//
// Problem the seam solves
// =======================
// The M4 R9 harness asserts DOM observability through CDP
// Runtime.evaluate(document.lastInputEvent), which works fine for the
// surfaces whose DOM event is the observable contract (mouse / wheel /
// keyboard / IME / touch / drag / clipboard). But the R10 "last-
// pointer + leave" surface is DEFINITIONALLY not a DOM event — it's
// a piece of native state that lives in CbLastPointerState and is
// consumed by M5 R3's cursor-channel emitter. There is no DOM-side
// projection.
//
// To assert R10 from the harness, we need a tiny seam that:
//   1. After every successful CbLastPointerState mutation, eval a JS
//      snippet on the active streamed-WebContents that writes the
//      current snapshot to window.__uat_last_pointer.
//   2. Is compiled in ONLY when the build flag enable_cb_uat_marker
//      is true. Production builds MUST NOT carry the marker —
//      otherwise a probable injection vector ships to prod chromium.
//
// This header DECLARES the seam. R10 implements it on its branch
// when the flag is set; the OFF case is a no-op inline function so
// callers (cb_input_dispatch_mouse.cc, the future cb_last_pointer
// writer) can call unconditionally without a compile-time gate.
//
// Build flag
// ----------
// args.gn:
//   enable_cb_uat_marker = false   # default — must be true ONLY in
//                                  # the UAT lane chromium build.
//
// When that flag is on, BUILD.gn passes -DCB_UAT_MARKER=1 and the
// implementation (cb_input_dispatch_test_marker.cc, drafted in a
// follow-up R9 commit) compiles. When the flag is off, this header
// resolves to a no-op and the implementation file is excluded.
//
// Non-goals for R9:
//   * the implementation file itself — drafted alongside the rest of
//     the R9 harness; the seam header is what locks the contract.
//   * exposing more than the R10 snapshot — every additional seam
//     widens the test-mode surface.
//
// See harness/m4-uat/README.md and scenarios/pointer-leave.mjs for
// the consumer side.

#ifndef CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_TEST_MARKER_H_
#define CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_TEST_MARKER_H_

#include <cstdint>

#include "base/component_export.h"

namespace cloud_browser {

class CbLastPointerSnapshot;  // From cb_last_pointer.h (M4 R10).

// Public seam — call unconditionally from R10's last-pointer state
// holder. With CB_UAT_MARKER off, the function is an empty inline
// no-op and the linker drops it. With CB_UAT_MARKER on, the .cc
// translation unit posts a Runtime.evaluate on the active streamed-
// WebContents that writes window.__uat_last_pointer = { x, y,
// in_widget, source: 'native-marker' }.
//
// TODO(M4-R9-marker-impl): write cb_input_dispatch_test_marker.cc in
// the CB_UAT_MARKER=1 branch of build-integration/BUILD.gn. The .cc
// resolves the active WebContents through CbActiveWebContentsResolver
// (M4 R2) and posts the eval; the resolver's deselect-safety
// guarantees keep the marker from leaking across renderer swaps.

#if defined(CB_UAT_MARKER) && CB_UAT_MARKER == 1
COMPONENT_EXPORT(CLOUD_BROWSER_CAPTURE)
void NotifyLastPointerSnapshotForUatMarker(
    const CbLastPointerSnapshot& snapshot);
#else
// No-op implementation when the marker is disabled. Keeps callers
// branch-free; the compiler drops the call.
inline void NotifyLastPointerSnapshotForUatMarker(
    const CbLastPointerSnapshot& /*snapshot*/) {
  // intentional no-op
}
#endif

}  // namespace cloud_browser

#endif  // CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_TEST_MARKER_H_

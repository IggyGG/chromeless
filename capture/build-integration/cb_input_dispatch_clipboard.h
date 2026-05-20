// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbInputDispatchClipboard — M4 R8 typed delegate for the
// `clipboard_copy_request` envelope coming out of M4 R1's
// CbInputDispatch (after the signaling-thread → BrowserThread::UI
// hop).
//
// The clipboard-copy-request path is the spec's "native equivalent"
// of the Go input-bridge/main.go CDP dispatcher's
// `clipboard_copy_request` arm. Per docs/protocols/input-channel.md:
//
//   > Sent when the local user issues a copy gesture inside the video
//   > stage. Asks the server to push the current remote clipboard
//   > back over a **separate** out-of-band channel (not specified
//   > here — see future "clipboard" channel). Included in the input
//   > envelope so that the local copy gesture preserves activation
//   > timing.
//
// In other words, the envelope carries no payload: `{}` — the client
// is asking the server-side browser to run a copy operation against
// whatever is currently selected in the focused frame. The actual
// copied text travels back to the client over the M6 R2 clipboard
// DataChannel (cv2/m6-r2 clipboard relay), not over the input
// channel.
//
// R8 scope (CV2-48):
//   * `clipboard_copy_request` envelope dispatch
//   * Native equivalent of the Go bridge's two-step
//     `Input.dispatchKeyEvent` keyDown/keyUp Ctrl+C synthesis
//   * No reply on the input channel — the produced clipboard delta
//     is emitted by chromium's normal clipboard write path, which
//     M6 R2's relay observes and forwards over the dedicated
//     clipboard DC
//   * Activation-timing preservation: the dispatch happens
//     synchronously on the UI thread inside the envelope handler so
//     the copy executes in the same user-activation tick the client
//     reported
//   * Cross-rank modifier discipline: temporarily OR Ctrl into the
//     dispatched events ONLY. The cross-rank CbHeldModifierState
//     (owned by R4) is NOT mutated — a clipboard_copy_request is
//     not a user "Control is now held" signal, just a synthesised
//     transient keystroke pair. A subsequent mouse_button arriving
//     between our key_down and key_up MUST NOT see Ctrl pinned
//     in its modifier bitmask, so we keep our hands off
//     shared_modifiers_->blink_modifiers
//
// Non-goals for R8:
//   * Reply path (server→client clipboard delta) — owned by M6 R2
//     (cb_clipboard_relay.{h,cc} on cv2/m6-r2). R8 does not touch
//     the clipboard DC; we just trigger the chromium-side copy and
//     let chromium's clipboard subsystem fire its normal write
//     events that M6 R2 already observes
//   * Reading the clipboard content here — that bypasses M6's
//     dedicated relay and would land selection text on the input
//     channel, which is wrong per the protocol comment quoted
//     above ("**separate** out-of-band channel")
//   * Mutating the shared cross-rank modifier state — see the
//     "Cross-rank modifier discipline" point above; deliberately
//     scoped out so a mouse_button between our key_down and key_up
//     does not get Ctrl falsely OR'd in
//   * Handling clipboard_paste — that envelope IS on the v1 input
//     spec but is owned by a different M4 rank (R6 placeholder per
//     the M4 keyboard header's TODO; we leave the dispatch surface
//     unclaimed here)
//   * AutoRepeat / held-Ctrl-then-C: the envelope is a single one-
//     shot copy; there is no down/up split on the wire to honour
//     (unlike `key_down`/`key_up`). The kRawKeyDown/kKeyUp pair is
//     an implementation detail of the chromium-internal copy
//     command, not a wire-level handshake
//
// Implementation choice — synthesised Ctrl+C vs direct API
// (TODO(M4-R8-direct-clipboard-api)):
//
//   Two valid native realisations of the envelope exist:
//
//   (A) Synthesise kRawKeyDown + kKeyUp NativeWebKeyboardEvents for
//       'C' with the Ctrl modifier bit set, dispatched via
//       RenderWidgetHost::ForwardKeyboardEvent. Chromium's renderer
//       receives a real keyboard event pair, fires `keydown`/`keyup`
//       to the page's listeners (so a page that calls
//       preventDefault() in its keydown handler can suppress the
//       copy — same observable behaviour as the user pressing the
//       physical Ctrl+C themselves), and falls through to the
//       default editor command "Copy" on no preventDefault.
//
//   (B) Call `content::WebContents::Copy()` on the active
//       WebContents. This drives the focused frame's
//       `RenderFrame::Copy` (via mojo IPC) — the page's `copy`
//       event still fires (copy-event is wired to the editor
//       command, not to the keyboard event), but no DOM
//       `keydown`/`keyup` events are produced. Equivalent to the
//       browser's Edit→Copy menu item.
//
//   R8 picks (A) for two reasons:
//
//     1. Backend parity. The Go input-bridge/main.go arm picks (A)
//        explicitly ("Phase 1: synthesize Ctrl+C") — same
//        envelope, same observable result on both backends. The
//        M0 native-peer-gate's reason to exist is that the two
//        backends behave identically; (B) would diverge.
//
//     2. preventDefault() semantics. A page that has bound a
//        `keydown` handler to intercept Ctrl+C (e.g. a code editor
//        that wants to copy the selection differently) MUST get
//        the same opportunity to preventDefault on both backends.
//        (A) produces the keyboard event so the page's handler
//        runs; (B) skips straight to the editor command and the
//        page handler never sees it. The Go bridge gives the page
//        the chance, so we give the page the chance too.
//
//   TODO(M4-R8-direct-clipboard-api): if a future spec phase
//   decides to bypass page-side preventDefault entirely (e.g. for a
//   "copy-cannot-be-suppressed" admin policy), swap to (B) by
//   replacing DispatchCopy()'s NativeWebKeyboardEvent pair with a
//   single `wc->Copy()` call. Document the policy in the spec
//   first; the wire envelope shape does not change.
//
// Dependency on M4 R2 (active streamed-WebContents resolver):
//   R8 dispatches against the WebContents the FSVC is currently
//   capturing from, mirroring R3/R4/R5/R6/R7. We hold a pointer to
//   the same WebContentsResolver interface and tolerate null
//   exactly like R4 (LOG(WARNING) per envelope so the operator
//   sees the gap pre-R2 wiring).
//
// Dependency on M4 R4 (CbHeldModifierState):
//   R8 reads `shared_modifiers_->blink_modifiers` for diagnostics
//   only — we want to know whether the page-side already had Ctrl
//   held when the request arrived (which on a real user keystroke
//   would be true), so we can VLOG when the synth is redundant.
//   R8 does NOT mutate the shared state — see the "Cross-rank
//   modifier discipline" point above.
//
// Dependency on M4 R1 (CbInputDispatchDelegate):
//   R8 is a CbInputDispatchDelegate subclass that filters on
//   `clipboard_copy_request`. Non-clipboard envelopes are silently
//   dropped here; the composite delegate that the M4 wiring rank
//   assembles will route them to R3/R4/R5/R6/R7.
//
// All public methods are invoked on BrowserThread::UI (enforced by
// M4 R1's PostTask). The class is NOT thread-safe; chromium's
// keyboard injection API is itself UI-thread-only.

#ifndef CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_CLIPBOARD_H_
#define CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_CLIPBOARD_H_

#include <cstdint>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "cloud-browser/capture/build-integration/cb_input_dispatch.h"
#include "cloud-browser/capture/build-integration/cb_input_dispatch_keyboard.h"
#include "cloud-browser/capture/build-integration/cb_input_dispatch_mouse.h"

namespace content {
class RenderWidgetHost;
class WebContents;
}  // namespace content

namespace cloud_browser {

class CbInputDispatchClipboard : public CbInputDispatchDelegate {
 public:
  // |resolver| supplies the active WebContents for each dispatch;
  // may be null during M4 R2 development (R8 will log + skip
  // dispatch). Lifetime: caller-owned, must outlive this object.
  //
  // |shared_modifiers| is the cross-rank held-modifier state owned
  // by M4 R4. May be null in standalone R8 unit-test paths. R8
  // reads-only — never mutates. Lifetime: caller-owned, must
  // outlive this object.
  CbInputDispatchClipboard(WebContentsResolver* resolver,
                           const CbHeldModifierState* shared_modifiers);

  CbInputDispatchClipboard(const CbInputDispatchClipboard&) = delete;
  CbInputDispatchClipboard& operator=(const CbInputDispatchClipboard&) = delete;

  ~CbInputDispatchClipboard() override;

  // CbInputDispatchDelegate. R8 only acts on `clipboard_copy_request`
  // envelopes; non-clipboard types are silently dropped here.
  //
  // TODO(M4-R8-composite-delegate): once the M4 wiring rank lands
  // the composite delegate, swap this from "filter and ignore" to
  // a pre-typed method (OnClipboardCopyRequest) so unrelated
  // envelopes never reach this class.
  void OnInputEvent(InputEnvelope envelope) override;

  // R1's other delegate hooks — R8 does not have additional
  // diagnostics over R1's logging defaults, so we let the base
  // implementation handle them.

 private:
  // The actual synthesised-Ctrl+C dispatch. Factored out so a
  // future Phase-2 swap to `content::WebContents::Copy()` (see
  // TODO(M4-R8-direct-clipboard-api) in the file header) only has
  // to replace this method body.
  void DispatchCopy(content::WebContents* wc,
                    base::TimeTicks event_time);

  // R8 reads the shared modifier state for a VLOG-only "Ctrl was
  // already held" diagnostic. Marked const because R8 never
  // mutates the shared state — see "Cross-rank modifier
  // discipline" in the file header.
  //
  // CV2-81 first-compile-link fix-forward (10th lesson-(i) ring,
  // lesson-(g.4) Discipline-shape): raw T* const wrapped in
  // raw_ptr<T> for chromium-style lint compliance. Mirrors
  // CV2-75's 9d33276 fix-forward precedent.
  const raw_ptr<WebContentsResolver> resolver_;
  const raw_ptr<const CbHeldModifierState> shared_modifiers_;
};

}  // namespace cloud_browser

#endif  // CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_CLIPBOARD_H_

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbInputDispatchIme — M4 R5 typed delegate for composition_start /
// composition_update / composition_end / composition_cancel envelopes
// coming out of M4 R1's CbInputDispatch (after the signaling-thread →
// BrowserThread::UI hop).
//
// The IME path is the native equivalent of input-bridge/main.go's
// `Input.imeSetComposition` / `Input.insertText` CDP dispatchers, but
// driven through chromium's internal `RenderWidgetHostImpl::Ime*`
// APIs instead of the CDP `Input.*` domain. The two paths are
// intentionally semantically parity-aligned — the same envelope
// sequence MUST produce the same observable DOM event sequence
// (compositionstart / compositionupdate / compositionend, plus the
// final `input` event from a commit) in either backend.
//
// Wire mapping summary (full detail in docs/protocols/input-channel.md
// §composition_*):
//
//   protocol               → native API
//   ─────────              ─────────
//   composition_start      → RWHI::ImeSetComposition(text="", spans,
//                                                    InvalidRange, 0, 0)
//   composition_update     → RWHI::ImeSetComposition(text, spans,
//                                                    InvalidRange,
//                                                    selStart, selEnd)
//   composition_end        → RWHI::ImeCommitText(text, spans,
//                                                InvalidRange, /*relative
//                                                cursor pos=*/0)
//   composition_cancel     → RWHI::ImeCancelComposition()
//
// Notes:
//
//   * The CDP path uses Input.insertText for composition_end; the
//     native equivalent is ImeCommitText, not InsertText() (the latter
//     bypasses the IME state machine and is wrong for committing a
//     composition because it doesn't clear the in-progress
//     composition the renderer is already drawing underlines for).
//
//   * `replacement_range` stays gfx::Range::InvalidRange() for the v1
//     protocol — dead-key replacement is reserved for a future v2
//     extension (the Go bridge sets replacementStart/replacementEnd
//     to 0,0 today; gfx::Range::InvalidRange() is the documented
//     "let renderer decide" sentinel that produces the same
//     behaviour). TODO(M4-R5-replacement-range).
//
//   * `ime_text_spans` carries the underline / highlight metadata
//     blink uses to render the composition state. R5 builds a single
//     ui::ImeTextSpan covering [0, text.length) with the default
//     COMPOSITION type so the composing string is visually
//     distinguished. v2 will pull richer span data from a per-
//     platform IME bridge — see candidate_list note in
//     input-channel.md.
//
// R5 scope (CV2-45):
//   * composition_start / composition_update / composition_end /
//     composition_cancel native dispatch
//   * UTF-16-aware selection clamping (the protocol counts in UTF-16
//     code units to match CompositionEvent.data; blink::WebString is
//     also UTF-16 internally, so no transcoding is needed beyond
//     u16string conversion)
//   * is_composing() flag, defensively read by M4 R4 (keyboard) so
//     that even if a misbehaving client forwards key_* during a
//     composition, R4 can drop those events on the floor. The flag
//     is set on composition_start, cleared on composition_end /
//     composition_cancel.
//   * publishes a last-committed-text snapshot for tests and the
//     verification harness — the snapshot is the text the renderer
//     was last told to commit (composition_end.data), not any
//     in-progress composing string
//
// Non-goals for R5:
//   * mouse / keyboard / drag / touch / clipboard (M4 R3 / R4 / R6 /
//     R7 / R8 own those)
//   * dead-key combos that span TWO envelopes via replacement_range —
//     v1 clients fold those into a single composition_end, so R5
//     never sees the multi-envelope shape (see TODO above)
//   * candidate-list UI surfacing (input-channel.md notes this is
//     currently unobservable from a Web client and reserved for v2)
//   * IME bridge for non-Web-stack platforms (Wayland IM, Fcitx5,
//     IBus) — those would surface via a system IME, not the
//     RTCDataChannel
//
// Dependency on M4 R2 (active streamed-WebContents resolver):
//   R5 dispatches to the WebContents that the FSVC is currently
//   capturing from. Resolution is M4 R2's job; this file uses the
//   WebContentsResolver interface declared in cb_input_dispatch_
//   mouse.h (M4 R3) so the R-series share one resolver type — when
//   R2 lands, both R3 and R5 will pick up the concrete
//   implementation via the same composition-root wiring in
//   cloud_browser_browser_main_parts.cc.
//
//   TODO(M4-R5-r2-interface): when R2 lands and the resolver moves
//   to its own header (cb_active_webcontents_resolver.h, per R3's
//   TODO(M4-R3-r2-interface)), switch the include below from R3's
//   header to the R2 header.
//
// All public methods are invoked on BrowserThread::UI (enforced by
// M4 R1's PostTask). The class is NOT thread-safe — it holds
// mutable is-composing state and the chromium IME APIs are
// themselves UI-thread-only.

#ifndef CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_IME_H_
#define CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_IME_H_

#include <cstdint>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "cloud-browser/capture/build-integration/cb_input_dispatch.h"
#include "cloud-browser/capture/build-integration/cb_input_dispatch_mouse.h"

namespace content {
class RenderWidgetHost;
class WebContents;
}  // namespace content

namespace cloud_browser {

// Snapshot of the last successfully committed composition. Read by
// the R5 unit test and the M7 verification harness so a passing
// end-to-end "type 你 via Pinyin" run can be asserted without
// reaching into the renderer's accessibility tree.
//
// Coordinates are not relevant for IME (composition has no spatial
// extent in this struct — the optional rect from composition_start
// is forwarded to chromium directly and not retained here).
struct CbLastCompositionSnapshot {
  // UTF-16 string the renderer was last told to commit. Empty after
  // a composition_cancel; never updated by composition_start /
  // composition_update (those produce no commit).
  std::u16string last_committed_text;
  // Monotonic timestamp of the last successful commit. Default-
  // constructed = "no composition committed yet".
  base::TimeTicks at;
};

class CbInputDispatchIme : public CbInputDispatchDelegate {
 public:
  // |resolver| supplies the active WebContents for each dispatch; may
  // be null during M4 R2 development (R5 will log + skip dispatch).
  // Lifetime: caller-owned, must outlive this object.
  explicit CbInputDispatchIme(WebContentsResolver* resolver);

  CbInputDispatchIme(const CbInputDispatchIme&) = delete;
  CbInputDispatchIme& operator=(const CbInputDispatchIme&) = delete;

  ~CbInputDispatchIme() override;

  // CbInputDispatchDelegate. R5 only acts on composition_* envelopes;
  // unknown / non-composition types are silently dropped here (the
  // composite delegate that M4 wiring assembles will route them to
  // R3 / R4 / R7 / R8 instead).
  //
  // TODO(M4-R5-composite-delegate): once R8 lands the composite
  // delegate, swap this from "filter and ignore" to a pre-typed
  // method (OnCompositionStart / OnCompositionUpdate /
  // OnCompositionEnd / OnCompositionCancel) so non-composition
  // envelopes never reach this class. Mirrors the equivalent TODO in
  // cb_input_dispatch_mouse.h.
  void OnInputEvent(InputEnvelope envelope) override;

  // True between composition_start and composition_end /
  // composition_cancel. R4 (keyboard) reads this as a defensive
  // suppression check — the protocol REQUIRES the client to drop
  // key_* events during composition (input-channel.md §"Key
  // suppression during composition"), but a misbehaving client could
  // still forward them and R4 should ignore rather than dispatch a
  // double-event.
  //
  // Reads are UI-thread only (matches the dispatch contract); no
  // memory ordering required.
  bool is_composing() const { return is_composing_; }

  // Snapshot for tests + verification harness. Empty until the first
  // composition_end fires.
  const CbLastCompositionSnapshot& last_composition() const {
    return last_composition_;
  }

  // Test seam — forces is_composing_ to a specific value without
  // dispatching. Used by cb_input_dispatch_ime_test.cc to assert
  // R4's suppression contract independent of a working IME dispatch
  // path.
  void SetIsComposingForTesting(bool composing) {
    is_composing_ = composing;
  }

 private:
  // Per-type dispatch handlers. Each pulls fields out of `data` per
  // the protocol shape in docs/protocols/input-channel.md.
  void DispatchCompositionStart(const base::DictValue& data,
                                base::TimeTicks event_time);
  void DispatchCompositionUpdate(const base::DictValue& data,
                                 base::TimeTicks event_time);
  void DispatchCompositionEnd(const base::DictValue& data,
                              base::TimeTicks event_time);
  void DispatchCompositionCancel(base::TimeTicks event_time);

  // Resolves the WebContents → focused RenderWidgetHost the
  // composition should drive. Returns null if the resolver is null
  // (pre-M4 R2) or no WebContents is active.
  //
  // Why focused-widget (rather than the WebContents's primary main
  // frame's RWH): IME state is per-widget, and a page with a focused
  // <iframe> hosts its editable element inside the iframe's
  // RenderWidgetHost — committing to the main frame's RWH there
  // would land the text in the wrong document.
  //
  // TODO(M4-R5-focused-widget): verify that
  // WebContents::GetFocusedFrame()->GetView()->GetRenderWidgetHost()
  // is the correct accessor once the build is alive. Aura-host
  // headless paths sometimes have a stale focused frame on the
  // first dispatch — re-check together with R3's bring-to-front
  // path (cb_input_dispatch_mouse.cc::EnsureBroughtToFront).
  content::RenderWidgetHost* ResolveFocusedRenderWidgetHost();

  // Tracks whether we are currently inside a composition. Set on
  // composition_start, cleared on composition_end /
  // composition_cancel. Sticky across composition_update.
  bool is_composing_ = false;

  // Last-committed snapshot for tests + verification.
  CbLastCompositionSnapshot last_composition_;

  // M4 R2 resolver. Caller-owned; can be null pre-R2.
  // CV2-81 first-compile-link fix-forward (lesson-(g.4) Discipline-shape).
  const raw_ptr<WebContentsResolver> resolver_;
};

}  // namespace cloud_browser

#endif  // CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_IME_H_

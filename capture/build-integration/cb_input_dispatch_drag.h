// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbInputDispatchDrag — M4 R7 typed delegate for drag_start / drag_over /
// drop / drag_end envelopes coming out of M4 R1's CbInputDispatch (after
// the signaling-thread → BrowserThread::UI hop).
//
// The drag path is the spec's "native equivalent" of the Go input-bridge/
// main.go CDP dispatcher's drag-and-drop arm, but driven through chromium's
// internal `content::RenderWidgetHostImpl::DragTargetDrag{Enter,Over,Leave}`
// and `DragTargetDrop` APIs (with a `content::DropData` payload) instead of
// `Input.dispatchDragEvent`. The two paths are intentionally semantically
// parity-aligned — the same envelope sequence MUST produce the same
// observable DOM event sequence (`dragenter` / `dragover` / `drop` /
// `dragend`) in either backend. See capture/input-bridge/main.go's
// Dispatch() drag_* arms and buildDragEvent() for the wire-format and
// CDP-payload reference.
//
// ─── ⚠ PRINCIPAL-RISK MODULE ────────────────────────────────────────────
// CV2-47 is flagged as M4's principal-risk rank. Drag is the only input
// type with:
//   * a multi-envelope, per-drag state machine that the server must hold
//     across signaling-thread hops (per-drag-id payload caching),
//   * an asymmetric terminal — drop AND drag_end are both terminal, but
//     drop carries the payload and drag_end carries the success bit, and
//     drag_end after drop is a state-release not a dispatch,
//   * a DataTransfer marshalling step (the protocol's `items[]` array
//     with `kind`/`type`/`data` triples → chromium's `content::DropData`
//     with `text`/`html`/`url`/`custom_data`/`filenames` member groups),
//   * a tolerance requirement for malformed sequencing (stray drag_over
//     outside an active drag MUST be no-op + log, not error),
//   * a coalescing requirement (drag_over coalesceable on the same rules
//     as mouse_move; drag_start/drop/drag_end NEVER coalesced).
//
// Document all of these explicitly in TODO(M4-R7-...) markers below so
// the fold-in reviewer can map every spec acceptance criterion to a code
// site.
// ────────────────────────────────────────────────────────────────────────
//
// R7 scope (CV2-47):
//   * drag_start / drag_over / drop / drag_end native dispatch via the
//     RenderWidgetHostImpl::DragTarget{Enter,Over,Leave,Drop} surface
//   * per-active-drag state: drag-id (single in v1), source-coords,
//     current-target widget-point, cached DropData payload, type-list,
//     coarse phase (kIdle / kActive / kAwaitingEnd)
//   * DataTransfer marshalling: protocol `items[]` → `content::DropData`
//     (kind="string" → text/html/url/custom_data; kind="file" →
//     filenames with empty paths so the page sees `DataTransfer.types`
//     contain the MIME but reading file content yields empty per the
//     v1 file-drag policy)
//   * drop-vs-cancel asymmetry handler: drop dispatches DragTargetDrop
//     and transitions kActive → kAwaitingEnd; drag_end with success=true
//     after drop just clears state; drag_end with success=false from
//     either kActive OR kAwaitingEnd dispatches DragTargetDragLeave to
//     synthesise the `dragend(success=false)` DOM event, then clears
//   * stray drag_over outside an active drag: log + no-op (spec
//     compliance — servers MUST tolerate per input-channel.md §
//     drag_start/drag_over)
//   * dragOperationsMask: hard-coded `kCopy` to mirror the Go bridge's
//     pre-v2 behaviour; Ctrl/Alt/Meta override is a v2 spec concern
//     (TODO below)
//   * coordinate map shared with R3: content-space px → widget DIPs,
//     reuses the same DSF logic for parity
//   * once-per-active-widget activation analogue (same Page.bringToFront
//     latch as R3 — chromium's drag detector also requires the renderer
//     to be visible + focused before it will dispatch `dragenter`)
//   * shared held-modifier read from R3's CbInputDispatchMouse so a
//     `key_down Shift / drag_start / drop / key_up Shift` sequence
//     surfaces shiftKey=true in the dispatched DragEvent.modifiers
//     (matches the v1 mouse shift-click invariant)
//
// Non-goals for R7:
//   * cross-tab / cross-window drag — protocol explicitly scopes drags
//     to the single streamed WebContents the FSVC is currently capturing
//     from. A drag that originates in another tab is out of scope for
//     v1; document the assumption and let chromium's default behaviour
//     (drag ignored by inactive WebContents) handle it
//   * drag-source initiated drags — protocol v1 only handles
//     drags INTO the cloud-browser viewport (client-side drag, server-
//     side drop target). Drags originated INSIDE the renderer (page
//     calls `setData()` then user picks up the element) are surfaced
//     to the client through the cursor channel as a `cursor=grabbing`
//     hint and the eventual DOM `dragstart` fires inside the rendered
//     page; we never dispatch envelopes for those
//   * file-content transfer — protocol v1 carries `kind: "file"` items
//     with no `data` field. The page sees the MIME type in
//     `DataTransfer.types` but `getData()` returns empty. File upload
//     is its own v1+ roadmap item (T??)
//   * pointer capture during drag — chromium's drag pipeline inhibits
//     `mousemove` while a drag is active by design; we do NOT need to
//     suppress the mouse_move path on R3 during a drag (the renderer
//     handles the suppression)
//   * the `Input.setInterceptDrags` toggle the Go bridge enables once
//     per process — that's a CDP-side mechanism to inhibit OS-level
//     drag handling that would race the protocol drag. The native
//     RWH DragTarget* path bypasses the OS drag machinery entirely so
//     no equivalent toggle is needed; the renderer-side dispatch is
//     fully synthetic
//
// Dependency on M4 R1 (envelope sink) — R7 implements
//   CbInputDispatchDelegate and is registered alongside R3/R4/R5/R6/R8
//   on the composite delegate that M3 R5 attaches to the input
//   DataChannel. M4 R1 has already validated `v == 1` and hopped to
//   BrowserThread::UI before invoking us. See TODO(M4-R7-composite-
//   delegate).
//
// Dependency on M4 R2 (active streamed-WebContents resolver) — same
// contract as R3: R7 holds a WebContentsResolver* and tolerates null
// pre-R2 by logging per envelope and dropping the dispatch. We reuse
// R3's interface (not redeclared here) so a single resolver
// implementation feeds R3 + R4 + R5 + R6 + R7 + R8 in production.
//
// Dependency on M4 R3 (mouse dispatch) — R7 reads R3's held-modifier
// state for the `modifiers` field on each DragTarget* call. We hold a
// pointer to the R3 instance solely for `held_modifiers_blink()`
// access; see TODO(M4-R7-shared-modifier-extraction). The same fold-in
// task that promotes R3's `held_modifiers_blink_` to a shared
// CbHeldModifierState (called out by R4) is what lets R7 drop the
// direct R3 dependency.
//
// All public methods are invoked on BrowserThread::UI (enforced by M4
// R1's PostTask). The class is NOT thread-safe — it mutates the
// per-drag state on every envelope and chromium's drag-injection API
// is itself UI-thread-only.

#ifndef CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_DRAG_H_
#define CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_DRAG_H_

#include <cstdint>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "cloud-browser/capture/build-integration/cb_input_dispatch.h"
#include "cloud-browser/capture/build-integration/cb_input_dispatch_mouse.h"

namespace content {
class DropData;
class RenderWidgetHost;
class WebContents;
}  // namespace content

namespace cloud_browser {

// Per-drag cached state. v1 protocol scopes a session to a single
// concurrent drag (no drag-id field on the wire), so we only need one
// slot — but the field is named in singular form to make it obvious
// where a future v2 multi-drag map would live.
//
// TODO(M4-R7-multi-drag-future): if v2 ever introduces drag-id on the
// wire for parallel drags (e.g. multi-touch drag-and-drop on a tablet
// client), promote ActiveDrag to a map keyed by drag-id and gate the
// state-machine transitions per-id. The current single-slot design is
// load-bearing for v1's tolerance contract — stray drag_over outside
// an active drag MUST be a no-op, which is straightforward to verify
// against a single `phase != kIdle` check.
struct CbDragItem {
  // "string" or "file". The protocol's full set; v1 emits both but the
  // file variant carries no payload (file-content transfer is deferred
  // to a v1+ task).
  std::string kind;
  // MIME type, lower-cased on the wire (the client guarantees the
  // case; we don't normalise on the server side because the parity
  // backend doesn't either — both round-trip whatever the client sent).
  std::string type;
  // UTF-8 payload. Required when kind == "string"; ignored (and
  // expected empty) when kind == "file".
  std::string data;
};

// Coarse state-machine phase. Compact for two reasons:
//   * the spec describes only three meaningful states (idle, active,
//     awaiting-end), and modelling the AwaitingEnd state explicitly is
//     load-bearing for the drop-vs-cancel asymmetry — see the comment
//     on DispatchDragEnd below;
//   * a future v2 multi-drag map would carry one of these enums per
//     drag-id, so keeping the enum small + copyable matters.
enum class CbDragPhase : uint8_t {
  // No active drag. drag_over here is a no-op (spec compliance).
  kIdle = 0,
  // drag_start has fired (DragTargetDragEnter dispatched); zero-or-more
  // drag_over events expected; terminal is either drop or drag_end.
  kActive = 1,
  // drop has fired (DragTargetDrop dispatched). The state is retained
  // until drag_end clears it so the success=true case can be a no-op
  // and the success=false case (cancel-after-drop, unusual but spec-
  // legal) can dispatch a DragTargetDragLeave to surface
  // dragend(success=false).
  kAwaitingEnd = 2,
};

class CbInputDispatchDrag : public CbInputDispatchDelegate {
 public:
  // |resolver| supplies the active WebContents for each dispatch; may
  // be null during M4 R2 development (R7 logs + skips dispatch).
  // Lifetime: caller-owned, must outlive this object.
  //
  // |mouse| supplies the shared held-modifier state. Read-only access;
  // R7 never mutates R3's state. May be null in unit tests that only
  // exercise the state-machine paths — the dispatched DragTarget*
  // calls will use modifiers=0 in that case.
  //
  // TODO(M4-R7-shared-modifier-extraction): the production wiring in
  // cloud_browser_browser_main_parts.cc will construct a single shared
  // CbHeldModifierState and inject the same pointer into R3 and R7.
  // Until then, R7 reads through the R3 instance pointer directly.
  CbInputDispatchDrag(WebContentsResolver* resolver,
                      const CbInputDispatchMouse* mouse);

  CbInputDispatchDrag(const CbInputDispatchDrag&) = delete;
  CbInputDispatchDrag& operator=(const CbInputDispatchDrag&) = delete;

  ~CbInputDispatchDrag() override;

  // CbInputDispatchDelegate. R7 only acts on drag_* and `drop`
  // envelopes; unknown / non-drag types are silently dropped here (the
  // composite delegate that M4 wiring assembles will route them to
  // R3 / R4 / R5 / R6 / R8 instead).
  //
  // TODO(M4-R7-composite-delegate): once the composite delegate lands
  // (R8 spec ticket), swap this from "filter and ignore" to typed
  // pre-routed methods so non-drag envelopes never reach this class.
  void OnInputEvent(InputEnvelope envelope) override;

  // ── Test seams ────────────────────────────────────────────────────

  // Reset the state machine. Used by cb_input_dispatch_drag_test.cc
  // between scenarios; never called in production.
  void ResetStateForTesting();

  // Read-only access to the current phase. Used by unit tests to
  // assert state-machine invariants without dispatching real drag
  // events (which require a RWH).
  CbDragPhase phase_for_testing() const { return phase_; }

  // Read-only access to the cached payload. Used by unit tests to
  // assert the drop-re-asserts-items contract in the spec.
  const std::vector<CbDragItem>& cached_items_for_testing() const {
    return cached_items_;
  }
  const std::vector<std::string>& cached_types_for_testing() const {
    return cached_types_;
  }

 private:
  // ── Per-type dispatch handlers ────────────────────────────────────

  // drag_start: caches items + types, transitions to kActive,
  // dispatches DragTargetDragEnter at (x, y).
  //
  // PRINCIPAL-RISK: re-entry from kActive or kAwaitingEnd is the
  // malformed-client case. Per the spec this is undefined behaviour;
  // we choose to FORCE-CLEAR the prior state with a synthesised
  // DragTargetDragLeave before re-entering kActive so the renderer's
  // drag pipeline doesn't end up in a half-state. Log loud so we
  // notice the client-side bug.
  //
  // TODO(M4-R7-renter-drag-start): confirm that DragTargetDragLeave
  // followed by DragTargetDragEnter in the same UI-thread task is
  // race-free in chromium's drag pipeline. The CDP path doesn't model
  // this explicitly (CDP synthesises the leave inside dispatchDragEvent
  // when the next dragEnter arrives with no intervening leave).
  void DispatchDragStart(const base::DictValue& data,
                         base::TimeTicks event_time);

  // drag_over: dispatches DragTargetDragOver at (x, y) if kActive;
  // logs + no-op otherwise (spec compliance).
  void DispatchDragOver(const base::DictValue& data,
                        base::TimeTicks event_time);

  // drop: re-asserts items + types (drop carries the full payload per
  // the spec; the cached state is overwritten in case the client
  // mutated the items between drag_start and drop, which is unusual
  // but allowed), dispatches DragTargetDrop at (x, y), and transitions
  // kActive → kAwaitingEnd. Does NOT clear state — the follow-up
  // drag_end is the release.
  //
  // PRINCIPAL-RISK: drop while kIdle is a malformed-sequence case. The
  // Go bridge silently dispatches CDP's `drop` anyway (relying on
  // chromium to ignore drops without a prior dragEnter); we mirror
  // that here by attempting the dispatch + logging. A stricter
  // alternative is to drop-on-the-floor — call this out for the
  // fold-in reviewer.
  //
  // TODO(M4-R7-drop-without-start): pick strict vs tolerant once we
  // see real client behaviour. The cb-chromium-side renderer behaviour
  // for "DragTargetDrop with no preceding DragTargetDragEnter" needs
  // confirmation against chromium HEAD — the Aura path almost
  // certainly rejects it as a no-op, but the synthetic-RWH path may
  // surface a DCHECK in debug.
  void DispatchDrop(const base::DictValue& data,
                    base::TimeTicks event_time);

  // drag_end: terminal transition. Four paths:
  //   * kIdle: informational no-op (log).
  //   * kActive + success=true: malformed (drop never fired) — log;
  //     dispatch DragTargetDragLeave to keep the renderer's drag
  //     pipeline consistent; clear state.
  //   * kActive + success=false: normal cancel path — dispatch
  //     DragTargetDragLeave (chromium surfaces dragend(success=false)
  //     to the page); clear state.
  //   * kAwaitingEnd + success=true: normal success path — drop
  //     already fired; just clear state.
  //   * kAwaitingEnd + success=false: cancel-after-drop. Unusual but
  //     spec-legal (client side decided to undo the drop). Dispatch
  //     DragTargetDragLeave so the renderer sees a dragend with
  //     success=false; clear state.
  //
  // PRINCIPAL-RISK: this is the most asymmetric branch in the entire
  // M4 surface — every other handler is "translate envelope → dispatch
  // → done", but drag_end has to inspect TWO pieces of state (current
  // phase + envelope.success) and pick one of five behaviours. Bug-
  // surface here is high; the unit-test plan must enumerate all five
  // transitions explicitly.
  void DispatchDragEnd(const base::DictValue& data,
                       base::TimeTicks event_time);

  // ── Helpers ───────────────────────────────────────────────────────

  // Decode the protocol's `items[]` + `types[]` into a content::
  // DropData. Implemented in the .cc file because DropData is a
  // chromium internal type we don't want in the public header.
  //
  // The transformation is deterministic and stateless: same inputs
  // always produce the same DropData. The cached payload is rebuilt
  // on every drag_start and drop (the spec re-asserts items on drop,
  // and a v2 client mutating items mid-drag would land here).
  //
  // TODO(M4-R7-dropdata-fidelity): chromium's DropData has more
  // members than v1 protocol exposes (file_contents, html_base_url,
  // referrer_policy, custom_data). For v1 we leave the unexposed
  // members default-constructed. Cross-check against
  // content/public/common/drop_data.h when the chromium tree is
  // available — the field set has shifted across versions.
  void BuildDropData(content::DropData* out) const;

  // Coordinate map. Reuses R3's algorithm — content-space px →
  // widget DIPs, division by RWHV::GetDeviceScaleFactor() if non-zero,
  // no-op otherwise. Re-implemented here (rather than pulled from R3
  // statically) so R7's unit tests don't need a CbInputDispatchMouse
  // instance.
  //
  // TODO(M4-R7-share-coord-map): collapse R3's and R7's coordinate-map
  // implementations into a shared free function in cb_input_coord.h at
  // fold-in. Duplicating it here keeps the draft compilation surface
  // tight.
  struct WidgetPoint {
    float x;
    float y;
  };
  WidgetPoint ContentToWidget(content::RenderWidgetHost* rwh,
                              int content_x, int content_y) const;

  // Once-per-instance activation analogue, identical to R3's
  // EnsureBroughtToFront. We re-implement instead of forwarding to R3
  // because the latch is per-class — if R7 fires before R3 ever has,
  // the renderer-side focus chain needs the same kick.
  //
  // TODO(M4-R7-share-activation-latch): make the bring-to-front latch
  // shared across R3/R4/R5/R6/R7/R8 at fold-in (probably hung off the
  // composite delegate or the WebContentsResolver). Duplicating it
  // here is bounded — at most one redundant WasShown()+Focus() per
  // capture session.
  void EnsureBroughtToFront(content::WebContents* wc);

  // Translate the cached state + dispatched coordinates into the
  // modifier mask used by chromium's drag-target methods. Reads from
  // mouse_->held_modifiers_blink() if mouse_ is non-null, returns 0
  // otherwise.
  uint32_t CurrentModifiersBlink() const;

  // Resolved (resolver_->GetActiveWebContents() / GetRenderViewHost /
  // GetWidget) helper. Returns nullptr if any step in the chain
  // resolves to null; the caller is responsible for logging the
  // specific failure mode (so the log message names the dropped
  // envelope type for grep-ability).
  content::RenderWidgetHost* ResolveRwhOrNull() const;

  // ── State ─────────────────────────────────────────────────────────

  // Current phase. See enum doc for the transitions.
  CbDragPhase phase_ = CbDragPhase::kIdle;

  // Cached payload — refreshed on drag_start and overwritten on drop.
  // Kept around through kAwaitingEnd so that a hypothetical v2 spec
  // extension that surfaces the items on drag_end can read them
  // without re-parsing the envelope.
  std::vector<CbDragItem> cached_items_;
  std::vector<std::string> cached_types_;

  // Last successfully forwarded screen-space coords. Used to seed the
  // DragTargetDragLeave dispatch on cancel — the renderer expects the
  // leave to fire at the last-known pointer, not at (0,0).
  //
  // TODO(M4-R7-last-pos-default): the Go bridge sends dragCancel with
  // (0, 0) when no drag_over has fired; we instead synthesise at
  // last_widget_pos_ (defaulting to the drag_start coords if no
  // drag_over arrived). Confirm chromium's tolerance — a leave at
  // (0,0) might land on a different element than the drag_start did,
  // surfacing a spurious dragleave on the page.
  WidgetPoint last_widget_pos_{0.f, 0.f};

  // Once-per-instance bring-to-front latch. Same semantics as R3.
  bool brought_to_front_ = false;

  // M4 R2 resolver. Caller-owned; can be null pre-R2.
  // CV2-81 first-compile-link fix-forward (10th lesson-(i) ring,
  // lesson-(g.4) Discipline-shape): wrapped in raw_ptr<T> for
  // chromium-style lint compliance per 9d33276 precedent.
  const raw_ptr<WebContentsResolver> resolver_;

  // M4 R3 mouse instance — read-only, for the shared held-modifier
  // state. Caller-owned; can be null in unit tests.
  const raw_ptr<const CbInputDispatchMouse> mouse_;

  // dragOperationsMask. Hard-coded `kCopy` per v1; promoted to a
  // member so a v2 modifier-driven path can flip it without touching
  // call sites.
  //
  // Chromium uses blink::kDragOperationCopy / kDragOperationLink /
  // kDragOperationMove constants on this mask. Forward-declared in
  // the .cc to avoid pulling third_party/blink into the public header.
  int drag_operations_mask_;

  // dragOperationsMask the renderer last reported as the legal
  // operation set (DragTarget* return an updated mask the renderer
  // accepts). v1 ignores this; v2 may use it to drive the cursor
  // channel's drag-feedback overlay.
  //
  // TODO(M4-R7-renderer-mask-feedback): wire the renderer's accepted
  // operation mask back to the cursor channel (M5) so the client can
  // distinguish a drag-with-copy from a drag-with-no-drop cursor.
  // Until then this stays as `kCopy` regardless of what the page sets.
  int renderer_accepted_mask_;
};

}  // namespace cloud_browser

#endif  // CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_DRAG_H_

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cloud-browser/capture/build-integration/cb_input_dispatch_drag.h"

#include <string>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/drop_data.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/mojom/drag/drag.mojom-shared.h"
#include "ui/gfx/geometry/point_f.h"
#include "url/gurl.h"

namespace cloud_browser {

namespace {

// Spec MIME constants. Kept anonymous-namespace local so the public
// header doesn't leak the protocol's vocabulary.
constexpr char kMimePlain[] = "text/plain";
constexpr char kMimeHtml[] = "text/html";
constexpr char kMimeUriList[] = "text/uri-list";

// v1 protocol: dragOperationsMask is hard-coded copy (1) on every
// dispatch. The Go bridge does this in buildDragEvent; we mirror so
// the two backends produce parity DOM `dragenter.dataTransfer.
// effectAllowed` values.
//
// blink's WebDragOperationCopy is the cross-platform constant — but
// since the chromium type lives behind a mojom-shared include, we
// store the int and let the .cc convert at the call site.
//
// TODO(M4-R7-shared-blink-constants): use blink::kDragOperationCopy
// directly once the include is verified to work in M4's build (the
// header may be browser-process-only and we'd need
// kWebDragOperationCopy from a different layer instead).
constexpr int kDragOperationCopyV1 = 1;

// Parse the protocol's `items[]` array into our internal CbDragItem
// vector. Tolerates malformed entries (missing fields) by skipping
// them + logging — the protocol acceptance test for drag_start
// asserts servers MUST NOT crash on partial items.
std::vector<CbDragItem> ParseItems(const base::Value::List* items_list) {
  std::vector<CbDragItem> out;
  if (!items_list) {
    return out;
  }
  out.reserve(items_list->size());
  for (const base::Value& item_v : *items_list) {
    const base::Value::Dict* item_d = item_v.GetIfDict();
    if (!item_d) {
      LOG(WARNING) << "CbInputDispatchDrag: drag item not a dict; skipping";
      continue;
    }
    const std::string* kind = item_d->FindString("kind");
    const std::string* type = item_d->FindString("type");
    if (!kind || !type) {
      LOG(WARNING) << "CbInputDispatchDrag: drag item missing kind/type; "
                   << "skipping";
      continue;
    }
    CbDragItem out_item;
    out_item.kind = *kind;
    out_item.type = *type;
    if (*kind == "string") {
      const std::string* data = item_d->FindString("data");
      if (!data) {
        LOG(WARNING) << "CbInputDispatchDrag: string-kind drag item missing "
                     << "data; treating as empty";
        out_item.data.clear();
      } else {
        out_item.data = *data;
      }
    } else if (*kind == "file") {
      // v1 file-drag policy: file items carry no data. We accept the
      // absence and leave out_item.data empty. Reading file content
      // on the page side will yield empty, which is the documented v1
      // behaviour (see input-channel.md § v1 file-drag policy).
      out_item.data.clear();
    } else {
      LOG(WARNING) << "CbInputDispatchDrag: unknown item kind=" << *kind
                   << "; passing through with empty data";
      out_item.data.clear();
    }
    out.push_back(std::move(out_item));
  }
  return out;
}

std::vector<std::string> ParseTypes(const base::Value::List* types_list) {
  std::vector<std::string> out;
  if (!types_list) {
    return out;
  }
  out.reserve(types_list->size());
  for (const base::Value& t_v : *types_list) {
    if (const std::string* s = t_v.GetIfString()) {
      out.push_back(*s);
    }
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------
// CbInputDispatchDrag
// ---------------------------------------------------------------------

CbInputDispatchDrag::CbInputDispatchDrag(WebContentsResolver* resolver,
                                         const CbInputDispatchMouse* mouse)
    : resolver_(resolver),
      mouse_(mouse),
      drag_operations_mask_(kDragOperationCopyV1),
      renderer_accepted_mask_(kDragOperationCopyV1) {
  // resolver_ may be null during M4 R2 development — same tolerance
  // contract as R3. mouse_ may be null in unit tests; production
  // wiring supplies the same pointer that R3 uses.
}

CbInputDispatchDrag::~CbInputDispatchDrag() = default;

void CbInputDispatchDrag::OnInputEvent(InputEnvelope envelope) {
  // Already on BrowserThread::UI (M4 R1 hopped before invoking us).
  const base::TimeTicks now = base::TimeTicks::Now();
  if (envelope.type == "drag_start") {
    DispatchDragStart(envelope.data, now);
  } else if (envelope.type == "drag_over") {
    DispatchDragOver(envelope.data, now);
  } else if (envelope.type == "drop") {
    DispatchDrop(envelope.data, now);
  } else if (envelope.type == "drag_end") {
    DispatchDragEnd(envelope.data, now);
  }
  // else: silently ignore; not R7's type. The composite delegate
  // routes elsewhere.
}

// ---------------------------------------------------------------------
// drag_start
// ---------------------------------------------------------------------

void CbInputDispatchDrag::DispatchDragStart(const base::Value::Dict& data,
                                            base::TimeTicks event_time) {
  if (!resolver_) {
    LOG(WARNING) << "CbInputDispatchDrag: drag_start dropped — no "
                 << "WebContentsResolver wired (M4 R2 pending)";
    return;
  }
  content::WebContents* wc = resolver_->GetActiveWebContents();
  if (!wc) {
    LOG(WARNING) << "CbInputDispatchDrag: drag_start dropped — no "
                 << "active WebContents";
    return;
  }
  content::RenderWidgetHost* rwh = ResolveRwhOrNull();
  if (!rwh) {
    LOG(WARNING) << "CbInputDispatchDrag: drag_start dropped — no RWH";
    return;
  }

  EnsureBroughtToFront(wc);

  // Protocol shape: { "x": int, "y": int, "types": [string],
  //                   "items": [{kind, type, data}] }.
  const std::optional<int> x = data.FindInt("x");
  const std::optional<int> y = data.FindInt("y");
  if (!x || !y) {
    LOG(WARNING) << "CbInputDispatchDrag: drag_start missing x/y";
    return;
  }

  // ── PRINCIPAL-RISK BRANCH: re-entry from kActive / kAwaitingEnd ──
  //
  // The spec is silent on a second drag_start while a drag is already
  // in flight, but it can happen if the client crashes mid-drag and
  // reconnects, or if the user starts a second drag without releasing
  // the first (mouse-state desync). We force-clear by synthesising a
  // DragTargetDragLeave at the prior last-known pointer, then proceed
  // with the new drag.
  //
  // TODO(M4-R7-renter-drag-start): see header doc — verify race-free
  // in chromium's drag pipeline. The CDP path synthesises this
  // implicitly; we make it explicit so the trace is greppable.
  if (phase_ != CbDragPhase::kIdle) {
    LOG(WARNING) << "CbInputDispatchDrag: drag_start while phase=" << static_cast<int>(phase_)
                 << " — synthesising DragTargetDragLeave at last_widget_pos="
                 << last_widget_pos_.x << "," << last_widget_pos_.y;
    // TODO(M4-R7-rwh-api): the exact chromium API surface for
    // synthetic drag-target dispatch needs verification once M4
    // compiles against the patched chromium. Most likely candidates:
    //   * content::RenderWidgetHostImpl::DragTargetDragLeave() — the
    //     impl method invoked by content/browser/devtools/protocol/
    //     input_handler.cc's Input.dispatchDragEvent. Public-vs-impl
    //     visibility needs to be confirmed; the impl method may not
    //     be part of the stable browser-process API.
    //   * content::WebContents::DragSourceSystemDragEnded() — too
    //     coarse, ends drag from the source side only.
    //   * aura::client::DragDropDelegate::OnDragExited() — Aura-only,
    //     called by the OS drag pipeline (not our synthetic path).
    // For the draft we represent the intended call site; the fold-in
    // task will swap to the correct symbol.
    //
    // rwh->DragTargetDragLeave();  // TODO(M4-R7-rwh-api)
    phase_ = CbDragPhase::kIdle;
    cached_items_.clear();
    cached_types_.clear();
  }

  // Parse + cache payload. Done BEFORE state transition so a malformed
  // items array doesn't leave us in kActive with empty cached payload.
  const base::Value::List* items_list = data.FindList("items");
  const base::Value::List* types_list = data.FindList("types");
  cached_items_ = ParseItems(items_list);
  cached_types_ = ParseTypes(types_list);

  // Build the DropData payload from the cached state.
  content::DropData drop_data;
  BuildDropData(&drop_data);

  const WidgetPoint widget = ContentToWidget(rwh, *x, *y);
  last_widget_pos_ = widget;

  // ── Dispatch DragTargetDragEnter ──
  //
  // TODO(M4-R7-rwh-api): the canonical entry point for synthetic drag
  // dispatch is the same one CDP's Input.dispatchDragEvent uses —
  // content::RenderWidgetHostImpl::DragTargetDragEnter(drop_data,
  // client_pt, screen_pt, drag_operations_mask, modifiers,
  // base::DoNothing /* callback */). The public RenderWidgetHost
  // interface does NOT expose DragTarget* methods — they are Impl-
  // only. Two options at fold-in:
  //   (a) cast to RenderWidgetHostImpl (matches what CDP does; lives
  //       at content/browser/renderer_host/render_widget_host_impl.h)
  //   (b) route through a new content::WebContents-level surface
  //       that we add as part of M4 R2/R7 (cleaner but adds a
  //       chromium-side patch to track)
  // Default to (a) for the draft because it minimises chromium-patch
  // surface; revisit if the cast is ergonomically painful.
  //
  // The screen_pt argument matters: chromium's drag pipeline uses it
  // for cursor positioning during the drag preview. For our synthetic
  // case we don't have a real screen-space anchor; passing the widget-
  // space point matches the cb-chromium aura host's 1280x720 root-
  // window assumption (the host IS the screen in our embedder).
  //
  // rwh->DragTargetDragEnter(drop_data,
  //                          gfx::PointF(widget.x, widget.y),
  //                          gfx::PointF(widget.x, widget.y),
  //                          drag_operations_mask_,
  //                          CurrentModifiersBlink(),
  //                          base::DoNothing());

  phase_ = CbDragPhase::kActive;
  // Silence unused-parameter warnings while the RWH call is TODO'd
  // out; remove at fold-in.
  (void)event_time;
  (void)drop_data;
}

// ---------------------------------------------------------------------
// drag_over
// ---------------------------------------------------------------------

void CbInputDispatchDrag::DispatchDragOver(const base::Value::Dict& data,
                                           base::TimeTicks event_time) {
  // ── Spec compliance: stray drag_over outside active drag is no-op ──
  if (phase_ != CbDragPhase::kActive) {
    // Reason for VLOG over LOG(WARNING): the spec explicitly tolerates
    // this. A real client can race a delayed drag_over after drag_end
    // and we don't want to spam warnings.
    VLOG(1) << "CbInputDispatchDrag: drag_over outside active drag — no-op "
            << "(phase=" << static_cast<int>(phase_) << ")";
    return;
  }
  if (!resolver_) {
    LOG(WARNING) << "CbInputDispatchDrag: drag_over dropped — no "
                 << "WebContentsResolver wired (M4 R2 pending)";
    return;
  }
  content::RenderWidgetHost* rwh = ResolveRwhOrNull();
  if (!rwh) {
    LOG(WARNING) << "CbInputDispatchDrag: drag_over dropped — no RWH";
    return;
  }

  const std::optional<int> x = data.FindInt("x");
  const std::optional<int> y = data.FindInt("y");
  if (!x || !y) {
    LOG(WARNING) << "CbInputDispatchDrag: drag_over missing x/y";
    return;
  }
  const WidgetPoint widget = ContentToWidget(rwh, *x, *y);
  last_widget_pos_ = widget;

  // TODO(M4-R7-rwh-api): same impl-method concern as drag_start.
  // rwh->DragTargetDragOver(gfx::PointF(widget.x, widget.y),
  //                         gfx::PointF(widget.x, widget.y),
  //                         drag_operations_mask_,
  //                         CurrentModifiersBlink(),
  //                         base::DoNothing());

  (void)event_time;
}

// ---------------------------------------------------------------------
// drop
// ---------------------------------------------------------------------

void CbInputDispatchDrag::DispatchDrop(const base::Value::Dict& data,
                                       base::TimeTicks event_time) {
  // ── PRINCIPAL-RISK: drop while kIdle ──
  //
  // Malformed sequencing — drop without a preceding drag_start. The Go
  // bridge dispatches CDP's `drop` anyway (relying on chromium to
  // ignore it); we mirror that for parity. Strict alternative is
  // available — see header TODO.
  if (phase_ == CbDragPhase::kIdle) {
    LOG(WARNING) << "CbInputDispatchDrag: drop without prior drag_start — "
                 << "dispatching anyway for Go-bridge parity (see "
                 << "TODO(M4-R7-drop-without-start))";
  }
  if (!resolver_) {
    LOG(WARNING) << "CbInputDispatchDrag: drop dropped — no "
                 << "WebContentsResolver wired (M4 R2 pending)";
    return;
  }
  content::WebContents* wc = resolver_->GetActiveWebContents();
  if (!wc) {
    LOG(WARNING) << "CbInputDispatchDrag: drop dropped — no active WebContents";
    return;
  }
  content::RenderWidgetHost* rwh = ResolveRwhOrNull();
  if (!rwh) {
    LOG(WARNING) << "CbInputDispatchDrag: drop dropped — no RWH";
    return;
  }

  EnsureBroughtToFront(wc);

  // Protocol: drop re-asserts items + types. Overwrite cached state
  // in case the client mutated between drag_start and drop. Per the
  // input-bridge/main.go Dispatch() drop arm.
  const base::Value::List* items_list = data.FindList("items");
  const base::Value::List* types_list = data.FindList("types");
  cached_items_ = ParseItems(items_list);
  cached_types_ = ParseTypes(types_list);

  const std::optional<int> x = data.FindInt("x");
  const std::optional<int> y = data.FindInt("y");
  if (!x || !y) {
    LOG(WARNING) << "CbInputDispatchDrag: drop missing x/y";
    return;
  }

  content::DropData drop_data;
  BuildDropData(&drop_data);

  const WidgetPoint widget = ContentToWidget(rwh, *x, *y);
  last_widget_pos_ = widget;

  // ── Dispatch DragTargetDrop ──
  //
  // chromium's RenderWidgetHostImpl::DragTargetDrop signature is
  // (drop_data, client_pt, screen_pt, modifiers, callback) — note
  // that drop does NOT take a drag_operations_mask the way Enter/Over
  // do. The renderer picks one operation from the mask Enter
  // negotiated, exposes it on DataTransfer.dropEffect, and DragTarget
  // Drop honours whatever the renderer selected.
  //
  // TODO(M4-R7-rwh-api): same impl-method concern as drag_start.
  //
  // rwh->DragTargetDrop(drop_data,
  //                     gfx::PointF(widget.x, widget.y),
  //                     gfx::PointF(widget.x, widget.y),
  //                     CurrentModifiersBlink(),
  //                     base::DoNothing());

  // ── State transition ──
  //
  // Per spec: drop does NOT clear state. The follow-up drag_end with
  // success:true is what releases. We transition to kAwaitingEnd so
  // a malformed second drop is detectable (phase != kIdle at the
  // start of DispatchDrop).
  phase_ = CbDragPhase::kAwaitingEnd;
  (void)event_time;
  (void)drop_data;
}

// ---------------------------------------------------------------------
// drag_end — PRINCIPAL-RISK: five distinct transitions
// ---------------------------------------------------------------------

void CbInputDispatchDrag::DispatchDragEnd(const base::Value::Dict& data,
                                          base::TimeTicks event_time) {
  const std::optional<bool> success_opt = data.FindBool("success");
  if (!success_opt) {
    LOG(WARNING) << "CbInputDispatchDrag: drag_end missing success bool — "
                 << "treating as success=false";
  }
  const bool success = success_opt.value_or(false);

  switch (phase_) {
    case CbDragPhase::kIdle:
      // drag_end outside an active drag — informational, no-op.
      // Matches input-bridge/main.go's behaviour.
      VLOG(1) << "CbInputDispatchDrag: drag_end while idle — no-op";
      return;

    case CbDragPhase::kActive: {
      // Two sub-cases:
      //   * success=true while active: malformed (drop never fired).
      //     Still dispatch DragTargetDragLeave to keep renderer state
      //     consistent. Loud log so we notice the client bug.
      //   * success=false while active: normal cancel path.
      if (success) {
        LOG(WARNING) << "CbInputDispatchDrag: drag_end success=true while "
                     << "kActive — drop never fired? Dispatching leave to "
                     << "release renderer state.";
      }
      content::RenderWidgetHost* rwh = ResolveRwhOrNull();
      if (rwh) {
        // TODO(M4-R7-rwh-api): see drag_start.
        // rwh->DragTargetDragLeave(/* client_pt */, /* screen_pt */);
        (void)rwh;
      } else {
        LOG(WARNING) << "CbInputDispatchDrag: drag_end cancel-path could not "
                     << "resolve RWH — state cleared without dispatch";
      }
      break;
    }

    case CbDragPhase::kAwaitingEnd: {
      // drop already fired. Two sub-cases:
      //   * success=true: normal success path — release state silently.
      //   * success=false: cancel-after-drop. Unusual but spec-legal.
      //     Synthesise a DragTargetDragLeave so the renderer fires
      //     dragend(success=false). The drop already mutated the
      //     page; we cannot un-drop, but we CAN report the cancel
      //     via the dragend handler.
      if (!success) {
        LOG(WARNING) << "CbInputDispatchDrag: drag_end success=false after "
                     << "drop — synthesising DragTargetDragLeave for "
                     << "dragend(success=false) DOM event. NOTE: the drop "
                     << "side effects are NOT reversed.";
        content::RenderWidgetHost* rwh = ResolveRwhOrNull();
        if (rwh) {
          // TODO(M4-R7-rwh-api): see drag_start.
          // rwh->DragTargetDragLeave(/* client_pt */, /* screen_pt */);
          (void)rwh;
        }
      }
      // success=true → no dispatch needed.
      break;
    }
  }

  // Clear state regardless of branch taken.
  phase_ = CbDragPhase::kIdle;
  cached_items_.clear();
  cached_types_.clear();
  // last_widget_pos_ intentionally NOT cleared — leaves the last-known
  // anchor for a hypothetical immediately-following drag_start that
  // wants to seed its synthetic leave correctly.

  (void)event_time;
}

// ---------------------------------------------------------------------
// DropData marshalling
// ---------------------------------------------------------------------

void CbInputDispatchDrag::BuildDropData(content::DropData* out) const {
  DCHECK(out);
  // chromium's DropData layout is "field-per-MIME":
  //   * `text`     — text/plain (base::NullableString16)
  //   * `html`     — text/html  (base::NullableString16) + html_base_url
  //   * `url`      — text/uri-list, first entry (GURL)
  //   * `url_title`— page-side title for the URL (string16)
  //   * `custom_data` — std::unordered_map<std::u16string, std::u16string>
  //                     for application-defined MIME types
  //   * `filenames`— file-kind items (vector<DropData::FileInfo>)
  //
  // We walk the cached_items_ vector once and slot each item into the
  // appropriate bucket. Items with kind=="file" land in filenames with
  // empty path (v1 file-drag policy — see header doc).
  //
  // PRINCIPAL-RISK: DropData fields differ across chromium versions.
  // For the draft we use the field names that have been stable across
  // 124..132; the fold-in compile against the patched chromium will
  // catch any drift.
  //
  // TODO(M4-R7-dropdata-fidelity): cross-check against the real
  // content/public/common/drop_data.h once a chromium tree is
  // available. NullableString16's accessor for "set" varies (some
  // versions use `std::optional<std::u16string>`).

  for (const CbDragItem& item : cached_items_) {
    if (item.kind == "file") {
      // v1 file policy: empty path + empty display_name; the page
      // sees the MIME in DataTransfer.types but file content is empty.
      //
      // TODO(M4-R7-file-info-shape): chromium's DropData::FileInfo
      // signature has shifted (base::FilePath path vs std::string
      // url). Pick the right one at fold-in.
      //
      // content::DropData::FileInfo file_info;
      // file_info.path = base::FilePath();
      // file_info.display_name = std::u16string();
      // out->filenames.push_back(std::move(file_info));
      (void)out;
      continue;
    }

    // kind == "string" (and tolerated unknown kinds — same path).
    const std::u16string data16 = base::UTF8ToUTF16(item.data);
    if (item.type == kMimePlain) {
      // out->text = data16;
      (void)data16;
    } else if (item.type == kMimeHtml) {
      // out->html = data16;
      // out->html_base_url = GURL();  // v1 has no base-URL signal
      (void)data16;
    } else if (item.type == kMimeUriList) {
      // text/uri-list can carry multiple newline-separated URLs. v1
      // protocol emits one item per URL; the renderer's DataTransfer
      // exposes them via `DataTransfer.getData("text/uri-list")`. We
      // store the first URL on DropData.url (chromium uses it for
      // the dropped-URL convenience accessors), and the full payload
      // is recoverable via the renderer-side getData() call against
      // the custom_data map.
      //
      // PRINCIPAL-RISK: DropData has historically been single-URL on
      // url and the multi-URL case has been wonky. v2 may want a
      // proper uri-list channel.
      //
      // out->url = GURL(item.data);
      // out->custom_data[base::UTF8ToUTF16(item.type)] = data16;
      (void)data16;
    } else {
      // Application MIME — text/custom or vendor-specific. Lands in
      // custom_data.
      //
      // out->custom_data[base::UTF8ToUTF16(item.type)] = data16;
      (void)data16;
    }
  }

  // TODO(M4-R7-dropdata-cache): we re-marshall on every dispatch
  // call. For a drag with many items + many drag_over events this is
  // O(items * over_count). Optimise by caching the built DropData on
  // drag_start/drop and reusing on drag_over. Deferred — coalescing
  // already bounds drag_over rate per spec.
}

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

CbInputDispatchDrag::WidgetPoint
CbInputDispatchDrag::ContentToWidget(content::RenderWidgetHost* rwh,
                                     int content_x, int content_y) const {
  // Mirror R3's implementation exactly. See header TODO(M4-R7-share-
  // coord-map) for the planned collapse.
  float dsf = 1.f;
  if (auto* view = rwh ? rwh->GetView() : nullptr) {
    const float view_dsf = view->GetDeviceScaleFactor();
    if (view_dsf > 0.f) {
      dsf = view_dsf;
    }
  }
  return {
      static_cast<float>(content_x) / dsf,
      static_cast<float>(content_y) / dsf,
  };
}

void CbInputDispatchDrag::EnsureBroughtToFront(content::WebContents* wc) {
  if (brought_to_front_) {
    return;
  }
  if (!wc) {
    return;
  }
  // Same body as R3's EnsureBroughtToFront. WasShown() + RWHV::Focus()
  // together set the focus chain the renderer's drag pipeline needs
  // to deliver synthetic drag events to the focused frame.
  //
  // TODO(M4-R7-share-activation-latch): collapse with R3 at fold-in.
  wc->WasShown();
  if (auto* view = wc->GetRenderWidgetHostView()) {
    view->Focus();
  }
  brought_to_front_ = true;
}

uint32_t CbInputDispatchDrag::CurrentModifiersBlink() const {
  if (!mouse_) {
    return 0;
  }
  // Read R3's shared state. Held-MOUSE-button bits are NOT included
  // in the drag modifiers field — chromium's drag pipeline derives
  // the "drag is in flight" state from the DragTarget* calls
  // themselves, not from the modifier bits.
  //
  // We only forward the four named modifier keys (Shift / Ctrl / Alt
  // / Meta) plus any modifier bits R4 has set. Held-button bits are
  // masked out via blink::WebInputEvent::Modifiers::kKeyModifiers.
  //
  // TODO(M4-R7-key-modifiers-mask): replace this comment with the
  // actual mask constant once the header is verified to expose it
  // (it's defined as kKeyModifiers in web_input_event.h, but the
  // value has been promoted between blink-major versions).
  const uint32_t key_modifiers_mask =
      blink::WebInputEvent::Modifiers::kShiftKey |
      blink::WebInputEvent::Modifiers::kControlKey |
      blink::WebInputEvent::Modifiers::kAltKey |
      blink::WebInputEvent::Modifiers::kMetaKey;
  return mouse_->held_modifiers_blink() & key_modifiers_mask;
}

content::RenderWidgetHost* CbInputDispatchDrag::ResolveRwhOrNull() const {
  if (!resolver_) {
    return nullptr;
  }
  content::WebContents* wc = resolver_->GetActiveWebContents();
  if (!wc) {
    return nullptr;
  }
  auto* rwhv = wc->GetRenderWidgetHostView();
  return rwhv ? rwhv->GetRenderWidgetHost() : nullptr;
}

void CbInputDispatchDrag::ResetStateForTesting() {
  phase_ = CbDragPhase::kIdle;
  cached_items_.clear();
  cached_types_.clear();
  last_widget_pos_ = WidgetPoint{0.f, 0.f};
  brought_to_front_ = false;
}

}  // namespace cloud_browser

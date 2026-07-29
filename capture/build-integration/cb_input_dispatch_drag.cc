// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cloud-browser/capture/build-integration/cb_input_dispatch_drag.h"

#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback_helpers.h"  // base::DoNothing
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/drop_data.h"
#include "third_party/blink/public/common/input/web_input_event.h"
// blink::DragOperationsMask, the enum the Enter/Over calls take. Pulled in
// transitively by render_widget_host.h:25, but named here per IWYU since we
// static_cast to it directly.
#include "third_party/blink/public/common/page/drag_operation.h"
#include "third_party/blink/public/mojom/drag/drag.mojom-shared.h"
#include "ui/base/clipboard/file_info.h"  // ui::FileInfo for DropData::filenames
#include "ui/gfx/geometry/point_f.h"
#include "url/gurl.h"

namespace cloud_browser {

// ---------------------------------------------------------------------
// CV2-82 CLOSEOUT — the DragTarget* calls are live.
//
// CV2-81 wired R7's state machine end-to-end (phase transitions, payload
// caching, coord-mapping, EnsureBroughtToFront, modifier reads from R3)
// but left the SIX `rwh->DragTarget*` dispatch calls commented out
// pending chromium-7727 API verification. Until this change, drag was
// the ONLY input path that ran its full plumbing and then dispatched
// nothing — the renderer never saw a synthetic drag event, so
// drag-and-drop silently did nothing on every site.
//
// All three open questions are now resolved against the pinned tree
// (147.0.7727.144 — see docs/build/chromium-7727-api-pins.md, read from
// the build node rather than from memory):
//
//   * NO CAST IS NEEDED. The premise of the deferral above — that the
//     DragTarget* family is RenderWidgetHostImpl-only — is FALSE at
//     7727. All four methods plus FilterDropData are public virtuals
//     on content::RenderWidgetHost itself (render_widget_host.h:306,
//     319, 324, 326, 344). So this file needs no content/browser
//     include and no cb_friends visibility entry, unlike the M4 R3
//     mouse and R6 touch paths which genuinely do reach into Impl.
//   * Signatures: render_widget_host.h:306-331. DragTargetDrop takes
//     NO operations mask (unlike Enter/Over) and a plain
//     base::OnceClosure; Enter/Over take blink::DragOperationsMask +
//     int key_modifiers + DragOperationCallback (which is
//     OnceCallback<void(ui::mojom::DragOperation, bool)> — base::
//     DoNothing() adapts). DragTargetDragLeave takes BOTH points.
//   * FilterDropData is MANDATORY and was NOT in the original plan:
//     render_widget_host_impl.h:283 and :306 both say "drop_data must
//     have been filtered. The embedder should call FilterDropData
//     before passing the drop data to RWHI." Skipping it would hand
//     the renderer unfiltered paths/URLs.
//   * DropData::view_id must be set to the target widget's routing id
//     before Enter — chromium's own CDP caller does this at
//     input_handler.cc:949, and the field defaults to kRoutingIdNone.
//
// Call shapes below mirror content/browser/devtools/protocol/
// input_handler.cc:950 and :1084 — chromium's own synthetic-drag
// caller — so this path is exercised by the same code the CDP
// Input.dispatchDragEvent tests cover.
//
// Still deliberately NOT done here (each is its own frontier):
//   * The drag SOURCE side (DragSourceEndedAt / DragSourceSystemDragEnded)
//     — we only synthesise drags INTO the page, which is what the
//     protocol models. A page-initiated drag OUT is unmodelled.
//   * File drags carry empty FilePaths (see BuildDropData): the guest
//     has no host file to point at until the M6 file-transfer relay
//     lands. Text/HTML/URI-list payloads are complete.
// ---------------------------------------------------------------------

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
std::vector<CbDragItem> ParseItems(const base::ListValue* items_list) {
  std::vector<CbDragItem> out;
  if (!items_list) {
    return out;
  }
  out.reserve(items_list->size());
  for (const base::Value& item_v : *items_list) {
    const base::DictValue* item_d = item_v.GetIfDict();
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

std::vector<std::string> ParseTypes(const base::ListValue* types_list) {
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

void CbInputDispatchDrag::DispatchDragStart(const base::DictValue& data,
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
    // Force-clear the stale drag before starting the new one. Chromium's
    // drag pipeline keeps per-widget target state; entering twice without an
    // intervening leave leaves the previous target latched and the new
    // dragenter is dropped by blink as a duplicate.
    const gfx::PointF stale_pt(static_cast<float>(last_widget_pos_.x),
                               static_cast<float>(last_widget_pos_.y));
    rwh->DragTargetDragLeave(stale_pt, stale_pt);
    phase_ = CbDragPhase::kIdle;
    cached_items_.clear();
    cached_types_.clear();
  }

  // Parse + cache payload. Done BEFORE state transition so a malformed
  // items array doesn't leave us in kActive with empty cached payload.
  const base::ListValue* items_list = data.FindList("items");
  const base::ListValue* types_list = data.FindList("types");
  cached_items_ = ParseItems(items_list);
  cached_types_ = ParseTypes(types_list);

  // Build the DropData payload from the cached state.
  content::DropData drop_data;
  BuildDropData(&drop_data);

  const WidgetPoint widget = ContentToWidget(rwh, *x, *y);
  last_widget_pos_ = widget;

  // ── Dispatch DragTargetDragEnter ──
  //
  // Same entry point CDP's Input.dispatchDragEvent uses. The screen_pt
  // argument matters: chromium's drag pipeline uses it for cursor
  // positioning during the drag preview. We have no real screen-space
  // anchor, and in this embedder the aura host IS the screen (a single
  // headless display rooted at 0,0 — see CbHeadlessScreen), so widget-space
  // and screen-space coincide.
  //
  // Route the payload at this specific widget, then filter it. Both steps
  // mirror chromium's own synthetic-drag caller at input_handler.cc:949-952:
  // view_id defaults to kRoutingIdNone, and FilterDropData strips paths/URLs
  // the renderer must not see (render_widget_host_impl.h:283).
  drop_data.view_id = rwh->GetRoutingID();
  rwh->FilterDropData(&drop_data);
  const gfx::PointF pt(static_cast<float>(widget.x),
                       static_cast<float>(widget.y));
  // static_cast<int> on the modifiers: CurrentModifiersBlink returns uint32_t
  // (blink's modifier bitfield width) while the parameter is int. Same
  // explicit narrowing the M4 R3 mouse path does at
  // cb_input_dispatch_mouse.cc:199 — implicit would trip -Wconversion.
  rwh->DragTargetDragEnter(
      drop_data, pt, pt,
      static_cast<blink::DragOperationsMask>(drag_operations_mask_),
      static_cast<int>(CurrentModifiersBlink()), base::DoNothing());

  phase_ = CbDragPhase::kActive;
  // event_time is carried by the protocol for trace correlation; chromium's
  // drag pipeline timestamps internally and takes no time argument.
  (void)event_time;
}

// ---------------------------------------------------------------------
// drag_over
// ---------------------------------------------------------------------

void CbInputDispatchDrag::DispatchDragOver(const base::DictValue& data,
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

  // No DropData on drag_over: chromium keeps the payload from DragTargetDragEnter
  // for the life of the drag, so only geometry + modifiers move here.
  const gfx::PointF pt(static_cast<float>(widget.x),
                       static_cast<float>(widget.y));
  rwh->DragTargetDragOver(
      pt, pt, static_cast<blink::DragOperationsMask>(drag_operations_mask_),
      static_cast<int>(CurrentModifiersBlink()), base::DoNothing());

  (void)event_time;
}

// ---------------------------------------------------------------------
// drop
// ---------------------------------------------------------------------

void CbInputDispatchDrag::DispatchDrop(const base::DictValue& data,
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
  const base::ListValue* items_list = data.FindList("items");
  const base::ListValue* types_list = data.FindList("types");
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
  // Verified at render_widget_host_impl.h:307 — drop takes
  // (drop_data, client_pt, screen_pt, key_modifiers, callback) and does NOT
  // take a drag_operations_mask the way Enter/Over do. The renderer picks one
  // operation from the mask Enter negotiated, exposes it on
  // DataTransfer.dropEffect, and DragTargetDrop honours that selection. The
  // callback is a plain base::OnceClosure here, not a DragOperationCallback.
  //
  // FilterDropData again: this is a second, independently-filtered payload
  // (render_widget_host_impl.h:306), not the one Enter already filtered.
  drop_data.view_id = rwh->GetRoutingID();
  rwh->FilterDropData(&drop_data);
  const gfx::PointF pt(static_cast<float>(widget.x),
                       static_cast<float>(widget.y));
  rwh->DragTargetDrop(drop_data, pt, pt,
                      static_cast<int>(CurrentModifiersBlink()),
                      base::DoNothing());

  // ── State transition ──
  //
  // Per spec: drop does NOT clear state. The follow-up drag_end with
  // success:true is what releases. We transition to kAwaitingEnd so
  // a malformed second drop is detectable (phase != kIdle at the
  // start of DispatchDrop).
  phase_ = CbDragPhase::kAwaitingEnd;
  (void)event_time;
}

// ---------------------------------------------------------------------
// drag_end — PRINCIPAL-RISK: five distinct transitions
// ---------------------------------------------------------------------

void CbInputDispatchDrag::DispatchDragEnd(const base::DictValue& data,
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
        // Leave at the last-known pointer: the protocol carries no
        // coordinates on drag_end, and chromium needs a point to route the
        // leave to the correct target widget.
        const gfx::PointF pt(static_cast<float>(last_widget_pos_.x),
                             static_cast<float>(last_widget_pos_.y));
        rwh->DragTargetDragLeave(pt, pt);
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
          const gfx::PointF pt(static_cast<float>(last_widget_pos_.x),
                               static_cast<float>(last_widget_pos_.y));
          rwh->DragTargetDragLeave(pt, pt);
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
      // v1 file policy: empty path + empty display_name; the page sees the
      // MIME in DataTransfer.types but file content is empty. A real file
      // drag needs a host-side file the guest can point at, which arrives
      // with the M6 file-transfer relay.
      //
      // Verified at drop_data.h:106 — `filenames` is
      // std::vector<ui::FileInfo>, NOT a nested content::DropData::FileInfo
      // (which is what the retired TODO here guessed). ui::FileInfo's ctor
      // is (base::FilePath path, base::FilePath display_name) — display_name
      // is a FilePath too, not a u16string.
      out->filenames.emplace_back(base::FilePath(), base::FilePath());
      continue;
    }

    // kind == "string" (and tolerated unknown kinds — same path).
    const std::u16string data16 = base::UTF8ToUTF16(item.data);
    if (item.type == kMimePlain) {
      // drop_data.h:117 — std::optional<std::u16string>; assigning a
      // u16string engages the optional.
      out->text = data16;
    } else if (item.type == kMimeHtml) {
      out->html = data16;                // drop_data.h:122, same optional shape
      out->html_base_url = GURL();       // v1 has no base-URL signal
    } else if (item.type == kMimeUriList) {
      // text/uri-list can carry multiple newline-separated URLs. v1 protocol
      // emits one item per URL; the renderer's DataTransfer exposes them via
      // DataTransfer.getData("text/uri-list"). We store the URL on
      // DropData.url (chromium's dropped-URL convenience accessors read it)
      // and also mirror the raw payload into custom_data so getData() sees
      // the exact bytes the client sent.
      //
      // PRINCIPAL-RISK (unchanged from the draft): DropData.url is single-
      // valued, so with multiple uri-list items the LAST one wins on .url.
      // custom_data has the same collision. A multi-URL drag therefore
      // degrades to its final URL on the convenience accessor. v2 wants a
      // proper uri-list channel.
      out->url = GURL(item.data);
      out->custom_data[base::UTF8ToUTF16(item.type)] = data16;
    } else {
      // Application MIME — text/custom or vendor-specific. Lands in
      // custom_data (drop_data.h:132, std::unordered_map<u16string,u16string>).
      out->custom_data[base::UTF8ToUTF16(item.type)] = data16;
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

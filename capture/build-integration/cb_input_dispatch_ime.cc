// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cloud-browser/capture/build-integration/cb_input_dispatch_ime.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/ime/ime_text_span.h"
#include "ui/gfx/range/range.h"

// Forward-declared in the header to keep ui/base/ime out of public-include
// land. The cast to content::RenderWidgetHostImpl happens here in the .cc
// because the Ime* methods we need are on the Impl, not the public
// interface — the public RenderWidgetHost does not expose ImeSetComposition
// / ImeCommitText.
//
// TODO(M4-R5-public-ime-api): if chromium exposes a public-facing wrapper
// for these (e.g. via content::WebContents::ImeSetComposition), switch to
// that to keep this file out of the content/browser/ internals. As of
// chromium 131-ish the Ime* methods are private to RenderWidgetHostImpl
// and reached via static_cast. Confirm the supported entry point during
// the first compile pass.
#include "content/browser/renderer_host/render_widget_host_impl.h"  // nogncheck

namespace cloud_browser {

namespace {

// Build the single full-string ui::ImeTextSpan that the renderer uses to
// draw the composition underline. R5 produces one default-styled span
// covering the entire composing string. v2 will pull per-segment styles
// from a future client-side IME bridge — see
// TODO(M4-R5-ime-text-spans).
std::vector<ui::ImeTextSpan> BuildDefaultCompositionSpans(
    size_t text_length_utf16) {
  std::vector<ui::ImeTextSpan> spans;
  if (text_length_utf16 == 0) {
    // Empty composition (composition_start with empty data, or a
    // composition_cancel-style clear). The renderer accepts an empty
    // span list and will collapse the composition visualisation.
    return spans;
  }
  ui::ImeTextSpan span;
  span.type = ui::ImeTextSpan::Type::kComposition;
  span.start_offset = 0;
  span.end_offset = text_length_utf16;
  span.underline_color = SK_ColorTRANSPARENT;
  span.thickness = ui::ImeTextSpan::Thickness::kThin;
  span.background_color = SK_ColorTRANSPARENT;
  spans.push_back(span);
  return spans;
}

// Pull `data` (UTF-8 in the wire envelope) out of the dict and convert
// to UTF-16 because blink's IME APIs take std::u16string. Returns empty
// on a missing / non-string value — composition_start with empty data
// is valid per the protocol.
std::u16string ReadDataAsU16(const base::DictValue& dict) {
  const std::string* s = dict.FindString("data");
  if (!s || s->empty()) {
    return std::u16string();
  }
  return base::UTF8ToUTF16(*s);
}

// Clamp [0, text_len_utf16] for a JSON-supplied caret offset. Mirrors
// the Go bridge's clampInt() so v1.1 envelopes with out-of-range
// selection_start/selection_end never wedge the renderer.
int ClampInt(int value, int lo, int hi) {
  return std::max(lo, std::min(hi, value));
}

// Reads an optional integer field. Returns true + populates |out| if
// the field is present AND is an int; returns false if missing or
// non-int (the caller falls back to the "caret at end" default).
bool ReadOptionalInt(const base::DictValue& dict,
                     const char* key,
                     int* out) {
  const auto opt = dict.FindInt(key);
  if (!opt.has_value()) {
    return false;
  }
  *out = *opt;
  return true;
}

// Cast helper. Mirrors the pattern used elsewhere in capture/build-
// integration to reach the Impl-only IME methods.
content::RenderWidgetHostImpl* AsImpl(content::RenderWidgetHost* rwh) {
  return static_cast<content::RenderWidgetHostImpl*>(rwh);
}

}  // namespace

CbInputDispatchIme::CbInputDispatchIme(WebContentsResolver* resolver)
    : resolver_(resolver) {}

CbInputDispatchIme::~CbInputDispatchIme() = default;

void CbInputDispatchIme::OnInputEvent(InputEnvelope envelope) {
  // Filter to composition_* — anything else this delegate sees in the
  // pre-composite-delegate wiring is silently dropped (R3 / R4 / R7 /
  // R8 own non-IME types).
  //
  // TODO(M4-R5-composite-delegate): drop this filter once R8 lands.
  const std::string& type = envelope.type;
  const base::TimeTicks event_time = base::TimeTicks::Now();

  if (type == "composition_start") {
    DispatchCompositionStart(envelope.data, event_time);
  } else if (type == "composition_update") {
    DispatchCompositionUpdate(envelope.data, event_time);
  } else if (type == "composition_end") {
    DispatchCompositionEnd(envelope.data, event_time);
  } else if (type == "composition_cancel") {
    DispatchCompositionCancel(event_time);
  }
  // Else: not an IME envelope. Drop silently — see above.
}

void CbInputDispatchIme::DispatchCompositionStart(
    const base::DictValue& data,
    base::TimeTicks /*event_time*/) {
  content::RenderWidgetHost* rwh = ResolveFocusedRenderWidgetHost();
  if (!rwh) {
    LOG(WARNING) << "cb-ime: composition_start dropped — no focused RWH";
    return;
  }

  // composition_start.data is typically empty (the user has begun
  // composing but the composing string is initially empty). Some IMEs
  // may emit a non-empty start with a pre-seeded buffer; we forward
  // it as-is so the renderer's compositionstart fires with the same
  // payload either backend produces.
  const std::u16string text = ReadDataAsU16(data);
  const size_t len_u16 = text.size();

  int selection_start = static_cast<int>(len_u16);
  int selection_end = static_cast<int>(len_u16);
  int sel = 0;
  if (ReadOptionalInt(data, "selection_start", &sel)) {
    selection_start = ClampInt(sel, 0, static_cast<int>(len_u16));
    selection_end = selection_start;
  }
  if (ReadOptionalInt(data, "selection_end", &sel)) {
    selection_end = ClampInt(sel, 0, static_cast<int>(len_u16));
  }

  // The optional `rect` field from composition_start is reserved for
  // a future client-side candidate-positioning UI per
  // input-channel.md — servers MAY ignore. R5 ignores. v2 may forward
  // it to chromium's candidate-window service if we add one.

  AsImpl(rwh)->ImeSetComposition(text, BuildDefaultCompositionSpans(len_u16),
                                 gfx::Range::InvalidRange(), selection_start,
                                 selection_end);
  is_composing_ = true;
}

void CbInputDispatchIme::DispatchCompositionUpdate(
    const base::DictValue& data,
    base::TimeTicks /*event_time*/) {
  content::RenderWidgetHost* rwh = ResolveFocusedRenderWidgetHost();
  if (!rwh) {
    LOG(WARNING) << "cb-ime: composition_update dropped — no focused RWH";
    return;
  }

  // composition_update.data is the current in-progress composing
  // string. selection_start / selection_end default to caret-at-end
  // per input-channel.md if absent.
  const std::u16string text = ReadDataAsU16(data);
  const size_t len_u16 = text.size();

  int selection_start = static_cast<int>(len_u16);
  int selection_end = static_cast<int>(len_u16);
  int sel = 0;
  if (ReadOptionalInt(data, "selection_start", &sel)) {
    selection_start = ClampInt(sel, 0, static_cast<int>(len_u16));
    selection_end = selection_start;
  }
  if (ReadOptionalInt(data, "selection_end", &sel)) {
    selection_end = ClampInt(sel, 0, static_cast<int>(len_u16));
  }

  // candidate_list is forwarded only to a future client-side
  // candidate UI per the protocol; v1.1 native dispatch ignores it.

  AsImpl(rwh)->ImeSetComposition(text, BuildDefaultCompositionSpans(len_u16),
                                 gfx::Range::InvalidRange(), selection_start,
                                 selection_end);
  // Defensive: composition_update with no prior composition_start is a
  // malformed sequence per input-channel.md, but we still want to
  // reflect the renderer-visible state — the renderer is now drawing
  // composition underlines, so we ARE composing. Setting the flag
  // unconditionally also covers the case where composition_start was
  // dropped due to no-focused-RWH but composition_update arrived once
  // focus was established.
  is_composing_ = true;
}

void CbInputDispatchIme::DispatchCompositionEnd(
    const base::DictValue& data,
    base::TimeTicks event_time) {
  content::RenderWidgetHost* rwh = ResolveFocusedRenderWidgetHost();
  if (!rwh) {
    LOG(WARNING) << "cb-ime: composition_end dropped — no focused RWH";
    // Clear the composing flag anyway so a subsequent key_* event
    // isn't suppressed by R4 due to a state machine mismatch.
    is_composing_ = false;
    return;
  }

  // composition_end.data is the final committed string. It is
  // authoritative even if intermediate composition_update events
  // were dropped (input-channel.md §"key suppression / final
  // string"), so the renderer-visible result is always
  // text==envelope.data.
  const std::u16string text = ReadDataAsU16(data);

  // ImeCommitText replaces the in-progress composition with `text`
  // and clears the composition state in the renderer. relative_cursor
  // _pos=0 places the caret at the end of the committed text — the
  // chromium-standard default and the behaviour the CDP
  // Input.insertText path produces in the Go bridge.
  //
  // We use ImeCommitText rather than InsertText() because the latter
  // bypasses the IME state machine and is wrong for committing a
  // composition: the renderer is already drawing composition
  // underlines and would not clear them.
  AsImpl(rwh)->ImeCommitText(text, BuildDefaultCompositionSpans(text.size()),
                             gfx::Range::InvalidRange(),
                             /*relative_cursor_pos=*/0);

  is_composing_ = false;
  last_composition_.last_committed_text = text;
  last_composition_.at = event_time;
}

void CbInputDispatchIme::DispatchCompositionCancel(
    base::TimeTicks /*event_time*/) {
  content::RenderWidgetHost* rwh = ResolveFocusedRenderWidgetHost();
  if (!rwh) {
    LOG(WARNING) << "cb-ime: composition_cancel dropped — no focused RWH";
    is_composing_ = false;
    return;
  }

  // T88 / input-channel.md: discard any in-progress composition state.
  // Native equivalent of the Go bridge's "imeSetComposition with empty
  // text" is RWHI::ImeCancelComposition(), which both clears the
  // composing string AND fires compositionend with empty data in the
  // renderer — matching the DOM-event sequence a user-driven Escape
  // would produce.
  AsImpl(rwh)->ImeCancelComposition();
  is_composing_ = false;
  // last_composition_ is intentionally NOT updated — a cancel
  // committed nothing, so the snapshot keeps the most recent real
  // commit so a passing test can distinguish "committed X then
  // cancelled Y" from "cancelled".
}

content::RenderWidgetHost* CbInputDispatchIme::ResolveFocusedRenderWidgetHost() {
  if (!resolver_) {
    LOG(WARNING) << "cb-ime: no WebContentsResolver — composition dropped "
                    "(pre-M4 R2)";
    return nullptr;
  }
  content::WebContents* wc = resolver_->GetActiveWebContents();
  if (!wc) {
    return nullptr;
  }

  // Use the focused frame so a focused <iframe>'s RWH receives the
  // composition. Falls back to the primary main frame's RWHV if no
  // focused frame is reported (which can happen briefly during
  // navigation).
  content::RenderFrameHost* rfh = wc->GetFocusedFrame();
  if (!rfh) {
    rfh = wc->GetPrimaryMainFrame();
  }
  if (!rfh) {
    return nullptr;
  }
  content::RenderWidgetHostView* rwhv = rfh->GetView();
  if (!rwhv) {
    return nullptr;
  }
  return rwhv->GetRenderWidgetHost();
}

}  // namespace cloud_browser

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cb_cursor_envelope.cc — see cb_cursor_envelope.h.

#include "capture/cursor/cb_cursor_envelope.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/json/json_writer.h"
#include "base/time/time.h"
#include "base/values.h"

namespace cloud_browser {
namespace cursor {

namespace {

// Wire constants. Kept in one place so a future v2 bump touches a
// single edit site (and the negative-tests can reference them too).
constexpr int kProtocolVersion = 1;
constexpr std::string_view kEnvelopeTypeCursor = "cursor";
constexpr std::string_view kShapeNone = "none";
constexpr std::string_view kShapeDefault = "default";
constexpr std::string_view kShapeCustom = "custom";

// CSS-keyword strings for each ui::mojom::CursorType. The canonical
// list lives in docs/protocols/cursor-channel.md § "Shape names" —
// this switch MUST stay in sync. The footguns called out in the
// header (kPointer → "default", kHand → "pointer") are tested in
// cb_cursor_envelope_test.cc as the first two fixture cases.
//
// Any new ui::mojom::CursorType enumerator added by a future chromium
// uprev falls through to the default-arm "default" string — that's
// safe per the protocol's "Unknown CSS keywords" clause (the receiver
// renders default rather than dropping the envelope).
std::string_view CssKeywordForType(ui::mojom::CursorType type) {
  using T = ui::mojom::CursorType;
  switch (type) {
    // Default arrow + the well-known "looks-clickable" pointing hand.
    // CSS swaps the names relative to the chromium enum — see header.
    case T::kPointer:       return kShapeDefault;
    case T::kHand:          return "pointer";

    // Text caret + cross-hair.
    case T::kIBeam:         return "text";
    case T::kVerticalText:  return "vertical-text";
    case T::kCross:         return "crosshair";

    // Wait / progress / help / context-menu indicators.
    case T::kWait:          return "wait";
    case T::kProgress:      return "progress";
    case T::kHelp:          return "help";
    case T::kContextMenu:   return "context-menu";

    // Permission / drag-state cursors.
    case T::kCell:          return "cell";
    case T::kAlias:         return "alias";
    case T::kCopy:          return "copy";
    case T::kNoDrop:        return "no-drop";
    case T::kNotAllowed:    return "not-allowed";
    case T::kGrab:          return "grab";
    case T::kGrabbing:      return "grabbing";

    // Move + zoom.
    case T::kMove:          return "move";
    case T::kZoomIn:        return "zoom-in";
    case T::kZoomOut:       return "zoom-out";

    // Eight compass-direction resize cursors.
    case T::kEastResize:        return "e-resize";
    case T::kNorthResize:       return "n-resize";
    case T::kNorthEastResize:   return "ne-resize";
    case T::kNorthWestResize:   return "nw-resize";
    case T::kSouthResize:       return "s-resize";
    case T::kSouthEastResize:   return "se-resize";
    case T::kSouthWestResize:   return "sw-resize";
    case T::kWestResize:        return "w-resize";

    // Axis + diagonal resize.
    case T::kEastWestResize:                return "ew-resize";
    case T::kNorthSouthResize:              return "ns-resize";
    case T::kNorthEastSouthWestResize:      return "nesw-resize";
    case T::kNorthWestSouthEastResize:      return "nwse-resize";
    case T::kColumnResize:                  return "col-resize";
    case T::kRowResize:                     return "row-resize";

    // All-scroll (compass "everywhere" pan affordance).
    case T::kMiddlePanning:                 return "all-scroll";
    // TODO(M5-R2-panning-cursors): the eight kFooPanning enumerators
    // (kEastPanning, kNorthPanning, …) and the chromium-only
    // kMiddlePanningVertical/Horizontal don't have CSS-keyword peers.
    // The legacy JS probe (capture/cursor-watcher/main.go's probeJS)
    // never observed them either — they're middle-click pan affordances
    // chromium synthesizes for its own UI surfaces, not styles the
    // renderer ever sets on a hover. Down-convert to "all-scroll"
    // (the closest CSS keyword) when they appear; refine to a richer
    // mapping if and when the M5 acceptance probe spots one in the
    // wild on the cb-chromium pod.
    case T::kEastPanning:                   return "all-scroll";
    case T::kNorthPanning:                  return "all-scroll";
    case T::kNorthEastPanning:              return "all-scroll";
    case T::kNorthWestPanning:              return "all-scroll";
    case T::kSouthPanning:                  return "all-scroll";
    case T::kSouthEastPanning:              return "all-scroll";
    case T::kSouthWestPanning:              return "all-scroll";
    case T::kWestPanning:                   return "all-scroll";

    // kNone is a real CSS keyword but represents "hide the cursor",
    // which the protocol expresses via visible=false + shape="none"
    // applied by the assembler — not via the type-side mapping. When
    // a renderer asks for SetCursor(kNone) without ALSO calling
    // HideCursor, we still emit shape="none" so the client renderer
    // matches the page's intent.
    case T::kNone:          return kShapeNone;

    // kNull is the "use the default" signal — synonym for kPointer
    // in practice. Map to "default" so the renderer treats it the
    // same as kPointer.
    case T::kNull:          return kShapeDefault;

    // kCustom always pairs with custom_image_b64. R2 emits the
    // shape="custom" string but leaves the bytes empty (the renderer
    // falls back per cursor-channel.md "Unknown CSS keywords" /
    // "custom" without image). R5 plumbs the bitmap through.
    case T::kCustom:        return kShapeCustom;

    // Drag-and-drop synthetic cursors chromium produces for its own
    // DnD UI. They have no CSS-keyword peer the renderer would have
    // asked for via `cursor:` style; surface as "default" until a
    // real user-visible regression demands otherwise.
    // TODO(M5-R2-dnd-cursors): revisit once the M5 acceptance probe
    // covers a DnD scenario.
    case T::kDndNone:       return "no-drop";
    case T::kDndMove:       return "move";
    case T::kDndCopy:       return "copy";
    case T::kDndLink:       return "alias";
  }
  // Future-proofing: any enumerator a chromium uprev adds without a
  // matching switch arm falls through to "default" — see the header
  // "Unknown / future" footgun note. The renderer's fallback path
  // handles unknown shape strings gracefully (cursor-channel.md
  // "Unknown CSS keywords MAY appear; the client falls back to
  // default rendering").
  return kShapeDefault;
}

}  // namespace

std::string_view ShapeForType(ui::mojom::CursorType type) {
  return CssKeywordForType(type);
}

EnvelopeAssembler::EnvelopeAssembler() = default;
EnvelopeAssembler::~EnvelopeAssembler() = default;

void EnvelopeAssembler::SetLatchedPosition(int x, int y) {
  // R4 will replace this stub with a real call site driven by the
  // mouse-move observer. R2 leaves both at 0; the protocol allows
  // it (the renderer just paints at origin until the first move).
  latched_x_ = x;
  latched_y_ = y;
}

V1EnvelopeView EnvelopeAssembler::Assemble(ui::mojom::CursorType type,
                                           bool visible,
                                           int64_t now_epoch_ms) {
  V1EnvelopeView view;
  view.v = kProtocolVersion;
  view.type_tag = kEnvelopeTypeCursor;

  // Time stamp resolution: the sentinel keeps base/time/ out of the
  // header. Production callers pass 0 → wall-clock; tests pass an
  // explicit value to pin `t` deterministically.
  view.t_epoch_ms = now_epoch_ms != 0
                        ? now_epoch_ms
                        : base::Time::Now().InMillisecondsSinceUnixEpoch();

  // Strictly increasing per assembler instance. Matches the legacy
  // cursor-watcher contract: first envelope on a fresh assembler has
  // seq=0, each subsequent emit increments.
  view.seq = seq_++;

  view.x = latched_x_;
  view.y = latched_y_;
  view.visible = visible;

  // Emission rule from cursor-channel.md § "Shape names": when the
  // cursor is hidden the wire shape MUST be "none" regardless of the
  // most recent ui::mojom::CursorType. Apply that here so callers
  // don't have to coordinate the type-vs-visibility interaction.
  view.shape = visible ? CssKeywordForType(type) : kShapeNone;

  // TODO(M5-R5-custom-image): when type == kCustom and the upstream
  // CbCursorClient callback grows the SkBitmap + hotspot args, fill
  // view.custom_image_b64 + view.hotspot_{x,y} + view.image_format
  // here. R2 leaves all three optional/empty so the encoder skips
  // the fields, matching the cursor-channel.md "without image"
  // rendering path.

  return view;
}

std::optional<std::string> EnvelopeAssembler::EncodeJson(
    const V1EnvelopeView& view) {
  // base::DictValue mirrors the cursor-channel.md envelope shape
  // exactly. We build `data` first, splice into the outer dict, then
  // JSONWriter::Write.
  base::DictValue data;
  data.Set("x", view.x);
  data.Set("y", view.y);
  data.Set("visible", view.visible);
  data.Set("shape", std::string(view.shape));

  // Optional fields — only emit when populated, matching the
  // cursor-watcher legacy emitter (cursorData's `,omitempty` Go tags
  // in capture/cursor-watcher/main.go:63-66).
  if (view.hotspot_x.has_value() && view.hotspot_y.has_value()) {
    base::DictValue hotspot;
    hotspot.Set("x", *view.hotspot_x);
    hotspot.Set("y", *view.hotspot_y);
    data.Set("hotspot", std::move(hotspot));
  }
  if (!view.custom_image_b64.empty()) {
    data.Set("custom_image_b64", view.custom_image_b64);
    // image_format MUST accompany the bytes (cursor-channel.md table).
    // Default to "png" when callers haven't pinned it — v1 only
    // permits "png" anyway, so this is forced rather than a guess.
    data.Set("image_format",
             view.image_format.empty() ? std::string("png")
                                       : std::string(view.image_format));
  }

  base::DictValue envelope;
  envelope.Set("v", view.v);
  envelope.Set("type", std::string(view.type_tag));
  // int64 → double: base::DictValue::Set has no int64 overload; the
  // (int) overload is 32-bit and would silently truncate `t` past
  // 2038, so we cast to double. The double precision (53-bit mantissa)
  // safely holds epoch-ms through year ~285,000 and a 1-per-cursor-
  // change seq counter through ~30,000 years of continuous emit.
  //
  // Wire-format wall — wall #20: base::JSONWriter::Build for
  // Type::DOUBLE appends ".0" to integral values so the field
  // round-trips as a double rather than an int. That means we emit
  //   "t": 1730290000123.0, "seq": 17.0
  // where the legacy Go cursor-watcher (capture/cursor-watcher/
  // main.go's encoding/json on int64) emits
  //   "t": 1730290000123,   "seq": 17
  // Both are valid JSON numbers and parse-compatible — the JS
  // renderer client/src/cursor.ts uses JSON.parse → JS number which
  // collapses the distinction, and Go consumers reading back into
  // int64 via encoding/json accept the .0 form. If a future receiver
  // demands strict int formatting, swap this path to manual buffer
  // assembly (StringPrintf "%" PRId64 around the two numeric fields)
  // rather than fighting base::JSONWriter's policy.
  envelope.Set("t", static_cast<double>(view.t_epoch_ms));
  envelope.Set("seq", static_cast<double>(view.seq));
  envelope.Set("data", std::move(data));

  std::string json;
  if (!base::JSONWriter::Write(base::Value(std::move(envelope)), &json)) {
    return std::nullopt;
  }
  return json;
}

std::optional<std::string> EnvelopeAssembler::AssembleAndEncode(
    ui::mojom::CursorType type,
    bool visible,
    int64_t now_epoch_ms) {
  return EncodeJson(Assemble(type, visible, now_epoch_ms));
}

}  // namespace cursor
}  // namespace cloud_browser

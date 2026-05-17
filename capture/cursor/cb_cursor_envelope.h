// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Cursor-channel v1 envelope assembler for the cb-chromium native
// cursor egress path.
//
// Module M5 R2 of the ChromelessV2 native-peer migration (CV2-20).
// M5 R1 (CV2-19) registered an embedder aura::client::CursorClient on
// host_->window() so every renderer-driven SetCursor / ShowCursor /
// HideCursor surfaces as a CursorChangeCallback into our process.
// R2 turns each (ui::mojom::CursorType, bool visible) tuple into the
// JSON envelope the existing capture/cursor-watcher/ Go sidecar emits
// today on the wire — same `cursor-channel v1` shape, just produced
// in-process so M5 R6 can hand it to the M3 R5 DataChannel host
// without keeping the JS+CDP poll loop alive.
//
// # Wire contract (frozen — docs/protocols/cursor-channel.md v1)
//
// ```jsonc
// {
//   "v":    1,               // protocol version, integer; must equal 1
//   "type": "cursor",        // discriminant; only "cursor" in v1
//   "t":    1730290000123,   // server wall-clock, epoch ms
//   "seq":  17,              // strictly increasing per channel
//   "data": {
//     "x": 0, "y": 0,        // hotspot coords in source-image space;
//                            // R2 stubs these at 0,0 — see Non-goals
//     "visible": true,
//     "shape": "pointer",    // CSS-keyword string; see kShape* below
//     "hotspot":          { "x": 0, "y": 0 },     // optional
//     "custom_image_b64": "iVBORw0K…",            // optional
//     "image_format":     "png"                   // when image present
//   }
// }
// ```
//
// Source-pins for the contract:
//   * docs/protocols/cursor-channel.md (the authoritative v1 spec —
//     emission policy, shape-name list, versioning rules).
//   * capture/cursor-watcher/main.go:59-92 (the legacy emitter; same
//     wire shape this codec must reproduce so the Phase-1 client
//     renderer client/src/cursor.ts keeps round-tripping unchanged
//     across the streamer-page→native cutover).
//
// # ui::mojom::CursorType → shape string
//
// The CSS-cursor spec and chromium's `ui::mojom::CursorType` enum line
// up *almost* one-for-one, but two well-known footguns matter:
//
//   1. `ui::mojom::CursorType::kPointer` is the **default arrow**.
//      Its CSS equivalent is `"default"`, NOT `"pointer"`.
//      `ui::mojom::CursorType::kHand` is the pointing-hand cursor —
//      THAT is CSS `"pointer"`. The mapping below honors the CSS-side
//      strings (what the client renderer expects) over the enum-side
//      identifiers; mis-mapping these silently swaps every "looks
//      clickable" hover with the default arrow and is exactly the
//      class of regression the round-trip test in
//      cb_cursor_envelope_test.cc pins.
//
//   2. `ui::mojom::CursorType::kNull` and `kCustom` are both legal on
//      the wire but map to different shape strings:
//        kNull   -> "default" (synonym; renderer fallback path)
//        kCustom -> "custom"  (paired with custom_image_b64, see R5)
//      `visible=false` overrides both and forces shape="none" per the
//      cursor-channel.md emission rules.
//
// # Non-goals (other M5 R#s)
//
//   * R3 — DC emission. R2 returns the JSON string; M5 R3 plumbs it
//     through CbCursorClient::change_callback_ onto the M3 R5
//     DataChannel host.
//   * R4 — pointer coordinates. ui::Cursor's SetCursor signature
//     carries no x/y, so R2 always emits `x:0, y:0`. M5 R4 wires a
//     separate aura::WindowEventDispatcher pre-target observer for
//     `ui::ET_MOUSE_MOVED` and updates the latched coordinates the
//     envelope assembler reads.
//   * R5 — custom-image bytes. R2 emits shape="custom" with no
//     custom_image_b64 when the renderer asked for a `cursor: url(...)`
//     custom cursor (legal per the wire contract — the renderer
//     falls back to "default" rendering when the bytes are absent;
//     see cursor-channel.md "Unknown CSS keywords"). M5 R5 plumbs the
//     bitmap from ui::Cursor::custom_bitmap() through a SkBitmap →
//     PNG → base64 path with the 64 KiB size cap the protocol pins.
//   * R6 — DataChannel binding (the consumer side of R3).
//
// # Threading
//
// All entry points are pure functions of their arguments — no shared
// state beyond the EnvelopeAssembler's seq counter and the optional
// latched coordinates (R4). The CbCursorClient callback fires on the
// UI thread (aura::client::CursorClient contract); the assembler is
// expected to be owned and called from the same sequence. The
// EnvelopeAssembler is NOT internally synchronized — if a future
// caller wants to drive Assemble() off-sequence (e.g. from the M3 R2
// signaling task runner), wrap it in a SequenceChecker + bound
// callback, don't add a lock here.
//
// # Wall #-style notes for the reviewer
//
//   * Pure codec (mirrors M3 R1's :cb_wire_envelope shape) — only
//     chromium deps are //base for base::JSONWriter / base::Value
//     and //ui/base/cursor/mojom for the enum.
//   * No #include of "ui/base/cursor/cursor.h" — we take the enum
//     directly so this TU stays compilable in environments that don't
//     have the full ui::Cursor pulled in (unit tests, codegen probes).

#ifndef CAPTURE_CURSOR_CB_CURSOR_ENVELOPE_H_
#define CAPTURE_CURSOR_CB_CURSOR_ENVELOPE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ui/base/cursor/mojom/cursor_type.mojom-shared.h"

namespace cloud_browser {
namespace cursor {

// Canonical wire string for a given ui::mojom::CursorType, modulo the
// visibility rule: when the cursor is hidden the envelope's shape MUST
// be "none" regardless of the most-recent type (see cursor-channel.md
// emission policy + Shape names list). ShapeForType returns the type's
// CSS-keyword string with visibility forced to true; the assembler
// applies the visible=false override before encoding.
//
// Unknown / future ui::mojom::CursorType values fall through to
// "default" — receivers fall back to default rendering for any value
// they don't recognise (cursor-channel.md "Unknown CSS keywords") so
// down-converting here keeps the wire safe even if libcc adds a new
// enumerator in a chromium uprev.
std::string_view ShapeForType(ui::mojom::CursorType type);

// The decoded view of a v1 envelope. Pure data, no methods; assembled
// by EnvelopeAssembler::Assemble and consumed by M5 R3 (which encodes
// + writes onto the DC) or by tests (which round-trip-compare against
// the legacy emitter's output).
struct V1EnvelopeView {
  // Wire-shape fields (see header comment).
  int v = 1;
  std::string_view type_tag = "cursor";  // only "cursor" in v1
  int64_t t_epoch_ms = 0;
  int64_t seq = 0;

  // data.* — flattened into the view; the encoder writes them into a
  // nested object on the wire.
  int x = 0;
  int y = 0;
  bool visible = true;
  std::string_view shape = "default";

  // R5 will populate these for kCustom. R2 emits shape="custom" with
  // custom_image_b64 empty (renderer falls back to default rendering
  // per cursor-channel.md; documented + tested).
  std::optional<int> hotspot_x;
  std::optional<int> hotspot_y;
  std::string custom_image_b64;  // empty unless R5 has run
  std::string_view image_format;  // "png" when bytes present
};

// Stateful assembler — owns the monotonic `seq` counter and the
// latched pointer position (the latter wired in R4; until then both
// stay at 0 and the wire emits "x":0,"y":0 unconditionally).
//
// The cursor-channel.md emission policy is "emit on change" + suppress
// identical-state polls. R2 deliberately does NOT enforce that here —
// the upstream CbCursorClient callback already fires only on a
// SetCursor / ShowCursor / HideCursor (no rAF poll, no idle re-emit),
// so every Assemble call corresponds to a real change. If a future
// caller starts driving Assemble from a poll loop, the dedupe layer
// belongs in that caller (mirroring capture/cursor-watcher/main.go's
// equalState + maybeEmit pattern), not here.
class EnvelopeAssembler {
 public:
  EnvelopeAssembler();

  EnvelopeAssembler(const EnvelopeAssembler&) = delete;
  EnvelopeAssembler& operator=(const EnvelopeAssembler&) = delete;

  ~EnvelopeAssembler();

  // Build a V1EnvelopeView from a CbCursorClient callback's
  // (type, visible) pair. Advances the seq counter and stamps `t` from
  // the injected wall-clock (default: base::Time::Now()).
  //
  // `now_epoch_ms` is callable-injected so tests can pin `t` to a
  // deterministic value; production passes 0 and the assembler reads
  // base::Time::Now() internally. We use a sentinel rather than a
  // base::Clock* to keep the header free of //base/time/ — see the
  // .cc for the resolution detail.
  V1EnvelopeView Assemble(ui::mojom::CursorType type,
                          bool visible,
                          int64_t now_epoch_ms = 0);

  // Encode a V1EnvelopeView to a UTF-8 JSON string suitable for a
  // direct DC write. Returns nullopt only on JSON-writer failure
  // (shouldn't happen for well-formed input — base::JSONWriter only
  // fails on Value subtypes we never emit, but we surface the result
  // honestly so callers don't double-encode an error case).
  static std::optional<std::string> EncodeJson(const V1EnvelopeView& view);

  // Convenience: Assemble + EncodeJson in one call. The hot path for
  // M5 R3's DC emitter — keeps the seq counter advancing in lockstep
  // with the wire bytes.
  std::optional<std::string> AssembleAndEncode(ui::mojom::CursorType type,
                                               bool visible,
                                               int64_t now_epoch_ms = 0);

  // R4 entry point — latches the most recent pointer position from
  // the (future) mouse-move observer. Subsequent Assemble() calls
  // emit these as `x`/`y`. Stub for R2 (no caller yet).
  // TODO(M5-R4-pointer-position): wire from cb_mouse_move_observer.
  void SetLatchedPosition(int x, int y);

  // Test accessor — exposes the seq counter so the round-trip fixture
  // can assert strict monotonicity across a sequence of Assemble calls.
  int64_t seq_for_testing() const { return seq_; }

 private:
  // Strictly increasing per assembler instance, starts at 0 like the
  // cursor-watcher's legacy emitter (see watcher::maybeEmit in
  // capture/cursor-watcher/main.go).
  int64_t seq_ = 0;

  // Latched pointer coords — see SetLatchedPosition.
  int latched_x_ = 0;
  int latched_y_ = 0;
};

}  // namespace cursor
}  // namespace cloud_browser

#endif  // CAPTURE_CURSOR_CB_CURSOR_ENVELOPE_H_

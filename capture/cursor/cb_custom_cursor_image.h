// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Custom CSS cursor image extractor for the cb-chromium native cursor
// egress path.
//
// Module M5 R5 of the ChromelessV2 native-peer migration (CV2-23).
// Fills the cursor-channel v1 envelope's `custom_image_b64` /
// `hotspot` / `image_format` fields when the renderer asks for a
// CSS `cursor: url(...)` custom cursor (ui::Cursor::type() == kCustom).
//
// # Why native beats the JS path
//
// The legacy capture/cursor-watcher/main.go JS probe could only
// forward the URL string from `getComputedStyle(...).cursor` — the
// Go sidecar never fetched the bytes, and the cursor-channel v1
// envelope's `custom_image_b64` field has stayed a documented
// skeleton gap since the protocol was frozen. The browser process
// has the decoded SkBitmap right there on every renderer-side
// SetCursor (RWHV_Aura already painted it), so the native path
// reads `ui::Cursor::custom_bitmap()` for free.
//
// # Wire contract pins (docs/protocols/cursor-channel.md v1)
//
// ```jsonc
// "data": {
//   ...
//   "shape": "custom",                 // M5 R2 sets this for kCustom
//   "hotspot":          { "x": 8, "y": 8 },   // R5 fills
//   "custom_image_b64": "iVBORw0K…",          // R5 fills (≤ 64 KiB)
//   "image_format":     "png"                 // R5 fills (always)
// }
// ```
//
//   * `custom_image_b64` MUST be ≤ 64 KiB of BASE64-ENCODED bytes —
//     the cap is on the wire-readable string, not the pre-encode PNG
//     buffer. cursor-channel.md frames this as "v1 64 KiB encoded
//     cap" so the channel stays well under DataChannel SCTP fragment
//     limits when paired with the rest of the envelope's overhead.
//     Over-cap cursors drop bytes + fall back per contract (the
//     receiver paints the default arrow per cursor-channel.md
//     "custom without image").
//   * `image_format` MUST accompany the bytes; v1 only permits
//     "png" today. We hard-code "png" rather than guess.
//   * `hotspot` is in source-image **pixel** coordinates, NOT DIPs.
//     ui::Cursor::custom_hotspot() returns gfx::Point in the same
//     bitmap-pixel space, so this is a direct hand-off.
//
// # Non-goals (per CV2-23 spec)
//
//   * Animated cursors — chromium's ui::Cursor::custom_bitmap()
//     surfaces a single SkBitmap (the first frame); the
//     image-decoder side already collapses animated PNG / GIF / WebP
//     down to one frame before SetCursor lands. First-frame-only is
//     therefore implicit, not a code branch; documented for the
//     reader.
//   * `image_scale_factor` emission — cursor-channel.md v1 has no
//     wire field for it. We surface it on the extraction struct for
//     metrics + a future v2 bump, but never write it onto the
//     envelope.
//
// # Threading
//
// Pure functions of their arguments. Caller is responsible for
// invoking on the same sequence as M5 R1's CursorChangeCallback
// (the UI thread per aura::client::CursorClient contract). No
// internal synchronization, no I/O, no allocations beyond
// std::string growth.
//
// # Wall-style notes for the reviewer
//
//   * gfx::PNGCodec::EncodeBGRASkBitmap is the right encoder — it
//     pulls the SkBitmap's pixmap directly without an unpremultiply
//     round-trip for the common N32 layout. The SkPngEncoder path
//     under it honors the bitmap's alpha-type so transparent custom
//     cursors (the common case for circular pointer dots) keep
//     their alpha channel through the encode.
//   * base::Base64Encode takes a base::span<const uint8_t> in modern
//     chromium (the std::string_view overload is legacy and binds
//     to UTF-8 strings, which a PNG byte stream is not). Use the
//     span overload.
//   * The 64 KiB cap is applied AFTER base64 expansion, because
//     that's what the wire pays for. A 48 KiB PNG that base64s to
//     65 KiB busts the cap and drops; a 49 KiB PNG that base64s to
//     65,532 bytes is legal. We deliberately don't pre-check the
//     raw PNG length — the encoded-length check covers both cases
//     and there's no point round-tripping the math.
//   * Integration: M5 R1's CursorChangeCallback signature is
//     `(ui::mojom::CursorType, bool)` and drops the ui::Cursor
//     entirely. Plumbing R5 through requires amending that
//     signature to carry the full ui::Cursor (or at minimum the
//     SkBitmap + hotspot + scale tuple). See the commit message
//     for the one-line cb_cursor_client.cc edit + the matching
//     header bump.

#ifndef CAPTURE_CURSOR_CB_CUSTOM_CURSOR_IMAGE_H_
#define CAPTURE_CURSOR_CB_CUSTOM_CURSOR_IMAGE_H_

#include <cstddef>
#include <string>

#include "ui/base/cursor/cursor.h"

namespace cloud_browser {
namespace cursor {

// Forward decl — defined in cb_cursor_envelope.h (M5 R2). We don't
// pull the envelope header here because the extractor is also
// useful from non-envelope call sites (metrics probes, the M5
// acceptance harness's pod-log scraper).
struct V1EnvelopeView;

// The cursor-channel v1 cap on the wire-readable custom_image_b64
// string. Over-cap cursors drop the bytes and the receiver paints
// the default arrow (cursor-channel.md "custom without image").
//
// 64 * 1024 — kept named so a v2 bump touches a single edit site.
inline constexpr size_t kV1CustomImageB64MaxBytes = 64u * 1024u;

// Outcome of an extraction attempt. `ok` discriminates the union of
// happy-path + skip cases; `skip` carries the reason when ok=false
// so the LOG(INFO) trampoline + the M5 R6 acceptance probe can
// report which contract clause fired.
struct CustomImageExtraction {
  // Reason an extraction was dropped. All non-kNone values leave the
  // wire fields empty so the receiver falls back per contract.
  enum class Skip {
    kNone,           // ok=true; bytes + hotspot present
    kNotCustom,      // cursor.type() != kCustom; pass-through
    kBitmapEmpty,    // custom_bitmap().isNull() || 0x0
    kEncodeFailed,   // gfx::PNGCodec::EncodeBGRASkBitmap returned false
    kOverCap,        // base64 length > kV1CustomImageB64MaxBytes
  };

  // True iff custom_image_b64 is populated + safe to emit.
  bool ok = false;

  // Reason for ok=false; kNone iff ok=true.
  Skip skip = Skip::kNotCustom;

  // Base64-encoded PNG bytes. Empty unless ok=true.
  std::string custom_image_b64;

  // Hotspot in source-image PIXEL coords (not DIPs). v1 envelope
  // takes integer hotspot; ui::Cursor::custom_hotspot() returns
  // gfx::Point so no rounding needed. Both default to 0 (the CSS
  // `cursor: url(...)` spec default when no hotspot keywords are
  // given).
  int hotspot_x = 0;
  int hotspot_y = 0;

  // Always "png" when ok=true (v1 permits only PNG). Empty
  // otherwise — keeps the encoder's "only emit when populated"
  // contract from M5 R2 working.
  std::string image_format;

  // The cursor's DIP-to-pixel ratio as reported by chromium. Not
  // emitted on the wire (cursor-channel.md v1 has no scale field);
  // surfaced here so a future v2 bump can plumb it through without
  // re-touching the extractor.
  //
  // Modern chromium (M147+) calls this ui::Cursor::image_scale_factor.
  // The legacy WebCursor::ImageScaleFactor accessor was renamed when
  // ui::Cursor absorbed WebCursor in 2021; both refer to the same
  // float. Default 1.0f for non-HiDPI cursors.
  float image_scale_factor = 1.0f;
};

// Pure function: examine `cursor` and produce an extraction.
//
// Behavior:
//   - cursor.type() != kCustom            -> ok=false, skip=kNotCustom
//   - custom_bitmap().isNull() || 0-area  -> ok=false, skip=kBitmapEmpty
//   - PNGCodec failure (OOM, malformed)   -> ok=false, skip=kEncodeFailed
//   - base64 length > 64 KiB              -> ok=false, skip=kOverCap
//   - otherwise                            -> ok=true, fields populated
//
// Safe on any ui::Cursor; callers don't need to gate on type.
//
// TODO(M5-R5-metrics): a UMA histogram of `Skip` would let us spot
// which clause is firing in the wild (kOverCap vs kBitmapEmpty
// would distinguish "the page asked for a 200x200 cursor we can't
// fit" from "we hit the kCustom branch before the bitmap landed").
CustomImageExtraction ExtractCustomImage(const ui::Cursor& cursor);

// Plumbs an extraction result onto a M5 R2 V1EnvelopeView. No-op
// (returns false) for ok=false — the receiver falls back per
// contract. Returns true iff any wire field was written.
//
// This is the seam M5 R2's `// TODO(M5-R5-custom-image)` comment
// points at: callers that already have a V1EnvelopeView in scope
// (the cb_cursor_client.cc HandleCursorSet path, post-callback
// amendment) chain `ApplyExtractionToEnvelope(ExtractCustomImage(c),
// &view)` immediately after EnvelopeAssembler::Assemble().
bool ApplyExtractionToEnvelope(const CustomImageExtraction& extraction,
                               V1EnvelopeView* view);

}  // namespace cursor
}  // namespace cloud_browser

#endif  // CAPTURE_CURSOR_CB_CUSTOM_CURSOR_IMAGE_H_

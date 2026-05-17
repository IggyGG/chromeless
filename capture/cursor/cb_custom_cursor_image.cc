// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cb_custom_cursor_image.cc — see cb_custom_cursor_image.h.

#include "capture/cursor/cb_custom_cursor_image.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "capture/cursor/cb_cursor_envelope.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/cursor/mojom/cursor_type.mojom-shared.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/point.h"

namespace cloud_browser {
namespace cursor {

namespace {

// Wire-side image format string. v1 permits only "png" — see
// cursor-channel.md "Image formats" table. Kept in one place so a
// future v2 bump (e.g. WebP) touches a single edit site.
constexpr char kImageFormatPng[] = "png";

// PNG encoder discards-transparency flag. Custom cursors are routinely
// transparent (circular dots, donut rings, hand-drawn pointers with
// anti-aliased edges) so we MUST keep alpha. Named for the reader.
constexpr bool kKeepAlphaChannel = false;

}  // namespace

CustomImageExtraction ExtractCustomImage(const ui::Cursor& cursor) {
  CustomImageExtraction out;

  // ---- Branch 1: non-kCustom cursors are a pass-through ----
  //
  // The extractor is safe to call from the unconditional callback
  // path; non-custom cursors just produce an ok=false result the
  // caller drops on the floor.
  if (cursor.type() != ui::mojom::CursorType::kCustom) {
    out.ok = false;
    out.skip = CustomImageExtraction::Skip::kNotCustom;
    return out;
  }

  // ---- Branch 2: read the bitmap + hotspot + scale ----
  //
  // ui::Cursor::custom_bitmap() returns const SkBitmap& by reference
  // (the cursor owns the bitmap). custom_hotspot() returns gfx::Point
  // by value in bitmap-pixel coords. image_scale_factor() returns
  // float (renamed from WebCursor::ImageScaleFactor in the 2021
  // WebCursor → ui::Cursor consolidation; both refer to the same
  // DIP-to-pixel ratio).
  const SkBitmap& bitmap = cursor.custom_bitmap();
  const gfx::Point hotspot = cursor.custom_hotspot();
  out.image_scale_factor = cursor.image_scale_factor();

  // Bitmap sanity. isNull() catches the "kCustom announced but no
  // pixels set yet" race (chromium will sometimes flip type to
  // kCustom in WebCursor::SetCursor before the bitmap arrives from
  // the renderer's WebCursorInfo IPC); empty()/0-area catches the
  // pathological 0x0 bitmap a malicious renderer could craft.
  //
  // Hotspot is intentionally NOT validated here — gfx::Point can
  // legally hold negative or out-of-bounds values (CSS allows
  // `cursor: url(...) -5 -5,auto`; the renderer clamps before
  // painting). We pass through what we observe; the receiver
  // handles edge cases per the renderer's own painting policy.
  if (bitmap.isNull() || bitmap.empty() ||
      bitmap.width() == 0 || bitmap.height() == 0) {
    out.ok = false;
    out.skip = CustomImageExtraction::Skip::kBitmapEmpty;
    return out;
  }

  // ---- Branch 3: PNG-encode ----
  //
  // gfx::PNGCodec::EncodeBGRASkBitmap reads the SkBitmap's pixmap
  // directly (no unpremultiply round-trip for the common N32/BGRA
  // layout chromium uses for cursor bitmaps). Returns false only on
  // OOM or a malformed bitmap that slipped the isNull() check above
  // — both are reasons to drop the wire fields rather than emit
  // garbage.
  //
  // The encoded buffer is owned locally; we base64 it onto the heap
  // string and let the vector free at scope exit. Cursor bitmaps
  // top out around 128x128 RGBA = 64 KiB raw → ~22 KiB PNG worst
  // case, so the allocation is bounded.
  std::vector<uint8_t> png_bytes;
  const bool encoded = gfx::PNGCodec::EncodeBGRASkBitmap(
      bitmap, kKeepAlphaChannel, &png_bytes);
  if (!encoded || png_bytes.empty()) {
    out.ok = false;
    out.skip = CustomImageExtraction::Skip::kEncodeFailed;
    return out;
  }

  // ---- Branch 4: base64 + cap check ----
  //
  // base::Base64Encode takes base::span<const uint8_t> in modern
  // chromium and returns std::string. The legacy std::string_view
  // overload is for UTF-8 text inputs and would compile-warn here
  // on a raw byte stream.
  //
  // The cap is on the BASE64-ENCODED length because that's what the
  // wire pays for (the JSON envelope writes the base64 string into
  // a quoted JSON string field; the unencoded PNG never crosses the
  // DC). A 48 KiB PNG that base64s to 65 KiB busts the cap. We
  // check post-encode and drop the bytes if over — the receiver
  // falls back per cursor-channel.md "custom without image" and
  // the renderer paints the default arrow.
  //
  // base64 expansion factor: 4 output bytes per 3 input bytes,
  // padded to a 4-byte boundary. A 49,151-byte PNG base64s to
  // 65,536 bytes exactly (the cap boundary).
  std::string b64 = base::Base64Encode(base::span<const uint8_t>(png_bytes));
  if (b64.size() > kV1CustomImageB64MaxBytes) {
    LOG(INFO) << "ExtractCustomImage: dropping over-cap custom cursor "
              << bitmap.width() << "x" << bitmap.height()
              << " png=" << png_bytes.size()
              << " b64=" << b64.size()
              << " cap=" << kV1CustomImageB64MaxBytes;
    out.ok = false;
    out.skip = CustomImageExtraction::Skip::kOverCap;
    // Leave custom_image_b64 empty so the encoder skips the field.
    return out;
  }

  // ---- Happy path: populate + return ----
  out.ok = true;
  out.skip = CustomImageExtraction::Skip::kNone;
  out.custom_image_b64 = std::move(b64);
  out.hotspot_x = hotspot.x();
  out.hotspot_y = hotspot.y();
  out.image_format = kImageFormatPng;
  return out;
}

bool ApplyExtractionToEnvelope(const CustomImageExtraction& extraction,
                               V1EnvelopeView* view) {
  if (!view || !extraction.ok) {
    // No view or extraction skipped — leave the view's optional
    // fields default-constructed. The encoder's "only emit when
    // populated" branch in EnvelopeAssembler::EncodeJson then
    // omits hotspot / custom_image_b64 / image_format from the
    // wire, which is the cursor-channel.md fallback path.
    return false;
  }

  view->custom_image_b64 = extraction.custom_image_b64;
  view->hotspot_x = extraction.hotspot_x;
  view->hotspot_y = extraction.hotspot_y;
  // V1EnvelopeView::image_format is a string_view; the extraction's
  // std::string lifetime must outlive the encoder call. The encoder
  // copies the value into base::Value::Dict on EncodeJson, so a
  // string_view bound to the extraction's std::string is safe for
  // any caller that holds the CustomImageExtraction across the
  // immediately-following EncodeJson() call. Tests pin this.
  view->image_format = extraction.image_format;
  return true;
}

}  // namespace cursor
}  // namespace cloud_browser

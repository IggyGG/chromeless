/*
 * encode-timestamp.js — render a timestamp into a small QR code on a canvas.
 *
 * Used by harness/latency/index.html to embed a machine-readable
 * timestamp inside every flash, so that a webcam pointed at the client
 * screen can OCR the timestamp out of each frame and the
 * reconciliation script (harness/latency/reconcile.py — T11) can
 * compute glass-to-glass latency without needing any side channel.
 *
 * The encoded payload is a compact pipe-delimited string rather than
 * JSON, to keep the QR small and Version 1 (21x21 modules) feasible
 * in the common case:
 *
 *     v1|<runId>|<frameId>|<ON|OFF>|<epochMs>
 *
 * runIds longer than ~10 chars push the QR to V2/V3, which is fine —
 * the receiver autodetects size.
 *
 * QR generation is delegated to the vendored `qrcode-generator`
 * library (Kazuhiko Arase, MIT licensed) loaded via vendor-qrcode.js.
 */

(function (global) {
  'use strict';

  /**
   * Build the canonical payload string for a timestamp record.
   * Keep this in sync with reconcile.py's parser.
   */
  function encodeTimestamp(rec) {
    // Truncate runId aggressively — it's only there to disambiguate
    // overlapping runs on the same physical screen recording.
    const runId = String(rec.runId || '').slice(0, 16);
    const onOff = rec.on ? 'ON' : 'OFF';
    return `v1|${runId}|${rec.frameId}|${onOff}|${rec.epochMs}`;
  }

  /**
   * Render the encoded payload as a QR code into the given <canvas>,
   * using the smallest QR version that fits with error correction
   * level M (15% recovery — enough to survive moderate webcam blur).
   *
   * @param {HTMLCanvasElement} canvas
   * @param {string} payload
   * @param {number} moduleSize  Pixels per QR module (default 12).
   */
  function drawQrInto(canvas, payload, moduleSize) {
    moduleSize = moduleSize || 12;

    // typeNumber=0 means "auto-pick smallest version that fits".
    // Error correction level 'M' is a good blur/speed compromise.
    const qr = qrcode(0, 'M');
    qr.addData(payload);
    qr.make();

    const count = qr.getModuleCount();
    const px = count * moduleSize;
    canvas.width = px;
    canvas.height = px;

    const ctx = canvas.getContext('2d');
    // Clear to white (the QR "quiet zone" bg).
    ctx.fillStyle = '#fff';
    ctx.fillRect(0, 0, px, px);

    ctx.fillStyle = '#000';
    for (let r = 0; r < count; r++) {
      for (let c = 0; c < count; c++) {
        if (qr.isDark(r, c)) {
          ctx.fillRect(c * moduleSize, r * moduleSize, moduleSize, moduleSize);
        }
      }
    }
  }

  // Expose to the page.
  global.encodeTimestamp = encodeTimestamp;
  global.drawQrInto = drawQrInto;
})(typeof window !== 'undefined' ? window : globalThis);

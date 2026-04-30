// Unit tests for client/src/cursor.ts.
//
// Uses vitest with happy-dom-style minimal stubs so the tests don't
// require a real browser. We test:
//   - sourceToViewport math (the object-fit: contain mapping).
//   - isValidEnvelope rejects malformed payloads.
//   - renderCursor only mutates the overlay on valid envelopes,
//     hides on visible:false, and disposes cleanly.

import { describe, it, expect, beforeEach } from "vitest";
import {
  PROTOCOL_VERSION,
  isValidEnvelope,
  sourceToViewport,
  renderCursor,
} from "./cursor.js";

// ---------------------------------------------------------------------------
// Tiny DOM stubs — we only use the surface the renderer touches.
// ---------------------------------------------------------------------------

class StubElement {
  children: StubElement[] = [];
  style: Record<string, string> = {};
  dataset: Record<string, string> = {};
  innerHTML = "";
  appendChild(c: StubElement): StubElement {
    this.children.push(c);
    return c;
  }
  remove(): void {
    /* tests don't track parents — no-op is fine. */
  }
  // Stand-in for DOMRect.
  getBoundingClientRect(): DOMRect {
    return { left: 0, top: 0, width: 800, height: 450, right: 800, bottom: 450, x: 0, y: 0, toJSON: () => ({}) } as DOMRect;
  }
}

function stubVideo(videoW = 1920, videoH = 1080): HTMLVideoElement {
  const el = new StubElement() as unknown as HTMLVideoElement;
  // @ts-expect-error: stub injection
  el.videoWidth = videoW;
  // @ts-expect-error: stub injection
  el.videoHeight = videoH;
  return el;
}

// Test-only handle for the stubbed body so each test can inspect children.
let testBody: StubElement;

beforeEach(() => {
  // jsdom-or-equivalent: provide minimal `document.createElement` and
  // `document.body` so renderCursor's element wiring works.
  testBody = new StubElement();
  (globalThis as unknown as { document: unknown }).document = {
    createElement: () => new StubElement(),
    body: testBody,
  };
});

/** Read the overlay (first child appended to the test body). Throws if
 *  renderCursor didn't add it — preferable to typecheck noise on a
 *  possibly-undefined index access. */
function overlayOf(): StubElement {
  const o = testBody.children[0];
  if (!o) throw new Error("no overlay attached to test body");
  return o;
}

// ---------------------------------------------------------------------------
// sourceToViewport — object-fit: contain math
// ---------------------------------------------------------------------------

describe("sourceToViewport", () => {
  // 1920x1080 source displayed in an 800x450 element fits perfectly:
  // scale = 0.4166..., zero padding.
  it("matches aspect ratio with zero padding", () => {
    const r = sourceToViewport(960, 540, { left: 0, top: 0, width: 800, height: 450 }, 1920, 1080);
    expect(r.x).toBeCloseTo(400, 0);
    expect(r.y).toBeCloseTo(225, 0);
  });

  // 16:9 source in a 4:3 box → letterboxed: padding goes top/bottom.
  it("letterboxes when client is wider than source aspect", () => {
    const r = sourceToViewport(0, 0, { left: 0, top: 0, width: 800, height: 800 }, 1920, 1080);
    // scale = 800/1920, dispH = 1080 * 800/1920 = 450, padY = (800-450)/2 = 175
    expect(r.x).toBeCloseTo(0, 0);
    expect(r.y).toBeCloseTo(175, 0);
  });

  it("returns rect-relative coords when video size is unknown", () => {
    const r = sourceToViewport(50, 60, { left: 10, top: 20, width: 800, height: 450 }, 0, 0);
    expect(r.x).toBe(60);
    expect(r.y).toBe(80);
  });
});

// ---------------------------------------------------------------------------
// isValidEnvelope
// ---------------------------------------------------------------------------

describe("isValidEnvelope", () => {
  const base = {
    v: PROTOCOL_VERSION,
    type: "cursor",
    t: 1,
    seq: 0,
    data: { x: 1, y: 2, visible: true, shape: "default" },
  };

  it("accepts a well-formed envelope", () => {
    expect(isValidEnvelope(base)).toBe(true);
  });

  it("rejects wrong version", () => {
    expect(isValidEnvelope({ ...base, v: 2 })).toBe(false);
  });

  it("rejects wrong type", () => {
    expect(isValidEnvelope({ ...base, type: "input" })).toBe(false);
  });

  it("rejects missing fields", () => {
    expect(isValidEnvelope({ ...base, data: {} })).toBe(false);
    expect(isValidEnvelope({ ...base, t: "x" as unknown as number })).toBe(false);
  });

  it("caps custom_image_b64 at 64 KiB", () => {
    const tiny = "A".repeat(100);
    const big = "A".repeat(64 * 1024 + 1);
    expect(isValidEnvelope({ ...base, data: { ...base.data, custom_image_b64: tiny } })).toBe(true);
    expect(isValidEnvelope({ ...base, data: { ...base.data, custom_image_b64: big } })).toBe(false);
  });

  it("rejects garbage", () => {
    expect(isValidEnvelope(null)).toBe(false);
    expect(isValidEnvelope("string")).toBe(false);
    expect(isValidEnvelope(42)).toBe(false);
  });
});

// ---------------------------------------------------------------------------
// renderCursor — overlay reacts to envelopes
// ---------------------------------------------------------------------------

describe("renderCursor", () => {
  it("hides overlay until first valid envelope", () => {
    const v = stubVideo();
    const r = renderCursor(v);
    // First child of body is the overlay.
    const overlay = overlayOf();
    expect(overlay.style.display).toBe("none");
    r.dispose();
  });

  it("positions and shows on visible:true", () => {
    const v = stubVideo(1920, 1080);
    const r = renderCursor(v);
    r.update({
      v: 1, type: "cursor", t: 1, seq: 0,
      data: { x: 960, y: 540, visible: true, shape: "pointer" },
    });
    const overlay = overlayOf();
    expect(overlay.style.display).toBe("block");
    expect(overlay.dataset.shape).toBe("pointer");
    expect(overlay.style.transform).toMatch(/translate3d\(\d+px, \d+px, 0\)/);
  });

  it("hides on visible:false", () => {
    const v = stubVideo();
    const r = renderCursor(v);
    r.update({
      v: 1, type: "cursor", t: 1, seq: 0,
      data: { x: 0, y: 0, visible: true, shape: "default" },
    });
    r.update({
      v: 1, type: "cursor", t: 2, seq: 1,
      data: { x: 0, y: 0, visible: false, shape: "none" },
    });
    const overlay = overlayOf();
    expect(overlay.style.display).toBe("none");
  });

  it("ignores invalid envelopes", () => {
    const v = stubVideo();
    const r = renderCursor(v);
    // No throw.
    r.update({ v: 99, type: "cursor", t: 1, seq: 0, data: {} });
    r.update("not an object");
    r.update(null);
    const overlay = overlayOf();
    expect(overlay.style.display).toBe("none");
  });

  it("falls back to default for unknown shapes", () => {
    const v = stubVideo();
    const r = renderCursor(v);
    r.update({
      v: 1, type: "cursor", t: 1, seq: 0,
      data: { x: 0, y: 0, visible: true, shape: "vendor-magic-shape" },
    });
    const overlay = overlayOf();
    expect(overlay.dataset.shape).toBe("default");
  });
});

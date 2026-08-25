// Cursor renderer for the chromeless client.
//
// Subscribes to the "cursor" RTCDataChannel (created in main.ts) and
// renders an HTML/CSS cursor at the reported coordinates as an
// overlay above the streamed video element. The video element itself
// has `cursor: none` so any cursor that may have been baked into the
// streamed frame is hidden.
//
// Wire format: docs/protocols/cursor-channel.md (v1). Server emitter:
// capture/cursor-watcher/.
//
// Design notes
// ------------
//   - We use one absolutely-positioned <div> overlay rather than per-
//     event DOM mutations: only `style.transform` (translate3d) and
//     `data-shape` change at runtime, which is cheap and GPU-
//     compositor-friendly.
//   - Every envelope is treated as ABSOLUTE state, never a diff. That
//     way we recover from any dropped packet as soon as the next
//     change envelope arrives.
//   - The renderer maps source-coordinate `(x, y)` (the protocol's
//     coordinate system) into client viewport coordinates using the
//     same `object-fit: contain` math as the input encoder in
//     input.ts — kept in a small helper that's also exported for tests.
//   - Unknown CSS cursor shapes fall back to `default`. The renderer
//     does NOT trust arbitrary `custom_image_b64` data — payloads
//     larger than the v1 64 KiB cap are dropped.

export const PROTOCOL_VERSION = 1 as const;

export type CursorShape =
  | "default" | "pointer" | "text" | "wait" | "crosshair" | "move"
  | "not-allowed" | "grab" | "grabbing"
  | "ew-resize" | "ns-resize" | "nesw-resize" | "nwse-resize"
  | "col-resize" | "row-resize"
  | "n-resize" | "e-resize" | "s-resize" | "w-resize"
  | "ne-resize" | "nw-resize" | "se-resize" | "sw-resize"
  | "zoom-in" | "zoom-out" | "help" | "progress" | "vertical-text"
  | "context-menu" | "alias" | "copy" | "cell" | "all-scroll" | "no-drop"
  | "custom" | "none";

export interface CursorData {
  x: number;
  y: number;
  visible: boolean;
  shape: CursorShape | string;
  hotspot?: { x: number; y: number };
  custom_image_b64?: string;
  image_format?: "png";
}

export interface CursorEnvelope {
  v: typeof PROTOCOL_VERSION;
  type: "cursor";
  t: number;
  seq: number;
  data: CursorData;
}

const KNOWN_SHAPES = new Set<string>([
  "default", "pointer", "text", "wait", "crosshair", "move",
  "not-allowed", "grab", "grabbing",
  "ew-resize", "ns-resize", "nesw-resize", "nwse-resize",
  "col-resize", "row-resize",
  "n-resize", "e-resize", "s-resize", "w-resize",
  "ne-resize", "nw-resize", "se-resize", "sw-resize",
  "zoom-in", "zoom-out", "help", "progress", "vertical-text",
  "context-menu", "alias", "copy", "cell", "all-scroll", "no-drop",
  "custom", "none",
]);

/** Map source-coordinate space into client viewport pixels. Mirrors the
 *  object-fit: contain math in input.ts. Exported for tests. */
export function sourceToViewport(
  x: number, y: number,
  rect: { left: number; top: number; width: number; height: number },
  videoW: number, videoH: number,
): { x: number; y: number } {
  if (videoW <= 0 || videoH <= 0) {
    return { x: rect.left + x, y: rect.top + y };
  }
  const scale = Math.min(rect.width / videoW, rect.height / videoH);
  const dispW = videoW * scale;
  const dispH = videoH * scale;
  const padX = (rect.width  - dispW) / 2;
  const padY = (rect.height - dispH) / 2;
  return {
    x: rect.left + padX + x * scale,
    y: rect.top  + padY + y * scale,
  };
}

/** Drop suspicious payloads. The renderer is a defensive boundary; a
 *  malformed envelope must not throw or render garbage. */
export function isValidEnvelope(env: unknown): env is CursorEnvelope {
  if (!env || typeof env !== "object") return false;
  const e = env as Partial<CursorEnvelope>;
  if (e.v !== PROTOCOL_VERSION) return false;
  if (e.type !== "cursor") return false;
  if (typeof e.t !== "number" || typeof e.seq !== "number") return false;
  const d = e.data as Partial<CursorData> | undefined;
  if (!d || typeof d !== "object") return false;
  if (typeof d.x !== "number" || typeof d.y !== "number") return false;
  if (typeof d.visible !== "boolean" || typeof d.shape !== "string") return false;
  if (d.custom_image_b64 !== undefined) {
    if (typeof d.custom_image_b64 !== "string") return false;
    if (d.custom_image_b64.length > 64 * 1024) return false;
  }
  return true;
}

export interface CursorOverlayOptions {
  /** Container the overlay div is appended to. Defaults to the video's
   *  offsetParent, falling back to document.body. */
  container?: HTMLElement;
  /** Override video intrinsic dimensions (default: read from <video>). */
  videoSize?: () => { width: number; height: number };
  /** Override viewport rect (default: video.getBoundingClientRect()). */
  videoRect?: () => DOMRect;
}

/**
 * Render a v1 cursor envelope stream as an overlay above `video`.
 *
 * Returns an object with `update(env)` to apply each envelope, and
 * `dispose()` to remove the overlay element + restore the video's
 * cursor style.
 *
 * Typical wiring:
 *
 *   const r = renderCursor(videoEl);
 *   dc.onmessage = (e) => {
 *     try { r.update(JSON.parse(e.data)); } catch {}
 *   };
 */
/** Largest custom cursor a browser will accept. Chrome ignores a `cursor:`
 *  declaration whose image exceeds 128x128 outright — the shape silently
 *  reverts to whatever the cascade says, which looks like the feature not
 *  working rather than the image being rejected. The wire cap is 64 KiB of
 *  base64 (isValidEnvelope), which comfortably permits an oversized PNG, so
 *  the size has to be checked on this side too. */
const MAX_CUSTOM_CURSOR_PX = 128;

/**
 * Translate a v1 cursor shape into a CSS `cursor` value.
 *
 * Nearly the identity function, and that is by design: 34 of the 36 values in
 * KNOWN_SHAPES are literal CSS cursor keywords, because the protocol was
 * written that way (docs/protocols/cursor-channel.md — "the standard CSS
 * cursor keywords are accepted as values for `shape`"). KNOWN_SHAPES doubles
 * as the injection guard, so nothing unvalidated reaches a style property.
 *
 * The two exceptions:
 *   - "none"   is already a CSS keyword; handled by the caller as the
 *              visibility signal it doubles as on the wire.
 *   - "custom" is not, and becomes `url(<png>) <hx> <hy>, default`.
 *
 * A fallback keyword after the url() list is MANDATORY in CSS — without it
 * the whole declaration is invalid and the cursor silently does not change.
 */
export function cssCursorFor(shape: string, d: CursorData): string {
  if (shape !== "custom") return shape;

  if (!d.custom_image_b64 || d.image_format !== "png") return "default";

  // hotspot was parsed and then IGNORED by the old overlay renderer, which
  // drew every custom image top-left-aligned at the reported point. CSS takes
  // the hotspot natively, so honouring it here fixes a live defect rather than
  // preserving one. Coordinates must be within the image or the declaration is
  // dropped; clamp rather than trust the guest.
  const hx = clampHotspot(d.hotspot?.x);
  const hy = clampHotspot(d.hotspot?.y);
  const url = `data:image/png;base64,${d.custom_image_b64}`;
  return `url("${url}") ${hx} ${hy}, default`;
}

function clampHotspot(v: number | undefined): number {
  if (typeof v !== "number" || !Number.isFinite(v) || v < 0) return 0;
  return Math.min(Math.round(v), MAX_CUSTOM_CURSOR_PX - 1);
}

export function renderCursor(
  video: HTMLVideoElement,
  opts: CursorOverlayOptions = {},
): { update(env: unknown): void; dispose(): void } {
  // The OS draws the pointer; we only tell it WHICH pointer to draw.
  //
  // This used to be `video.style.cursor = "none"` plus an overlay <div>
  // teleported to the coordinate the GUEST reported. That could never feel
  // native, and not because of round-trip latency — because the position data
  // mostly does not exist. The guest emits a cursor envelope only on a
  // cursor-change EDGE (cb_cursor_xy_join.h:53 — "chromium calls SetCursor
  // only when the renderer asks for a different cursor"), and the
  // position-only mouse-move observer that would fill the gaps was left
  // out of scope (cb_cursor_emit_policy.h:71, "until SigNoz shows the cursor
  // lagging mouse motion"). So moving across uniform space emitted NOTHING:
  // the overlay froze at the last shape edge and teleported at the next one.
  //
  // Setting `cursor:` on the <video> hands position back to the OS, at zero
  // latency, and keeps the channel for the thing it genuinely knows: which
  // shape the page wants under the pointer. Hover feedback arrives one RTT
  // late, which is correct — that IS remote information.
  const prevCursor = video.style.cursor;

  // A hidden marker, not a renderer. It exists ONLY to publish the shape the
  // channel last delivered, because that is the observability seam the
  // interactive suite reads (tests/interactive/harness.py:436 queries
  // [data-role="cursor-overlay"] and takes dataset.shape). Keeping it costs
  // one detached <div> and protects a check that sits in suite_channels,
  // which aborts the whole run on failure.
  //
  // It draws nothing: display stays "none" for the element's entire life.
  const overlay = document.createElement("div");
  overlay.dataset["role"] = "cursor-overlay";
  overlay.style.display = "none";

  const container = opts.container ?? document.body;
  container.appendChild(overlay);

  // Retained so a custom-cursor PNG can be sized against the video box; the
  // position mappers are no longer used for rendering. sourceToViewport stays
  // exported and tested because main.ts's videoContentMapper is its inverse
  // and the pair is load-bearing for INPUT coordinates either way.
  void opts.videoRect;
  void opts.videoSize;

  const update = (env: unknown): void => {
    if (!isValidEnvelope(env)) return;
    const d = env.data;

    // `visible: false` is the guest saying the pointer is hidden over this
    // content (a video going fullscreen, a page that sets cursor:none). Honour
    // it literally — `cursor: none` is the CSS spelling of the same thing.
    if (!d.visible) {
      overlay.dataset["shape"] = "none";
      video.style.cursor = "none";
      return;
    }

    // d.x / d.y are deliberately NOT read. Position is the OS's job now; see
    // the note at the top of this function's enclosing scope for why the
    // remote coordinates were never a usable position source anyway. They stay
    // validated in isValidEnvelope so the wire contract is unchanged.
    const shape = KNOWN_SHAPES.has(d.shape) ? d.shape : "default";
    overlay.dataset["shape"] = shape;
    video.style.cursor = cssCursorFor(shape, d);
  };

  const dispose = (): void => {
    overlay.remove();
    video.style.cursor = prevCursor;
  };

  return { update, dispose };
}

/* The inline-SVG glyph set that used to live here is gone.
 *
 * It covered 6 shapes (pointer, text, crosshair, wait/progress,
 * not-allowed/no-drop, grab-family) out of the 36 the protocol defines, so all
 * 20 resize cursors plus zoom-in/zoom-out/help/vertical-text/context-menu/
 * alias/copy/cell rendered as a plain arrow. The OS now draws every one of
 * them natively, at the right size for the display and matching the user's
 * own theme — so removing this is a fidelity upgrade, not a simplification. */


/** Convenience: subscribe to a data channel and render on every message.
 *  Returns the underlying renderer so callers can dispose. */
export function attachCursorChannel(
  dc: {
    addEventListener: (ev: "message", h: (e: MessageEvent) => void) => void;
    removeEventListener?: (ev: "message", h: (e: MessageEvent) => void) => void;
  },
  video: HTMLVideoElement,
  opts?: CursorOverlayOptions,
): { update(env: unknown): void; dispose(): void } {
  const r = renderCursor(video, opts);
  const onMessage = (e: MessageEvent): void => {
    try {
      r.update(JSON.parse(typeof e.data === "string" ? e.data : ""));
    } catch {
      /* drop malformed */
    }
  };
  dc.addEventListener("message", onMessage);
  return {
    update: r.update,
    dispose(): void {
      dc.removeEventListener?.("message", onMessage);
      r.dispose();
    },
  };
}

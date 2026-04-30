// Cursor renderer for the cloud-browser-webrtc client.
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
export function renderCursor(
  video: HTMLVideoElement,
  opts: CursorOverlayOptions = {},
): { update(env: unknown): void; dispose(): void } {
  // Hide whatever cursor the streamed frame may carry.
  const prevCursor = video.style.cursor;
  video.style.cursor = "none";

  const overlay = document.createElement("div");
  overlay.dataset["role"] = "cursor-overlay";
  overlay.style.position = "fixed";
  overlay.style.left = "0";
  overlay.style.top = "0";
  overlay.style.pointerEvents = "none";
  overlay.style.willChange = "transform";
  overlay.style.transform = "translate3d(-9999px,-9999px,0)";
  overlay.style.width = "16px";
  overlay.style.height = "16px";
  overlay.style.cursor = "default";
  overlay.style.zIndex = "2147483646";
  overlay.style.display = "none";

  // We render the actual glyph as a single inner span carrying the CSS
  // cursor — this lets the OS-native cursor draw at the overlay
  // position using `cursor:` on a non-empty element. (Modern browsers
  // require pointer events for cursor display, so we use a tiny dot
  // with mouse passthrough disabled — the cursor still shows because
  // the browser still queries the styled element for cursor when the
  // mouse hovers it implicitly via the overlay.)
  //
  // Practical reality: relying on CSS `cursor` outside a real hover is
  // unreliable. So we draw an inline-SVG arrow for `default`/`pointer`
  // and a text-caret SVG for `text`, and use a 1×1 transparent image
  // as a fallback for everything else (deferred to Phase 2 polish).
  const inner = document.createElement("span");
  inner.style.display = "block";
  inner.style.width = "100%";
  inner.style.height = "100%";
  overlay.appendChild(inner);

  const container = opts.container ?? document.body;
  container.appendChild(overlay);

  const getRect = opts.videoRect ?? (() => video.getBoundingClientRect());
  const getSize = opts.videoSize ?? (() => ({
    width: video.videoWidth || 0,
    height: video.videoHeight || 0,
  }));

  const update = (env: unknown): void => {
    if (!isValidEnvelope(env)) return;
    const d = env.data;
    if (!d.visible) {
      overlay.style.display = "none";
      return;
    }
    const rect = getRect();
    const { width: vw, height: vh } = getSize();
    const { x: cx, y: cy } = sourceToViewport(d.x, d.y, rect, vw, vh);

    overlay.style.display = "block";
    overlay.style.transform = `translate3d(${Math.round(cx)}px, ${Math.round(cy)}px, 0)`;

    const shape = KNOWN_SHAPES.has(d.shape) ? d.shape : "default";
    overlay.dataset["shape"] = shape;

    if (shape === "custom" && d.custom_image_b64 && d.image_format === "png") {
      const url = `data:image/png;base64,${d.custom_image_b64}`;
      inner.style.backgroundImage = `url("${url}")`;
      inner.style.backgroundRepeat = "no-repeat";
      inner.style.backgroundSize = "contain";
      inner.innerHTML = "";
    } else {
      inner.style.backgroundImage = "";
      inner.innerHTML = svgFor(shape);
    }
  };

  const dispose = (): void => {
    overlay.remove();
    video.style.cursor = prevCursor;
  };

  return { update, dispose };
}

/** Inline SVG glyphs for the most common shapes. Anything not covered
 *  here renders as the default arrow. Sizes are 16x16 SVGs centered on
 *  the (0,0) hotspot for arrow / pointer; (8,8) for crosshair / text.
 *  Phase 2 will replace these with a richer set. */
function svgFor(shape: string): string {
  switch (shape) {
    case "pointer":
      // Hand
      return `<svg xmlns="http://www.w3.org/2000/svg" width="20" height="22" viewBox="0 0 20 22">
        <path fill="white" stroke="black" stroke-width="1.2"
          d="M5 1v9.5L3.2 8.7c-.7-.7-1.8-.7-2.4 0-.7.7-.7 1.8 0 2.4l5.7 6c.7.8 1.7 1.2 2.7 1.2h4.4c2 0 3.6-1.6 3.6-3.6V8.5c0-1-.8-1.8-1.8-1.8s-1.8.8-1.8 1.8V6.7c0-1-.8-1.8-1.8-1.8s-1.8.8-1.8 1.8V5c0-1-.8-1.8-1.8-1.8s-1.8.8-1.8 1.8V1c0-.6-.4-1-1-1S5 .4 5 1z"/>
      </svg>`;
    case "text":
      return `<svg xmlns="http://www.w3.org/2000/svg" width="14" height="20" viewBox="0 0 14 20" style="transform:translate(-7px,-10px)">
        <path fill="white" stroke="black" stroke-width="1.2" d="M3 1h8M3 19h8M7 1v18"/>
      </svg>`;
    case "crosshair":
      return `<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16" style="transform:translate(-8px,-8px)">
        <path fill="none" stroke="black" stroke-width="1.2" d="M8 0v16M0 8h16"/>
      </svg>`;
    case "wait":
    case "progress":
      return `<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <circle cx="8" cy="8" r="6" fill="white" stroke="black" stroke-width="1.2"/>
        <path d="M8 4v4l3 2" fill="none" stroke="black" stroke-width="1.2"/>
      </svg>`;
    case "not-allowed":
    case "no-drop":
      return `<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
        <circle cx="8" cy="8" r="6" fill="white" stroke="red" stroke-width="2"/>
        <path d="M3 13L13 3" stroke="red" stroke-width="2"/>
      </svg>`;
    case "grab":
    case "grabbing":
    case "move":
    case "all-scroll":
      return `<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 20 20">
        <path fill="white" stroke="black" stroke-width="1.2"
          d="M10 2l3 3h-2v4h4V7l3 3-3 3v-2h-4v4h2l-3 3-3-3h2v-4H5v2L2 10l3-3v2h4V5H7z"/>
      </svg>`;
    case "default":
    default:
      // Arrow
      return `<svg xmlns="http://www.w3.org/2000/svg" width="16" height="22" viewBox="0 0 16 22">
        <path fill="white" stroke="black" stroke-width="1.2"
          d="M1 1v18l5-4h7L1 1z"/>
      </svg>`;
  }
}

/** Convenience: subscribe to a data channel and render on every message.
 *  Returns the underlying renderer so callers can dispose. */
export function attachCursorChannel(
  dc: { addEventListener: (ev: "message", h: (e: MessageEvent) => void) => void },
  video: HTMLVideoElement,
  opts?: CursorOverlayOptions,
): { update(env: unknown): void; dispose(): void } {
  const r = renderCursor(video, opts);
  dc.addEventListener("message", (e) => {
    try {
      r.update(JSON.parse(typeof e.data === "string" ? e.data : ""));
    } catch {
      /* drop malformed */
    }
  });
  return r;
}

// Make the remote browser follow the size of the stage it is shown in.
//
// Until this existed the stream was a fixed 1280x720 letterbox whatever the
// viewer's window did: `Cb.setViewport` had landed in the embedder
// (docs/protocols/cb-cdp-methods.md) and nothing on the client side ever
// called it. Resizing the window shrank the black stage around the same
// picture, and text on a large monitor stayed small and soft.
//
// Shape:
//   ResizeObserver on the stage → debounce (250 ms, the same the triform
//   portal uses) → POST /api/viewport {width, height} → the gateway issues
//   Cb.setViewport → the guest answers with the size it ACTUALLY applied →
//   the caller is told, so it can log a clamp rather than assume the request
//   landed verbatim.
//
// Sizes are CSS pixels (DIP). devicePixelRatio is deliberately not sent: the
// guest clamps the scale factor to 1.0 until input coordinates are
// DIP-normalised on its side, and asking for HiDPI before that would put
// every click in the wrong place (cb-cdp-methods.md, "HiDPI is not enabled
// yet"). When that lands, this is the one place that grows a third field.
//
// Failure model: one 5xx/4xx and the follower STOPS. The overwhelmingly
// common cause is a guest that predates Cb.setViewport (the gateway relays
// CDP's "wasn't found"), and re-asking on every resize would turn a known
// limitation into a request storm. `resume()` exists for a reconnect.

export interface ViewportApplied {
  width: number;
  height: number;
  deviceScaleFactor: number;
}

export type ViewportResult =
  | { ok: true; applied: ViewportApplied; clamped: boolean }
  | { ok: false; error: string };

const ENDPOINT = "/api/viewport";

/** Round a CSS size the way the guest will: to an even integer. */
export function evenDown(v: number): number {
  return Math.max(0, Math.floor(v)) & ~1;
}

/** POST one resize. Exported for tests and for callers that size explicitly. */
export async function requestViewport(width: number, height: number): Promise<ViewportResult> {
  const w = evenDown(width);
  const h = evenDown(height);
  if (w <= 0 || h <= 0) return { ok: false, error: "empty stage" };
  try {
    const res = await fetch(ENDPOINT, {
      method: "POST",
      credentials: "same-origin",
      cache: "no-store",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ width: w, height: h }),
    });
    const payload = (await res.json().catch(() => ({}))) as Partial<ViewportApplied> & { error?: string };
    if (!res.ok) return { ok: false, error: payload.error ?? `HTTP ${res.status}` };
    if (typeof payload.width !== "number" || typeof payload.height !== "number") {
      return { ok: false, error: "gateway returned no geometry" };
    }
    const applied: ViewportApplied = {
      width: payload.width,
      height: payload.height,
      deviceScaleFactor: typeof payload.deviceScaleFactor === "number" ? payload.deviceScaleFactor : 1,
    };
    return { ok: true, applied, clamped: applied.width !== w || applied.height !== h };
  } catch (err) {
    return { ok: false, error: err instanceof Error ? err.message : String(err) };
  }
}

export interface ViewportFollowerOptions {
  /** Quiet period after the last resize before a request is sent. Default 250. */
  debounceMs?: number;
  /** Called after every request, success or failure. */
  onResult?: (r: ViewportResult, requested: { width: number; height: number }) => void;
  /** Injection seams for tests. */
  request?: typeof requestViewport;
  observe?: (el: Element, cb: (w: number, h: number) => void) => () => void;
  setTimer?: (cb: () => void, ms: number) => unknown;
  clearTimer?: (id: unknown) => void;
}

export interface ViewportFollower {
  /** Send the stage's current size now (used on connect). */
  sync(): void;
  /** Re-enable after a failure stopped the follower (used on reconnect). */
  resume(): void;
  dispose(): void;
}

function observeWithResizeObserver(el: Element, cb: (w: number, h: number) => void): () => void {
  if (typeof ResizeObserver === "undefined") return () => {};
  const ro = new ResizeObserver((entries) => {
    const e = entries[entries.length - 1];
    if (!e) return;
    // contentRect is CSS pixels, which is what the guest wants (DIP at DSF 1).
    cb(e.contentRect.width, e.contentRect.height);
  });
  ro.observe(el);
  return () => ro.disconnect();
}

/**
 * Follow `stage`'s size with the remote viewport.
 *
 * `stage` is the element the <video> fills — NOT the <video> itself, whose
 * box follows the stream's aspect ratio and would feed the guest its own
 * output as the new request (a feedback loop that ratchets toward one
 * aspect ratio).
 */
export function followViewport(stage: Element, opts: ViewportFollowerOptions = {}): ViewportFollower {
  const debounceMs = opts.debounceMs ?? 250;
  const request = opts.request ?? requestViewport;
  const observe = opts.observe ?? observeWithResizeObserver;
  const setTimer = opts.setTimer ?? ((cb: () => void, ms: number) => globalThis.setTimeout(cb, ms));
  const clearTimer = opts.clearTimer ?? ((id: unknown) => globalThis.clearTimeout(id as number));

  let last = { width: 0, height: 0 };
  let sent = { width: -1, height: -1 };
  let timer: unknown = null;
  let stopped = false;
  let inFlight = false;
  let disposed = false;

  const fire = () => {
    timer = null;
    if (stopped || disposed || inFlight) return;
    const w = evenDown(last.width);
    const h = evenDown(last.height);
    if (w <= 0 || h <= 0) return;
    if (w === sent.width && h === sent.height) return;
    inFlight = true;
    void request(w, h).then((r) => {
      inFlight = false;
      if (r.ok) {
        sent = { width: r.applied.width, height: r.applied.height };
      } else {
        // Stop asking. See the failure model in the file header.
        stopped = true;
      }
      opts.onResult?.(r, { width: w, height: h });
      // The stage may have moved on while the request was in flight.
      if (!stopped && !disposed && (evenDown(last.width) !== sent.width || evenDown(last.height) !== sent.height)) {
        schedule();
      }
    });
  };

  const schedule = () => {
    if (timer !== null) clearTimer(timer);
    timer = setTimer(fire, debounceMs);
  };

  const detach = observe(stage, (w, h) => {
    last = { width: w, height: h };
    schedule();
  });

  return {
    sync() {
      const rect = stage.getBoundingClientRect();
      last = { width: rect.width, height: rect.height };
      sent = { width: -1, height: -1 };
      fire();
    },
    resume() {
      stopped = false;
      sent = { width: -1, height: -1 };
      schedule();
    },
    dispose() {
      disposed = true;
      if (timer !== null) clearTimer(timer);
      detach();
    },
  };
}

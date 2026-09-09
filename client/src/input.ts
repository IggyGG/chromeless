// Input channel encoder for the chromeless client.
//
// Wraps the RTCDataChannel named "input" (created in main.ts) and turns
// DOM events into the v1 wire envelope documented in
// docs/protocols/input-channel.md.
//
// Design notes:
//   - Mouse moves are coalesced: when more than `coalesceThreshold`
//     are pending, or the channel's bufferedAmount exceeds
//     `bufferedAmountThreshold`, only the latest mouse_move is kept.
//     Every other event type is preserved.
//   - The pending queue flushes on requestAnimationFrame so a burst
//     of pointermove events produces at most one network frame per
//     refresh.
//   - `seq` starts at 0 when the channel opens and increments per send.
//     On close/reopen, callers should construct a fresh InputChannel.
//   - Coordinates are forwarded as supplied by the caller. The
//     `attach(target, opts.toContentCoords)` helper accepts a hook
//     that maps page coordinates to source-image coordinates; if
//     omitted, raw `clientX`/`clientY` relative to the target's
//     bounding box are used.

export const PROTOCOL_VERSION = 1 as const;

export type InputType =
  | "mouse_move"
  | "mouse_button"
  | "mouse_wheel"
  | "key_down"
  | "key_up"
  | "composition_start"
  | "composition_update"
  | "composition_end"
  | "composition_cancel"
  | "clipboard_paste"
  | "clipboard_copy_request"
  // v1.1 — drag-and-drop. See docs/protocols/input-channel.md.
  | "drag_start"
  | "drag_over"
  | "drag_end"
  | "drop"
  // v1.1 — multi-touch. See docs/protocols/input-channel.md.
  | "touch_start"
  | "touch_move"
  | "touch_end"
  | "touch_cancel";

export interface MouseMoveData { x: number; y: number; }
export interface MouseButtonData { button: 0 | 1 | 2 | 3 | 4; action: "down" | "up"; x: number; y: number; }
// v1.1 — phase machine for scroll inertia.
export type WheelPhase = "start" | "changed" | "end" | null;
export type WheelDeltaMode = "pixel" | "line" | "page";

export interface MouseWheelData {
  dx: number; dy: number; mode: 0 | 1 | 2; x: number; y: number;
  // Optional v1.1 fields. Servers without phase handling can ignore.
  delta_mode?: WheelDeltaMode;
  phase?: WheelPhase;
  momentum?: boolean;
}
export interface KeyData { code: string; key: string; mods: number; }
// v1.1 (T88) — extended composition payload.
export interface CompositionRect { x: number; y: number; w: number; h: number; }
export interface CompositionData {
  data: string;
  // Caret + selection within `data`, in UTF-16 code units. Optional;
  // server defaults to caret-at-end when omitted.
  selection_start?: number;
  selection_end?: number;
  // composition_start ONLY: bounding rect of the focused element /
  // caret in source-content coords. Reserved for client-side
  // candidate UI; v1.1 servers may ignore.
  rect?: CompositionRect;
  // Optional list of IME candidates; unobservable from a DOM client
  // in v1.1 so clients leave it empty. Reserved for v2 platform-
  // specific bridges.
  candidate_list?: string[];
}
// composition_cancel carries no data — the envelope's `type` is the
// signal. We model it as the no-payload case for symmetry with
// other cancel-style events.
export type CompositionCancelData = Record<string, never>;
export interface ClipboardPasteData { text: string; }

// v1.1 — drag-and-drop payload shapes. See protocol doc for semantics.
export interface DragItem {
  /** "string" carries `data`; "file" is informational only in v1 (no bytes). */
  kind: "string" | "file";
  type: string; // MIME type, lower-case
  data?: string; // present iff kind === "string"
}
export interface DragStartData { x: number; y: number; types: string[]; items: DragItem[]; }
export interface DragOverData  { x: number; y: number; }
export interface DropData      { x: number; y: number; types: string[]; items: DragItem[]; }
export interface DragEndData   { success: boolean; }

// v1.1 — touch payload shapes.
export interface TouchStartData {
  identifier: number; x: number; y: number;
  radius_x: number; radius_y: number; force: number; twist: number;
}
export interface TouchMoveData {
  identifier: number; x: number; y: number;
  radius_x: number; radius_y: number; force: number; twist: number;
}
export interface TouchEndData    { identifier: number; }
export interface TouchCancelData { identifier: number; }

export type InputData =
  | MouseMoveData
  | MouseButtonData
  | MouseWheelData
  | KeyData
  | CompositionData
  | CompositionCancelData
  | ClipboardPasteData
  | DragStartData
  | DragOverData
  | DropData
  | DragEndData
  | TouchStartData
  | TouchMoveData
  | TouchEndData
  | TouchCancelData
  | Record<string, never>;

export interface InputEnvelope {
  v: typeof PROTOCOL_VERSION;
  type: InputType;
  t: number;
  seq: number;
  data: InputData;
}

// Modifier bitmask matches docs/protocols/input-channel.md.
export const MOD_SHIFT = 1;
export const MOD_CTRL  = 2;
export const MOD_ALT   = 4;
export const MOD_META  = 8;

export function modsFromEvent(e: { shiftKey: boolean; ctrlKey: boolean; altKey: boolean; metaKey: boolean }): number {
  return (e.shiftKey ? MOD_SHIFT : 0)
       | (e.ctrlKey  ? MOD_CTRL  : 0)
       | (e.altKey   ? MOD_ALT   : 0)
       | (e.metaKey  ? MOD_META  : 0);
}

// Minimal DataChannel surface so the class is testable without a real
// RTCDataChannel.
export interface SendableChannel {
  readonly readyState: "connecting" | "open" | "closing" | "closed";
  readonly bufferedAmount: number;
  send(data: string): void;
}

export interface InputChannelOptions {
  /** Drop oldest mouse_move when more than this number are queued. */
  coalesceThreshold?: number;
  /** Skip queueing new mouse_moves when channel.bufferedAmount exceeds this. */
  bufferedAmountThreshold?: number;
  /** Wall-clock supplier (override for tests). */
  now?: () => number;
  /** rAF supplier (override for tests; should return an id, like requestAnimationFrame). */
  raf?: (cb: () => void) => number;
  /** cancelAnimationFrame counterpart for `raf`. */
  cancelRaf?: (id: number) => void;
  /** Called every time we drop a mouse_move because of backpressure. */
  onCoalesce?: (droppedCount: number) => void;
  /** Called when send() throws. */
  onError?: (err: unknown, env: InputEnvelope) => void;
  // ----- v1.1 wheel-inertia tuning -----
  /** Quiet window after the last non-zero wheel before we synthesize phase=end. Default 150 ms. */
  wheelEndDelayMs?: number;
  /** Inter-event gap below which a wheel is considered momentum (heuristic). Default 100 ms. */
  wheelMomentumGapMs?: number;
  /** Deferred-task scheduler for wheel-end synthesis. Override for tests. Default setTimeout. */
  setTimer?: (cb: () => void, ms: number) => unknown;
  /** Cancel a deferred task scheduled by setTimer. Default clearTimeout. */
  clearTimer?: (id: unknown) => void;
}

const DEFAULT_COALESCE_THRESHOLD = 4;
const DEFAULT_BUFFERED_THRESHOLD = 64 * 1024;
const DEFAULT_WHEEL_END_DELAY_MS = 150;
const DEFAULT_WHEEL_MOMENTUM_GAP_MS = 100;

/**
 * The minimum an event target has to look like for `keyBelongsToClient`.
 * Duck-typed rather than `HTMLElement` so the rule is testable without a
 * DOM — this package's tests run in node, and a policy nobody can test is
 * how the previous one (there wasn't one) survived.
 */
export interface KeyTargetLike {
  tagName?: string;
  isContentEditable?: boolean;
  closest?: (selector: string) => unknown;
}

/** Client-owned regions: the control-channel prompt and the file picker. */
const CLIENT_OWNED_SELECTORS = [".cb-control-overlay", ".cb-filepick"];

/**
 * Is this key the CLIENT's rather than the cloud browser's?
 *
 * Keys are listened for on the window — a <video> cannot hold focus in a
 * way that delivers key events, so a target-scoped listener would receive
 * nothing. Without a policy that means EVERY key reaches the guest,
 * including the URL a user types into the client's own address bar, which
 * was streamed keystroke by keystroke into whatever the remote page had
 * focused. Browser shortcuts (Cmd/Ctrl+L, +T, +W, +R) fired in BOTH
 * browsers at once.
 *
 * The rule is about WHERE FOCUS IS, not which key it is. Enumerating
 * "browser shortcuts" would mean preventDefault on keys the viewer may
 * genuinely want locally, and a stream that swallows Cmd+W is worse than
 * one that merely does not forward it.
 */
export function keyBelongsToClient(target: KeyTargetLike | null | undefined): boolean {
  if (!target) return false;
  const tag = (target.tagName ?? "").toUpperCase();
  if (tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT" ||
      tag === "BUTTON") {
    return true;
  }
  if (target.isContentEditable === true) return true;
  if (typeof target.closest === "function") {
    for (const sel of CLIENT_OWNED_SELECTORS) {
      if (target.closest(sel)) return true;
    }
  }
  return false;
}

export class InputChannel {
  private readonly ch: SendableChannel;
  private readonly opts: Required<Pick<InputChannelOptions,
      "coalesceThreshold" | "bufferedAmountThreshold" | "now"
      | "wheelEndDelayMs" | "wheelMomentumGapMs">> &
    Pick<InputChannelOptions,
      "raf" | "cancelRaf" | "onCoalesce" | "onError"
      | "setTimer" | "clearTimer">;
  private seq = 0;
  /** Queue of pending mouse_move data; only the last one is kept after coalesce. */
  private pendingMove: MouseMoveData | null = null;
  /** Queue of pending drag_over data; only the last one is kept after coalesce. */
  private pendingDragOver: DragOverData | null = null;
  /** Queue of pending touch_move data per identifier; latest wins per finger. */
  private pendingTouchMoves = new Map<number, TouchMoveData>();
  // T88 — true between compositionstart and compositionend; used to
  // suppress raw key forwarding during IME composition per the
  // protocol's "no key_* during composition" rule.
  private composing = false;
  // v1.1 wheel-phase state. Reset to "idle" by emitWheelEnd().
  private wheelPhase: "idle" | "active" = "idle";
  private wheelLastEventNow = 0;
  private wheelPrevAbsDelta = 0;
  private wheelEndTimerId: unknown = null;
  private wheelLastPos = { x: 0, y: 0, mode: 0 as 0 | 1 | 2 };
  /** Queue of non-coalescable envelopes. Flushed in order on rAF tick. */
  private pendingOther: InputEnvelope[] = [];
  private rafId: number | null = null;
  private droppedSinceLastFlush = 0;

  constructor(ch: SendableChannel, opts: InputChannelOptions = {}) {
    this.ch = ch;
    this.opts = {
      coalesceThreshold: opts.coalesceThreshold ?? DEFAULT_COALESCE_THRESHOLD,
      bufferedAmountThreshold: opts.bufferedAmountThreshold ?? DEFAULT_BUFFERED_THRESHOLD,
      now: opts.now ?? Date.now,
      wheelEndDelayMs: opts.wheelEndDelayMs ?? DEFAULT_WHEEL_END_DELAY_MS,
      wheelMomentumGapMs: opts.wheelMomentumGapMs ?? DEFAULT_WHEEL_MOMENTUM_GAP_MS,
      ...(opts.raf !== undefined ? { raf: opts.raf } : {}),
      ...(opts.cancelRaf !== undefined ? { cancelRaf: opts.cancelRaf } : {}),
      ...(opts.onCoalesce !== undefined ? { onCoalesce: opts.onCoalesce } : {}),
      ...(opts.onError !== undefined ? { onError: opts.onError } : {}),
      ...(opts.setTimer !== undefined ? { setTimer: opts.setTimer } : {}),
      ...(opts.clearTimer !== undefined ? { clearTimer: opts.clearTimer } : {}),
    };
  }

  // ----- public senders -----

  sendMouseMove(x: number, y: number): void {
    if (this.ch.bufferedAmount > this.opts.bufferedAmountThreshold) {
      // Backpressure: drop entirely. Any prior queued move is also
      // superseded — the latest position is still the most useful one,
      // so keep replacing pendingMove.
      this.droppedSinceLastFlush++;
    }
    if (this.pendingMove !== null) {
      this.droppedSinceLastFlush++;
    }
    this.pendingMove = { x: Math.round(x), y: Math.round(y) };
    this.scheduleFlush();
  }

  sendMouseButton(button: 0 | 1 | 2 | 3 | 4, action: "down" | "up", x: number, y: number): void {
    this.enqueue("mouse_button", { button, action, x: Math.round(x), y: Math.round(y) });
  }

  sendMouseWheel(dx: number, dy: number, mode: 0 | 1 | 2, x: number, y: number): void {
    const now = this.opts.now();
    const rdx = Math.round(dx);
    const rdy = Math.round(dy);
    const rx  = Math.round(x);
    const ry  = Math.round(y);
    const absDelta = Math.abs(rdx) + Math.abs(rdy);
    const isZero = absDelta === 0;

    // Determine phase. Zero-delta events from outside this class
    // (rare) are forwarded as `changed` if a gesture is active,
    // otherwise dropped as no-ops — they have no semantic content
    // outside the phase machine.
    let phase: WheelPhase;
    if (this.wheelPhase === "idle") {
      if (isZero) {
        // No active gesture and a zero delta — nothing to forward.
        return;
      }
      phase = "start";
      this.wheelPhase = "active";
      this.wheelPrevAbsDelta = 0;
    } else {
      phase = "changed";
    }

    // Momentum heuristic: events that arrive within
    // wheelMomentumGapMs of the previous one AND whose magnitude is
    // smaller than the previous magnitude are likely OS-generated
    // momentum frames, not user-driven. The first event of a gesture
    // is never momentum.
    const gap = now - this.wheelLastEventNow;
    const momentum = phase === "changed"
      && gap > 0 && gap < this.opts.wheelMomentumGapMs
      && absDelta > 0
      && absDelta < this.wheelPrevAbsDelta;

    this.wheelLastEventNow = now;
    this.wheelPrevAbsDelta = absDelta;
    this.wheelLastPos = { x: rx, y: ry, mode };

    this.enqueue("mouse_wheel", {
      dx: rdx, dy: rdy, mode, x: rx, y: ry,
      delta_mode: deltaModeName(mode),
      phase,
      momentum,
    });

    // Reset / arm the end-of-gesture synthesizer. wheelEndDelayMs of
    // quiet → emit a phase=end zero-delta envelope.
    this.cancelWheelEndTimer();
    const setTimer = this.opts.setTimer ?? ((cb: () => void, ms: number) =>
      globalThis.setTimeout(cb, ms));
    this.wheelEndTimerId = setTimer(() => this.emitWheelEnd(), this.opts.wheelEndDelayMs);
  }

  private cancelWheelEndTimer(): void {
    if (this.wheelEndTimerId === null || this.wheelEndTimerId === undefined) return;
    const clearTimer = this.opts.clearTimer ?? ((id: unknown) =>
      globalThis.clearTimeout(id as number));
    clearTimer(this.wheelEndTimerId);
    this.wheelEndTimerId = null;
  }

  /** Synthesize the end-of-gesture envelope. Public for tests so
   *  they can fire it deterministically via the injected setTimer. */
  emitWheelEnd(): void {
    if (this.wheelPhase !== "active") return;
    const { x, y, mode } = this.wheelLastPos;
    this.wheelPhase = "idle";
    this.wheelPrevAbsDelta = 0;
    this.wheelEndTimerId = null;
    this.enqueue("mouse_wheel", {
      dx: 0, dy: 0, mode, x, y,
      delta_mode: deltaModeName(mode),
      phase: "end",
      momentum: false,
    });
  }

  sendKeyDown(code: string, key: string, mods: number): void {
    this.enqueue("key_down", { code, key, mods });
  }

  sendKeyUp(code: string, key: string, mods: number): void {
    this.enqueue("key_up", { code, key, mods });
  }

  /**
   * Send a composition envelope. Two call shapes are supported for
   * back-compat with v1.0 callers:
   *
   *   sendComposition("update", "ni hao")            // legacy: text only
   *   sendComposition("update", { data: "ni hao",
   *                                selection_start: 6,
   *                                selection_end: 6 })  // v1.1
   *
   * Pass `composition_cancel` via the dedicated sendCompositionCancel().
   */
  sendComposition(
    phase: "start" | "update" | "end",
    payload: string | CompositionData,
  ): void {
    const data: CompositionData = typeof payload === "string"
      ? { data: payload }
      : payload;
    this.enqueue(`composition_${phase}` as const, data);
  }

  /** v1.1 — IME aborted (Esc / focus loss). Carries no payload. */
  sendCompositionCancel(): void {
    this.enqueue("composition_cancel", {});
  }

  /**
   * v1 `clipboard_paste`. Kept for direct callers of this channel, but NOT
   * wired to the browser's paste event — see the note in attach(). The
   * guest recognises the type and handles it nowhere, so a paste sent here
   * is silently dropped; the working path is the clipboard channel's
   * `clipboard_offer` (client/src/clipboard.ts).
   */
  sendClipboardPaste(text: string): void {
    this.enqueue("clipboard_paste", { text });
  }

  sendClipboardCopyRequest(): void {
    this.enqueue("clipboard_copy_request", {});
  }

  // v1.1 — drag-and-drop senders. drag_over coalesces with the same
  // shape as mouse_move (latest position wins).

  sendDragStart(x: number, y: number, types: string[], items: DragItem[]): void {
    this.enqueue("drag_start", {
      x: Math.round(x), y: Math.round(y), types, items,
    });
  }

  sendDragOver(x: number, y: number): void {
    if (this.ch.bufferedAmount > this.opts.bufferedAmountThreshold) {
      this.droppedSinceLastFlush++;
    }
    if (this.pendingDragOver !== null) {
      this.droppedSinceLastFlush++;
    }
    this.pendingDragOver = { x: Math.round(x), y: Math.round(y) };
    this.scheduleFlush();
  }

  sendDrop(x: number, y: number, types: string[], items: DragItem[]): void {
    this.enqueue("drop", {
      x: Math.round(x), y: Math.round(y), types, items,
    });
  }

  sendDragEnd(success: boolean): void {
    this.enqueue("drag_end", { success });
  }

  // v1.1 — touch senders. touch_move coalesces per identifier (latest
  // position wins per finger); touch_start/end/cancel are not coalesced.

  sendTouchStart(t: TouchStartData): void {
    this.enqueue("touch_start", {
      identifier: t.identifier,
      x: Math.round(t.x), y: Math.round(t.y),
      radius_x: Math.max(1, Math.round(t.radius_x)),
      radius_y: Math.max(1, Math.round(t.radius_y)),
      force: t.force, twist: Math.round(t.twist),
    });
  }

  sendTouchMove(t: TouchMoveData): void {
    if (this.ch.bufferedAmount > this.opts.bufferedAmountThreshold) {
      this.droppedSinceLastFlush++;
    }
    if (this.pendingTouchMoves.has(t.identifier)) {
      this.droppedSinceLastFlush++;
    }
    this.pendingTouchMoves.set(t.identifier, {
      identifier: t.identifier,
      x: Math.round(t.x), y: Math.round(t.y),
      radius_x: Math.max(1, Math.round(t.radius_x)),
      radius_y: Math.max(1, Math.round(t.radius_y)),
      force: t.force, twist: Math.round(t.twist),
    });
    this.scheduleFlush();
  }

  sendTouchEnd(identifier: number): void {
    // Force any queued touch_move for this finger out before the end
    // event so the server sees the last-known position before the lift.
    const queued = this.pendingTouchMoves.get(identifier);
    if (queued !== undefined) {
      this.pendingTouchMoves.delete(identifier);
      this.pendingOther.push({
        v: PROTOCOL_VERSION, type: "touch_move",
        t: this.opts.now(), seq: 0, data: queued,
      });
    }
    this.enqueue("touch_end", { identifier });
  }

  sendTouchCancel(identifier: number): void {
    // Cancel discards any queued move for the same finger — the
    // server should treat the finger as gone immediately.
    this.pendingTouchMoves.delete(identifier);
    this.enqueue("touch_cancel", { identifier });
  }

  /** Force-flush the queue immediately. */
  flush(): void {
    if (this.rafId !== null && this.opts.cancelRaf !== undefined) {
      this.opts.cancelRaf(this.rafId);
    }
    this.rafId = null;
    this.doFlush();
  }

  /**
   * Wire DOM listeners onto a target element.
   * Returns a detach function that removes them all.
   */
  attach(
    target: HTMLElement,
    extra: {
      toContentCoords?: (clientX: number, clientY: number, rect: DOMRect) => { x: number; y: number };
      window?: Window;
      // Should this keyboard event be forwarded to the guest at all?
      //
      // Keys are listened for on the WINDOW, not on `target`, and they have
      // to be: a <video> cannot hold focus in a way that gives it key
      // events, so a target-scoped listener would receive nothing. The
      // consequence is that EVERY key in the page goes to the guest —
      // including what the user types into the client's own address bar,
      // and including Cmd/Ctrl+L, +T, +W and +R, which fire locally AND
      // remotely at once.
      //
      // Absent, every key is forwarded, which is the historical behaviour
      // and what the input tests assume. main.ts supplies the real policy.
      shouldForwardKey?: (e: KeyboardEvent) => boolean;
    } = {},
  ): () => void {
    const win = extra.window ?? globalThis.window;
    const shouldForwardKey = extra.shouldForwardKey ?? (() => true);
    const map = extra.toContentCoords ?? ((cx, cy, rect) => ({ x: cx - rect.left, y: cy - rect.top }));

    const onMouseMove = (e: MouseEvent) => {
      const { x, y } = map(e.clientX, e.clientY, target.getBoundingClientRect());
      this.sendMouseMove(x, y);
    };
    const onMouseDown = (e: MouseEvent) => {
      const { x, y } = map(e.clientX, e.clientY, target.getBoundingClientRect());
      this.sendMouseButton(domButton(e.button), "down", x, y);
    };
    const onMouseUp = (e: MouseEvent) => {
      const { x, y } = map(e.clientX, e.clientY, target.getBoundingClientRect());
      this.sendMouseButton(domButton(e.button), "up", x, y);
    };
    const onWheel = (e: WheelEvent) => {
      const { x, y } = map(e.clientX, e.clientY, target.getBoundingClientRect());
      this.sendMouseWheel(e.deltaX, e.deltaY, (e.deltaMode as 0 | 1 | 2) ?? 0, x, y);
      e.preventDefault();
    };
    const onContextMenu = (e: MouseEvent) => { e.preventDefault(); };
    // T88 — IME-aware key forwarding. Per the protocol's "no key_*
    // during composition" rule, we suppress key events the browser
    // marks as part of an in-progress composition. Detection uses:
    //   - KeyboardEvent.isComposing  (W3C standard, true between
    //     compositionstart and compositionend)
    //   - keyCode === 229            (Chromium's "this is composition"
    //     sentinel — fires for the IME-trigger key on platforms where
    //     isComposing isn't yet set)
    //   - this.composing             (our own latch; covers timing
    //     edge cases and tests that don't propagate isComposing)
    const isComposingKey = (e: KeyboardEvent) =>
      this.composing
      || (e as KeyboardEvent & { isComposing?: boolean }).isComposing === true
      || (e as KeyboardEvent & { keyCode?: number }).keyCode === 229;

    const onKeyDown = (e: KeyboardEvent) => {
      if (isComposingKey(e)) return;
      if (!shouldForwardKey(e)) return;
      // Special-case: Escape during composition is a CANCEL signal.
      // Some IMEs raise compositionend with an empty data string in
      // this path; others don't. We surface the cancel intent
      // unconditionally and let the bridge dedupe.
      if (this.composing && e.key === "Escape") {
        this.sendCompositionCancel();
        return;
      }
      this.sendKeyDown(e.code, e.key, modsFromEvent(e));
    };
    const onKeyUp = (e: KeyboardEvent) => {
      if (isComposingKey(e)) return;
      // NOTE: the gate is applied to keyup too, and that is a deliberate
      // risk. If focus moves between a key's down and its up, the guest
      // sees a down with no up and treats the key as held. The alternative
      // — always forwarding keyup — leaks every release of every key typed
      // into the client's own UI, and the guest's dispatcher tracks held
      // modifiers from these events. main.ts's policy therefore keeps the
      // gate STABLE for the life of a keypress (it keys on where focus is,
      // and focus does not move mid-keypress without a click or a Tab,
      // both of which end the keypress anyway).
      if (!shouldForwardKey(e)) return;
      this.sendKeyUp(e.code, e.key, modsFromEvent(e));
    };

    const onCompStart = (e: CompositionEvent) => {
      this.composing = true;
      // Compute a caret rectangle so the server-side bridge can
      // optionally surface a candidate-positioning hint. Best-
      // effort — Selection.getRangeAt may throw if the focused
      // element isn't editable in the conventional sense.
      let rect: CompositionRect | undefined;
      try {
        const sel = (extra.window ?? globalThis.window).getSelection?.();
        if (sel && sel.rangeCount > 0) {
          const r = sel.getRangeAt(0).getBoundingClientRect();
          if (r && (r.width > 0 || r.height > 0)) {
            const tr = target.getBoundingClientRect();
            const c = map(r.left, r.top, tr);
            rect = {
              x: Math.round(c.x),
              y: Math.round(c.y),
              w: Math.round(r.width),
              h: Math.round(r.height),
            };
          }
        }
      } catch { /* ignore — best-effort */ }
      const data: CompositionData = { data: e.data ?? "" };
      if (rect) data.rect = rect;
      this.sendComposition("start", data);
    };
    const onCompUpdate = (e: CompositionEvent) => {
      const text = e.data ?? "";
      const data: CompositionData = { data: text };
      // Selection within the composing string. The Web platform
      // doesn't expose IME caret position directly; we use the
      // active document selection's offsets if they fall inside
      // `text`. Otherwise we collapse the caret at the end of the
      // composing string (matches v1.0 behaviour and CDP's
      // caret-at-end default).
      const sel = (extra.window ?? globalThis.window).getSelection?.();
      let placed = false;
      if (sel && sel.rangeCount > 0) {
        const r = sel.getRangeAt(0);
        // r.startOffset / r.endOffset are within the text node —
        // we can't always tell whether they're within the composing
        // span vs the surrounding content. Best-effort: when both
        // offsets are within [0, text.length], use them; otherwise
        // fall back to caret-at-end.
        if (r.startOffset >= 0 && r.startOffset <= text.length
         && r.endOffset   >= 0 && r.endOffset   <= text.length) {
          data.selection_start = r.startOffset;
          data.selection_end   = r.endOffset;
          placed = true;
        }
      }
      if (!placed) {
        data.selection_start = text.length;
        data.selection_end   = text.length;
      }
      this.sendComposition("update", data);
    };
    const onCompEnd = (e: CompositionEvent) => {
      this.composing = false;
      const text = e.data ?? "";
      // Empty compositionend == cancel per the protocol's
      // "Detecting cancel from the DOM" rule.
      if (text.length === 0) {
        this.sendCompositionCancel();
      } else {
        this.sendComposition("end", { data: text });
      }
    };
    // NO paste listener here, deliberately.
    //
    // Every paste used to be sent TWICE and consumed ZERO times: this
    // listener put a `clipboard_paste` envelope on the INPUT channel while
    // main.ts's ClipboardChannel put a `clipboard_offer` on the CLIPBOARD
    // channel. The guest lists clipboard_paste in kKnownInputTypes — so it
    // is not even logged as unknown — and then nothing anywhere handles it
    // (cb_input_dispatch_clipboard.h says so explicitly: "Handling
    // clipboard_paste ... we leave the dispatch surface unclaimed here").
    // The offer on the clipboard channel is the one that works.
    //
    // The envelope stays in the v1 spec and `sendClipboardPaste` stays on
    // this class for anyone driving the channel directly; what is removed
    // is this client wiring a browser event to a path that goes nowhere.
    const onCopy = (_e: ClipboardEvent) => { this.sendClipboardCopyRequest(); };

    // ----- v1.1 drag-and-drop -----
    //
    // We listen on the target for the four DataTransfer-bearing
    // events. Per the protocol's state machine:
    //   dragenter (or dragstart) → drag_start envelope (carries items)
    //   dragover                 → drag_over envelope (coalesced)
    //   drop                     → drop envelope (re-asserts items)
    //   dragend / dragleave (when drag exits the target with no drop)
    //                            → drag_end envelope
    //
    // We must call e.preventDefault() in dragover for drop to fire on
    // the target at all — that's a DOM quirk, not a protocol detail.
    let dragInFlight = false;

    const onDragEnter = (e: DragEvent) => {
      const { x, y } = map(e.clientX, e.clientY, target.getBoundingClientRect());
      const { types, items } = extractDragItems(e.dataTransfer);
      this.sendDragStart(x, y, types, items);
      dragInFlight = true;
      e.preventDefault();
    };
    const onDragOver = (e: DragEvent) => {
      // preventDefault is required for drop to actually fire.
      e.preventDefault();
      if (!dragInFlight) return;
      const { x, y } = map(e.clientX, e.clientY, target.getBoundingClientRect());
      this.sendDragOver(x, y);
    };
    const onDrop = (e: DragEvent) => {
      const { x, y } = map(e.clientX, e.clientY, target.getBoundingClientRect());
      const { types, items } = extractDragItems(e.dataTransfer);
      this.sendDrop(x, y, types, items);
      // Per the protocol: client SHOULD send drag_end success:true after drop.
      this.sendDragEnd(true);
      dragInFlight = false;
      e.preventDefault();
    };
    const onDragLeave = (_e: DragEvent) => {
      // Drag exited the target without dropping. We can't reliably
      // distinguish "moved to a child element" from "left for good"
      // synchronously, so we only send drag_end on a true cancel
      // (dragend on the document). dragleave handler is kept as a
      // hook for Phase 2 instrumentation.
    };
    const onDragEnd = (e: DragEvent) => {
      if (!dragInFlight) return;
      // dataTransfer.dropEffect === "none" means cancel; otherwise the
      // drop already fired and we should report success.
      const success = e.dataTransfer?.dropEffect !== "none";
      this.sendDragEnd(success);
      dragInFlight = false;
    };
    // Suppress dragstart bubbling from inside the target — we don't
    // want our own selection-drag to trigger a dragenter before the
    // user has actually crossed the boundary. Phase 2 may extend
    // this for in-page drags initiated *from* the cloud Chromium.

    // ----- v1.1 multi-touch -----
    //
    // Each TouchEvent fires with .changedTouches representing the
    // fingers whose state changed in this event. We translate per
    // finger to one envelope per identifier, matching the protocol's
    // "one envelope per finger per event" rule. preventDefault on
    // touch events suppresses the synthesized mouse events that
    // would otherwise fire — important so the bridge doesn't see
    // both touch_* and mouse_* envelopes for the same gesture.
    const onTouchStart = (e: TouchEvent) => {
      const rect = target.getBoundingClientRect();
      for (const t of Array.from(e.changedTouches)) {
        const { x, y } = map(t.clientX, t.clientY, rect);
        this.sendTouchStart({
          identifier: t.identifier, x, y,
          radius_x: t.radiusX || 1, radius_y: t.radiusY || 1,
          force: t.force || 0, twist: t.rotationAngle || 0,
        });
      }
      e.preventDefault();
    };
    const onTouchMove = (e: TouchEvent) => {
      const rect = target.getBoundingClientRect();
      for (const t of Array.from(e.changedTouches)) {
        const { x, y } = map(t.clientX, t.clientY, rect);
        this.sendTouchMove({
          identifier: t.identifier, x, y,
          radius_x: t.radiusX || 1, radius_y: t.radiusY || 1,
          force: t.force || 0, twist: t.rotationAngle || 0,
        });
      }
      e.preventDefault();
    };
    const onTouchEnd = (e: TouchEvent) => {
      for (const t of Array.from(e.changedTouches)) {
        this.sendTouchEnd(t.identifier);
      }
      e.preventDefault();
    };
    const onTouchCancel = (e: TouchEvent) => {
      for (const t of Array.from(e.changedTouches)) {
        this.sendTouchCancel(t.identifier);
      }
      // No preventDefault on cancel — the system has already
      // unilaterally claimed the gesture.
    };

    target.addEventListener("mousemove", onMouseMove);
    target.addEventListener("mousedown", onMouseDown);
    target.addEventListener("mouseup", onMouseUp);
    target.addEventListener("wheel", onWheel, { passive: false });
    target.addEventListener("contextmenu", onContextMenu);
    win.addEventListener("keydown", onKeyDown);
    win.addEventListener("keyup", onKeyUp);
    target.addEventListener("compositionstart", onCompStart as EventListener);
    target.addEventListener("compositionupdate", onCompUpdate as EventListener);
    target.addEventListener("compositionend", onCompEnd as EventListener);
    // `copy` only — see the note above `onCopy` for why there is no paste
    // listener. clipboard_copy_request IS consumed (it arms the guest's
    // copy window, cb_input_dispatch_composite.cc:77), so this one stays.
    win.addEventListener("copy", onCopy);
    target.addEventListener("dragenter", onDragEnter as EventListener);
    target.addEventListener("dragover", onDragOver as EventListener);
    target.addEventListener("drop", onDrop as EventListener);
    target.addEventListener("dragleave", onDragLeave as EventListener);
    target.addEventListener("dragend", onDragEnd as EventListener);
    // Touch listeners — passive:false because we call preventDefault
    // to suppress synthesized mouse events.
    target.addEventListener("touchstart", onTouchStart as EventListener, { passive: false });
    target.addEventListener("touchmove",  onTouchMove  as EventListener, { passive: false });
    target.addEventListener("touchend",   onTouchEnd   as EventListener, { passive: false });
    target.addEventListener("touchcancel", onTouchCancel as EventListener);

    return () => {
      target.removeEventListener("mousemove", onMouseMove);
      target.removeEventListener("mousedown", onMouseDown);
      target.removeEventListener("mouseup", onMouseUp);
      target.removeEventListener("wheel", onWheel);
      target.removeEventListener("contextmenu", onContextMenu);
      win.removeEventListener("keydown", onKeyDown);
      win.removeEventListener("keyup", onKeyUp);
      target.removeEventListener("compositionstart", onCompStart as EventListener);
      target.removeEventListener("compositionupdate", onCompUpdate as EventListener);
      target.removeEventListener("compositionend", onCompEnd as EventListener);
      win.removeEventListener("copy", onCopy);
      target.removeEventListener("dragenter", onDragEnter as EventListener);
      target.removeEventListener("dragover", onDragOver as EventListener);
      target.removeEventListener("drop", onDrop as EventListener);
      target.removeEventListener("dragleave", onDragLeave as EventListener);
      target.removeEventListener("dragend", onDragEnd as EventListener);
      target.removeEventListener("touchstart", onTouchStart as EventListener);
      target.removeEventListener("touchmove",  onTouchMove  as EventListener);
      target.removeEventListener("touchend",   onTouchEnd   as EventListener);
      target.removeEventListener("touchcancel", onTouchCancel as EventListener);
    };
  }

  // ----- internals -----

  private enqueue(type: InputType, data: InputData): void {
    this.pendingOther.push({ v: PROTOCOL_VERSION, type, t: this.opts.now(), seq: 0, data });
    this.scheduleFlush();
  }

  private scheduleFlush(): void {
    if (this.rafId !== null) return;
    if (this.opts.raf !== undefined) {
      this.rafId = this.opts.raf(() => { this.rafId = null; this.doFlush(); });
    } else if (typeof globalThis.requestAnimationFrame === "function") {
      this.rafId = globalThis.requestAnimationFrame(() => { this.rafId = null; this.doFlush(); });
    } else {
      // Test environment with no rAF and no override: flush synchronously.
      this.doFlush();
    }
  }

  private doFlush(): void {
    if (this.ch.readyState !== "open") {
      // Drop everything; once closed, queued events are stale.
      this.pendingMove = null;
      this.pendingDragOver = null;
      this.pendingTouchMoves.clear();
      this.pendingOther.length = 0;
      this.droppedSinceLastFlush = 0;
      return;
    }

    // Flush non-coalescable events first, in arrival order.
    const others = this.pendingOther;
    this.pendingOther = [];
    for (const env of others) {
      this.dispatch(env);
    }

    // Then send the latest mouse_move, if any.
    if (this.pendingMove !== null) {
      const env: InputEnvelope = {
        v: PROTOCOL_VERSION,
        type: "mouse_move",
        t: this.opts.now(),
        seq: 0,
        data: this.pendingMove,
      };
      this.pendingMove = null;
      this.dispatch(env);
    }

    // …and the latest drag_over, if any.
    if (this.pendingDragOver !== null) {
      const env: InputEnvelope = {
        v: PROTOCOL_VERSION,
        type: "drag_over",
        t: this.opts.now(),
        seq: 0,
        data: this.pendingDragOver,
      };
      this.pendingDragOver = null;
      this.dispatch(env);
    }

    // …and the latest touch_move per finger, if any. Order is
    // ascending by identifier for determinism (helpful in tests +
    // makes wire traces easier to compare across runs).
    if (this.pendingTouchMoves.size > 0) {
      const ids = Array.from(this.pendingTouchMoves.keys()).sort((a, b) => a - b);
      for (const id of ids) {
        const data = this.pendingTouchMoves.get(id)!;
        this.pendingTouchMoves.delete(id);
        this.dispatch({
          v: PROTOCOL_VERSION, type: "touch_move",
          t: this.opts.now(), seq: 0, data,
        });
      }
    }

    if (this.droppedSinceLastFlush > 0) {
      this.opts.onCoalesce?.(this.droppedSinceLastFlush);
      this.droppedSinceLastFlush = 0;
    }

    // Coalesce-threshold guard: if a single rAF tick could not drain
    // (impossible here since we drained synchronously, but kept for
    // future async impl), report.
    if (this.pendingOther.length > this.opts.coalesceThreshold) {
      this.opts.onCoalesce?.(this.pendingOther.length);
    }
  }

  private dispatch(env: InputEnvelope): void {
    env.seq = this.seq++;
    try {
      this.ch.send(JSON.stringify(env));
    } catch (err) {
      this.opts.onError?.(err, env);
    }
  }
}

/** Map a numeric `WheelEvent.deltaMode` to the protocol's string alias. */
function deltaModeName(mode: 0 | 1 | 2): WheelDeltaMode {
  return mode === 0 ? "pixel" : mode === 1 ? "line" : "page";
}

/** Map DOM MouseEvent.button to protocol button id. */
function domButton(b: number): 0 | 1 | 2 | 3 | 4 {
  // DOM: 0 left, 1 middle, 2 right, 3 back, 4 forward — already matches.
  if (b === 0 || b === 1 || b === 2 || b === 3 || b === 4) return b;
  return 0;
}

/**
 * Extract `types` and `items` arrays from a DataTransfer for the v1.1
 * drag-and-drop payload. v1 file-drag policy: items with kind="file"
 * are emitted with NO `data` field, and the helper logs a console
 * warning so the user understands their file drop didn't transfer.
 *
 * Exported for unit tests.
 */
export function extractDragItems(dt: DataTransfer | null): { types: string[]; items: DragItem[] } {
  if (!dt) return { types: [], items: [] };
  const types = Array.from(dt.types);
  const items: DragItem[] = [];
  if (dt.items && dt.items.length > 0) {
    let warnedAboutFiles = false;
    for (let i = 0; i < dt.items.length; i++) {
      const it = dt.items[i];
      if (!it) continue;
      const mime = (it.type || "").toLowerCase();
      if (it.kind === "string") {
        // dt.getData reads from the "drag data store"; it works
        // synchronously in drop and dragstart, returns "" otherwise.
        // That's fine — empty strings still tell the server which
        // MIME types were available.
        const data = dt.getData(mime);
        items.push({ kind: "string", type: mime, data });
      } else if (it.kind === "file") {
        items.push({ kind: "file", type: mime });
        if (!warnedAboutFiles) {
          // eslint-disable-next-line no-console
          console.warn(
            "input.ts: file drags are not transmitted in v1; " +
            "the file MIME type is forwarded as metadata only.",
          );
          warnedAboutFiles = true;
        }
      }
    }
    return { types, items };
  }
  // No DataTransferItemList — fall back to dt.types + dt.getData per type.
  for (const t of types) {
    const data = dt.getData(t);
    items.push({ kind: "string", type: t.toLowerCase(), data });
  }
  return { types, items };
}

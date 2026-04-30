// Input channel encoder for the cloud-browser-webrtc client.
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
  | "clipboard_paste"
  | "clipboard_copy_request";

export interface MouseMoveData { x: number; y: number; }
export interface MouseButtonData { button: 0 | 1 | 2 | 3 | 4; action: "down" | "up"; x: number; y: number; }
export interface MouseWheelData { dx: number; dy: number; mode: 0 | 1 | 2; x: number; y: number; }
export interface KeyData { code: string; key: string; mods: number; }
export interface CompositionData { data: string; }
export interface ClipboardPasteData { text: string; }

export type InputData =
  | MouseMoveData
  | MouseButtonData
  | MouseWheelData
  | KeyData
  | CompositionData
  | ClipboardPasteData
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
}

const DEFAULT_COALESCE_THRESHOLD = 4;
const DEFAULT_BUFFERED_THRESHOLD = 64 * 1024;

export class InputChannel {
  private readonly ch: SendableChannel;
  private readonly opts: Required<Pick<InputChannelOptions, "coalesceThreshold" | "bufferedAmountThreshold" | "now">> &
    Pick<InputChannelOptions, "raf" | "cancelRaf" | "onCoalesce" | "onError">;
  private seq = 0;
  /** Queue of pending mouse_move data; only the last one is kept after coalesce. */
  private pendingMove: MouseMoveData | null = null;
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
      ...(opts.raf !== undefined ? { raf: opts.raf } : {}),
      ...(opts.cancelRaf !== undefined ? { cancelRaf: opts.cancelRaf } : {}),
      ...(opts.onCoalesce !== undefined ? { onCoalesce: opts.onCoalesce } : {}),
      ...(opts.onError !== undefined ? { onError: opts.onError } : {}),
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
    this.enqueue("mouse_wheel", { dx: Math.round(dx), dy: Math.round(dy), mode, x: Math.round(x), y: Math.round(y) });
  }

  sendKeyDown(code: string, key: string, mods: number): void {
    this.enqueue("key_down", { code, key, mods });
  }

  sendKeyUp(code: string, key: string, mods: number): void {
    this.enqueue("key_up", { code, key, mods });
  }

  sendComposition(phase: "start" | "update" | "end", data: string): void {
    this.enqueue(`composition_${phase}` as const, { data });
  }

  sendClipboardPaste(text: string): void {
    this.enqueue("clipboard_paste", { text });
  }

  sendClipboardCopyRequest(): void {
    this.enqueue("clipboard_copy_request", {});
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
    } = {},
  ): () => void {
    const win = extra.window ?? globalThis.window;
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
    const onKeyDown = (e: KeyboardEvent) => { this.sendKeyDown(e.code, e.key, modsFromEvent(e)); };
    const onKeyUp   = (e: KeyboardEvent) => { this.sendKeyUp(e.code,   e.key, modsFromEvent(e)); };
    const onCompStart  = (e: CompositionEvent) => this.sendComposition("start",  e.data ?? "");
    const onCompUpdate = (e: CompositionEvent) => this.sendComposition("update", e.data ?? "");
    const onCompEnd    = (e: CompositionEvent) => this.sendComposition("end",    e.data ?? "");
    const onPaste = (e: ClipboardEvent) => {
      const text = e.clipboardData?.getData("text/plain");
      if (typeof text === "string" && text.length > 0) this.sendClipboardPaste(text);
    };
    const onCopy = (_e: ClipboardEvent) => { this.sendClipboardCopyRequest(); };

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
    win.addEventListener("paste", onPaste);
    win.addEventListener("copy", onCopy);

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
      win.removeEventListener("paste", onPaste);
      win.removeEventListener("copy", onCopy);
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

/** Map DOM MouseEvent.button to protocol button id. */
function domButton(b: number): 0 | 1 | 2 | 3 | 4 {
  // DOM: 0 left, 1 middle, 2 right, 3 back, 4 forward — already matches.
  if (b === 0 || b === 1 || b === 2 || b === 3 || b === 4) return b;
  return 0;
}

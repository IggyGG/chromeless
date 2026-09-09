// Reconnecting WebSocket + peer-connection lifecycle helpers.
//
// Today's client (T34) opens a single signaling websocket and a single
// RTCPeerConnection for the lifetime of the page. If the signaling
// server restarts or the network blips, the user has to reload. T37
// makes that recovery automatic.
//
// What this module does:
//   1. `ReconnectingWebSocket` — wraps a WebSocket with exponential
//      backoff (1s, 2s, 4s, …, capped at 30s), preserving the session
//      URL, surfacing a coarse state machine (idle | connecting |
//      connected | reconnecting | failed | closed) to subscribers.
//   2. `requestIceRecovery(ws, role)` — emits the
//      `request_renegotiate` signaling envelope (T37 protocol
//      addition). The peer that owns the offer responds by calling
//      `createOffer({ iceRestart: true })` and sending a fresh offer
//      through signaling. Documented in
//      `docs/protocols/reconnect.md`.
//
// What this module does NOT do:
//   - Construct or drive an RTCPeerConnection. That stays in
//     `main.ts`. We only provide the events; main.ts decides how to
//     rebuild peer state on each reconnect.
//   - Buffer outbound messages while disconnected. Calling `send()`
//     while in any state other than `connected` is a no-op that
//     returns false. Callers should re-emit on the `connected` event.

export type ReconnectState =
  | "idle"
  | "connecting"
  | "connected"
  | "reconnecting"
  | "failed"
  | "closed";

/** Minimal browser-WebSocket subset we depend on. */
export interface RWSocket {
  readonly readyState: number;
  send(data: string): void;
  close(code?: number, reason?: string): void;
  addEventListener(type: "open" | "message" | "close" | "error", listener: (ev: any) => void): void;
  removeEventListener(type: "open" | "message" | "close" | "error", listener: (ev: any) => void): void;
}

export interface RWSocketCtor {
  new (url: string): RWSocket;
  readonly OPEN: number;
}

export interface ReconnectingWebSocketOptions {
  /** WebSocket constructor; defaults to global WebSocket. Inject for tests. */
  webSocket?: RWSocketCtor;
  /** Schedule a callback after `ms`. Defaults to setTimeout. Returns a handle. */
  setTimeout?: (cb: () => void, ms: number) => number;
  /** Cancel a scheduled callback. Defaults to clearTimeout. */
  clearTimeout?: (handle: number) => void;
  /** First retry delay in ms. Default 1000. */
  initialBackoffMs?: number;
  /** Cap on retry delay in ms. Default 30000. */
  maxBackoffMs?: number;
  /** Multiplier per retry. Default 2. */
  backoffFactor?: number;
  /**
   * Maximum number of consecutive failed attempts before transitioning
   * to "failed" and stopping. Default `Infinity` (retry forever until
   * `close()` is called).
   */
  maxAttempts?: number;
}

export interface ReconnectEvents {
  /** Underlying socket transitioned to OPEN. Fired on the very first connect AND every reconnect. */
  open: () => void;
  /** Underlying socket received a frame. */
  message: (ev: MessageEvent) => void;
  /** Underlying socket emitted close. The wrapper may still attempt to reconnect. */
  underlyingClose: (ev: CloseEvent) => void;
  /** State machine transitioned. */
  stateChange: (next: ReconnectState, prev: ReconnectState, info: { attempt: number; nextDelayMs: number }) => void;
}

type Listener<T> = T extends (...args: infer A) => void ? (...args: A) => void : never;

/**
 * Robust signaling websocket wrapper. Provides the same event surface
 * as a raw WebSocket plus a coarser `stateChange` event for the UI.
 */
export class ReconnectingWebSocket {
  // A SUPPLIER, not a string. The signaling URL carries a short-TTL auth
  // token (`?token=…`, auth.ts), and a URL frozen at construction is frozen
  // WITH that token: after a 15-minute TTL every redial presents an expired
  // credential, which the broker rejects at the handshake. That is
  // indistinguishable from "the broker is down" and gets worse the longer
  // the session lives — precisely when reconnecting matters most.
  //
  // A plain string is still accepted and wrapped, so every existing caller
  // and test is unchanged.
  private readonly urlFor: () => string;
  private readonly opts: Required<Omit<ReconnectingWebSocketOptions, "webSocket" | "setTimeout" | "clearTimeout">> & {
    WebSocket: RWSocketCtor;
    setTimeout: (cb: () => void, ms: number) => number;
    clearTimeout: (handle: number) => void;
  };
  private socket: RWSocket | null = null;
  private state: ReconnectState = "idle";
  private attempt = 0;
  private nextDelay = 0;
  private retryHandle: number | null = null;
  private listeners: { [K in keyof ReconnectEvents]: Set<Listener<ReconnectEvents[K]>> } = {
    open: new Set(),
    message: new Set(),
    underlyingClose: new Set(),
    stateChange: new Set(),
  };
  /** True after `close()` is invoked; suppresses further reconnects. */
  private permanentlyClosed = false;

  constructor(url: string | (() => string),
              options: ReconnectingWebSocketOptions = {}) {
    this.urlFor = typeof url === "function" ? url : () => url;
    const ws =
      options.webSocket ??
      (globalThis as unknown as { WebSocket?: RWSocketCtor }).WebSocket;
    if (!ws) throw new Error("No WebSocket constructor available; pass options.webSocket");
    const st =
      options.setTimeout ??
      ((cb: () => void, ms: number) => globalThis.setTimeout(cb, ms) as unknown as number);
    const ct =
      options.clearTimeout ??
      ((h: number) => globalThis.clearTimeout(h));
    this.opts = {
      WebSocket: ws,
      setTimeout: st,
      clearTimeout: ct,
      initialBackoffMs: options.initialBackoffMs ?? 1000,
      maxBackoffMs: options.maxBackoffMs ?? 30000,
      backoffFactor: options.backoffFactor ?? 2,
      maxAttempts: options.maxAttempts ?? Number.POSITIVE_INFINITY,
    };
  }

  /** Coarse state — see ReconnectState. */
  getState(): ReconnectState { return this.state; }

  /** True when the underlying WebSocket is open. */
  isConnected(): boolean { return this.state === "connected"; }

  /** Number of times we've attempted to (re)connect since the last success. */
  getAttempt(): number { return this.attempt; }

  on<K extends keyof ReconnectEvents>(event: K, listener: Listener<ReconnectEvents[K]>): () => void {
    this.listeners[event].add(listener);
    return () => { this.listeners[event].delete(listener); };
  }

  /** Begin connecting (idempotent). */
  connect(): void {
    if (this.permanentlyClosed) return;
    if (this.state === "connecting" || this.state === "connected") return;
    this.transition(this.attempt > 0 ? "reconnecting" : "connecting");
    this.openSocket();
  }

  /**
   * Send a frame on the underlying socket. Returns false if not
   * currently connected (callers should re-emit when `connected`).
   */
  send(data: string): boolean {
    if (!this.socket || this.state !== "connected") return false;
    if (this.socket.readyState !== this.opts.WebSocket.OPEN) return false;
    try {
      this.socket.send(data);
      return true;
    } catch {
      return false;
    }
  }

  /** Permanently close. No further reconnects. */
  close(code?: number, reason?: string): void {
    this.permanentlyClosed = true;
    if (this.retryHandle !== null) {
      this.opts.clearTimeout(this.retryHandle);
      this.retryHandle = null;
    }
    if (this.socket) {
      try { this.socket.close(code, reason); } catch { /* ignore */ }
      this.socket = null;
    }
    this.transition("closed");
  }

  // ---------- internals ----------

  private openSocket(): void {
    let s: RWSocket;
    try {
      // Resolved per dial, so a token refreshed since the last attempt is
      // the one presented.
      s = new this.opts.WebSocket(this.urlFor());
    } catch (err) {
      // Synchronous failure (rare; mostly bad URL). Treat as a closed socket.
      this.scheduleRetry();
      return;
    }
    this.socket = s;
    const onOpen = () => {
      this.attempt = 0;
      this.nextDelay = 0;
      this.transition("connected");
      for (const l of this.listeners.open) l();
    };
    const onMessage = (ev: MessageEvent) => {
      for (const l of this.listeners.message) l(ev);
    };
    const onClose = (ev: CloseEvent) => {
      for (const l of this.listeners.underlyingClose) l(ev);
      cleanup();
      this.socket = null;
      if (this.permanentlyClosed) return;
      this.scheduleRetry();
    };
    const onError = (_ev: Event) => {
      // Errors precede close on most WebSocket impls; close handles teardown.
    };
    s.addEventListener("open", onOpen);
    s.addEventListener("message", onMessage);
    s.addEventListener("close", onClose);
    s.addEventListener("error", onError);
    const cleanup = () => {
      s.removeEventListener("open", onOpen);
      s.removeEventListener("message", onMessage);
      s.removeEventListener("close", onClose);
      s.removeEventListener("error", onError);
    };
  }

  private scheduleRetry(): void {
    this.attempt += 1;
    if (this.attempt >= this.opts.maxAttempts) {
      this.transition("failed");
      return;
    }
    const base = this.opts.initialBackoffMs * Math.pow(this.opts.backoffFactor, this.attempt - 1);
    this.nextDelay = Math.min(base, this.opts.maxBackoffMs);
    this.transition("reconnecting");
    this.retryHandle = this.opts.setTimeout(() => {
      this.retryHandle = null;
      if (this.permanentlyClosed) return;
      this.openSocket();
    }, this.nextDelay);
  }

  private transition(next: ReconnectState): void {
    if (this.state === next) return;
    const prev = this.state;
    this.state = next;
    for (const l of this.listeners.stateChange) {
      l(next, prev, { attempt: this.attempt, nextDelayMs: this.nextDelay });
    }
  }
}

/**
 * Send the `request_renegotiate` envelope to ask the offerer to
 * `createOffer({ iceRestart: true })` and resume the session through
 * signaling. The signaling server forwards this verbatim per the
 * envelope rules in `docs/protocols/reconnect.md`.
 */
export function requestIceRecovery(
  send: (frame: string) => boolean,
  role: "client" | "browser",
): boolean {
  return send(JSON.stringify({ type: "request_renegotiate", from: role, data: null }));
}

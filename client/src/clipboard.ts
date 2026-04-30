// Bidirectional clipboard sync for the cloud-browser-webrtc client.
//
// Wraps the "clipboard" RTCDataChannel (separate from "input"). Wire
// format: docs/protocols/clipboard-channel.md (v1).
//
// Two halves:
//   - Outgoing (client→cloud): listen for `paste` events on a target
//     element (typically the document, or the streamed video stage),
//     read text from ClipboardEvent.clipboardData, send one envelope.
//   - Incoming (cloud→client): for each envelope received from the
//     channel, write text to navigator.clipboard.writeText() — gated
//     on document.hasFocus(); queued otherwise and flushed on the next
//     focus event.
//
// Strict v1 stance per the protocol:
//   - text only;
//   - 1 MiB cap on both directions;
//   - never silently polls the clipboard;
//   - drops envelopes with unknown `source`.

export const PROTOCOL_VERSION = 1 as const;
export const MAX_BYTES = 1_048_576; // 1 MiB

export type ClipboardDirection = "client->cloud" | "cloud->client";
export type ClipboardSource = "user_action";

export interface ClipboardData {
  direction: ClipboardDirection;
  source: ClipboardSource;
  text: string;
}

export interface ClipboardEnvelope {
  v: typeof PROTOCOL_VERSION;
  type: "clipboard_offer";
  t: number;
  seq: number;
  data: ClipboardData;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

export function isValidEnvelope(env: unknown): env is ClipboardEnvelope {
  if (!env || typeof env !== "object") return false;
  const e = env as Partial<ClipboardEnvelope>;
  if (e.v !== PROTOCOL_VERSION) return false;
  if (e.type !== "clipboard_offer") return false;
  if (typeof e.t !== "number" || typeof e.seq !== "number") return false;
  const d = e.data;
  if (!d || typeof d !== "object") return false;
  if (d.direction !== "client->cloud" && d.direction !== "cloud->client") return false;
  if (d.source !== "user_action") return false;
  if (typeof d.text !== "string") return false;
  if (utf8ByteLength(d.text) > MAX_BYTES) return false;
  return true;
}

// UTF-8 byte length without allocating a TextEncoder per call.
const _enc = typeof TextEncoder !== "undefined" ? new TextEncoder() : null;
export function utf8ByteLength(s: string): number {
  if (_enc) return _enc.encode(s).length;
  // Test-environment fallback (no TextEncoder).
  let n = 0;
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c < 0x80) n += 1;
    else if (c < 0x800) n += 2;
    else if (c >= 0xd800 && c <= 0xdbff) { n += 4; i++; }
    else n += 3;
  }
  return n;
}

// ---------------------------------------------------------------------------
// Channel surface — minimal subset for testability
// ---------------------------------------------------------------------------

export interface ClipboardChannelSurface {
  readonly readyState: "connecting" | "open" | "closing" | "closed";
  send(data: string): void;
  addEventListener(type: "message", listener: (e: { data: string }) => void): void;
  addEventListener(type: "open" | "close", listener: () => void): void;
}

export interface ClipboardOptions {
  /** Override clipboard write API for tests. */
  writeText?: (text: string) => Promise<void>;
  /** Override focus check for tests. Defaults to () => document.hasFocus(). */
  hasFocus?: () => boolean;
  /** Wall-clock supplier (override for tests). Defaults to Date.now. */
  now?: () => number;
  /** Called when an inbound envelope is dropped because of validation. */
  onDropped?: (reason: string, env: unknown) => void;
  /** Called when an outbound paste exceeds the 1 MiB cap. */
  onOversize?: (bytes: number) => void;
}

// ---------------------------------------------------------------------------
// ClipboardChannel
// ---------------------------------------------------------------------------

export class ClipboardChannel {
  private seq = 0;
  private pendingWrite: string | null = null;
  private focusListenerInstalled = false;
  private readonly opts: Required<Pick<ClipboardOptions, "writeText" | "hasFocus" | "now">> &
    Pick<ClipboardOptions, "onDropped" | "onOversize">;

  constructor(private readonly ch: ClipboardChannelSurface, opts: ClipboardOptions = {}) {
    this.opts = {
      writeText: opts.writeText ?? defaultWriteText,
      hasFocus: opts.hasFocus ?? defaultHasFocus,
      now: opts.now ?? Date.now,
      ...(opts.onDropped !== undefined ? { onDropped: opts.onDropped } : {}),
      ...(opts.onOversize !== undefined ? { onOversize: opts.onOversize } : {}),
    };
    ch.addEventListener("message", (e) => this.onMessage(e.data));
  }

  /** Send a client→cloud paste. Returns true if sent, false if rejected. */
  sendPaste(text: string): boolean {
    const bytes = utf8ByteLength(text);
    if (bytes > MAX_BYTES) {
      this.opts.onOversize?.(bytes);
      return false;
    }
    if (this.ch.readyState !== "open") return false;
    const env: ClipboardEnvelope = {
      v: PROTOCOL_VERSION,
      type: "clipboard_offer",
      t: this.opts.now(),
      seq: this.seq++,
      data: { direction: "client->cloud", source: "user_action", text },
    };
    try {
      this.ch.send(JSON.stringify(env));
      return true;
    } catch {
      return false;
    }
  }

  /** Wire DOM listeners onto a target. Returns a detach function. */
  attach(
    target: { addEventListener: (t: "paste", h: (e: ClipboardEvent) => void) => void;
              removeEventListener: (t: "paste", h: (e: ClipboardEvent) => void) => void },
  ): () => void {
    const onPaste = (e: ClipboardEvent) => {
      const text = e.clipboardData?.getData("text/plain");
      if (typeof text === "string" && text.length > 0) {
        this.sendPaste(text);
      }
    };
    target.addEventListener("paste", onPaste);
    return () => target.removeEventListener("paste", onPaste);
  }

  // ----- internal: incoming -----

  private onMessage(raw: string): void {
    let env: unknown;
    try {
      env = JSON.parse(raw);
    } catch {
      this.opts.onDropped?.("invalid JSON", raw);
      return;
    }
    if (!isValidEnvelope(env)) {
      this.opts.onDropped?.("validation", env);
      return;
    }
    if (env.data.direction !== "cloud->client") {
      // We never act on echoes of our own client->cloud envelopes
      // (per the protocol's "no echo loop" rule). Drop silently.
      return;
    }
    this.queueWrite(env.data.text);
  }

  private queueWrite(text: string): void {
    if (this.opts.hasFocus()) {
      void this.opts.writeText(text);
      return;
    }
    // Document not focused — queue and flush on the next focus event.
    this.pendingWrite = text;
    if (!this.focusListenerInstalled && typeof globalThis.window !== "undefined") {
      this.focusListenerInstalled = true;
      globalThis.window.addEventListener("focus", () => {
        const t = this.pendingWrite;
        this.pendingWrite = null;
        if (t !== null) void this.opts.writeText(t);
      }, { once: true });
    }
  }

  /** For tests: forcibly flush a pending queued write. */
  flushPending(): void {
    const t = this.pendingWrite;
    this.pendingWrite = null;
    if (t !== null) void this.opts.writeText(t);
  }
}

function defaultWriteText(text: string): Promise<void> {
  if (typeof navigator === "undefined" || !navigator.clipboard) {
    return Promise.reject(new Error("navigator.clipboard unavailable"));
  }
  return navigator.clipboard.writeText(text);
}

function defaultHasFocus(): boolean {
  if (typeof document === "undefined") return true;
  return document.hasFocus();
}

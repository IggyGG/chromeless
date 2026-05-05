// File-upload encoder for the chromeless client.
//
// Wraps the "files" RTCDataChannel and chunks a File / Blob into
// envelope-delimited messages per docs/protocols/file-upload.md (v1).
//
// Multiple uploads can run concurrently — they're demultiplexed by
// upload_id. Each call to `uploadFile()` returns a Promise that
// resolves with the server_path on success or rejects with a
// FileUploadError on failure.
//
// Backpressure: respects RTCDataChannel.bufferedAmount with a high-
// water mark and resumes on `bufferedamountlow` events.
//
// Strict v1 stance per the protocol:
//   - 100 MiB hard cap on size;
//   - 1 MiB hard cap per chunk;
//   - SHA-256 of full content is asserted in file_upload_start;
//   - cancel via the returned handle, no silent retries.

export const PROTOCOL_VERSION = 1 as const;

export const DEFAULT_CHUNK_SIZE = 64 * 1024;          // 64 KiB
export const MAX_CHUNK_SIZE     = 1024 * 1024;        // 1 MiB
export const MAX_TOTAL_SIZE     = 100 * 1024 * 1024;  // 100 MiB

export const DEFAULT_BUFFERED_HIGH_WATER = 1024 * 1024; // 1 MiB
export const DEFAULT_BUFFERED_LOW_WATER  = 256  * 1024; // 256 KiB

// ---------------------------------------------------------------------------
// Wire types — match docs/protocols/file-upload.md v1.
// ---------------------------------------------------------------------------

export type FileUploadType =
  | "file_upload_start"
  | "file_upload_chunk"
  | "file_upload_end"
  | "file_upload_cancel"
  // Server → client:
  | "file_upload_progress"
  | "file_upload_complete"
  | "file_upload_error";

export interface FileUploadStartData {
  v: typeof PROTOCOL_VERSION;
  type: "file_upload_start";
  upload_id: string;
  name: string;
  mime_type: string;
  size: number;
  sha256: string;
  target_selector?: string;
}
export interface FileUploadChunkData {
  v: typeof PROTOCOL_VERSION;
  type: "file_upload_chunk";
  upload_id: string;
  seq: number;
  data: string; // base64
}
export interface FileUploadEndData {
  v: typeof PROTOCOL_VERSION;
  type: "file_upload_end";
  upload_id: string;
}
export interface FileUploadCancelData {
  v: typeof PROTOCOL_VERSION;
  type: "file_upload_cancel";
  upload_id: string;
}
export interface FileUploadProgressData {
  v: typeof PROTOCOL_VERSION;
  type: "file_upload_progress";
  upload_id: string;
  bytes_received: number;
  bytes_total: number;
}
export interface FileUploadCompleteData {
  v: typeof PROTOCOL_VERSION;
  type: "file_upload_complete";
  upload_id: string;
  server_path: string;
  attached_via: "fileChooser" | "domSetFileInputFiles" | "none";
}
export interface FileUploadErrorData {
  v: typeof PROTOCOL_VERSION;
  type: "file_upload_error";
  upload_id: string;
  code: string;
  error: string;
}

export type ServerEnvelope =
  | FileUploadProgressData
  | FileUploadCompleteData
  | FileUploadErrorData;

// ---------------------------------------------------------------------------
// FileUploadError — what uploadFile() rejects with.
// ---------------------------------------------------------------------------

export class FileUploadError extends Error {
  constructor(
    public readonly code: string,
    message: string,
    public readonly uploadId?: string,
  ) {
    super(message);
    this.name = "FileUploadError";
  }
}

// ---------------------------------------------------------------------------
// Channel surface — minimal subset for tests.
// ---------------------------------------------------------------------------

export interface FilesChannelSurface {
  readonly readyState: "connecting" | "open" | "closing" | "closed";
  readonly bufferedAmount: number;
  bufferedAmountLowThreshold?: number;
  send(data: string): void;
  addEventListener(type: "message",            listener: (e: { data: string }) => void): void;
  addEventListener(type: "bufferedamountlow",  listener: () => void): void;
  addEventListener(type: "open" | "close",     listener: () => void): void;
}

// ---------------------------------------------------------------------------
// Helpers — UUID, sha256, base64.
// ---------------------------------------------------------------------------

/** Browser-side UUID v4. Falls back to crypto.getRandomValues if
 *  randomUUID isn't available. */
export function newUploadId(): string {
  const c = (globalThis as unknown as { crypto?: Crypto }).crypto;
  if (c && typeof c.randomUUID === "function") return c.randomUUID();
  // Non-cryptographic fallback for old test runtimes.
  const hex = (n: number): string => n.toString(16).padStart(2, "0");
  const bytes = new Uint8Array(16);
  if (c && typeof c.getRandomValues === "function") {
    c.getRandomValues(bytes);
  } else {
    for (let i = 0; i < 16; i++) bytes[i] = Math.floor(Math.random() * 256);
  }
  // Set v4 + RFC4122 variant bits.
  bytes[6] = ((bytes[6] ?? 0) & 0x0f) | 0x40;
  bytes[8] = ((bytes[8] ?? 0) & 0x3f) | 0x80;
  const b = (i: number) => hex(bytes[i] ?? 0);
  return `${b(0)}${b(1)}${b(2)}${b(3)}-${b(4)}${b(5)}-${b(6)}${b(7)}-${b(8)}${b(9)}-${b(10)}${b(11)}${b(12)}${b(13)}${b(14)}${b(15)}`;
}

/** Compute SHA-256 of a Blob/File using SubtleCrypto. */
export async function sha256Hex(buf: ArrayBuffer): Promise<string> {
  const c = (globalThis as unknown as { crypto?: Crypto }).crypto;
  if (!c || !c.subtle) {
    throw new FileUploadError("internal_error", "crypto.subtle is unavailable");
  }
  const digest = await c.subtle.digest("SHA-256", buf);
  const view = new Uint8Array(digest);
  let s = "";
  for (let i = 0; i < view.length; i++) {
    s += (view[i] ?? 0).toString(16).padStart(2, "0");
  }
  return s;
}

/** Base64-encode a Uint8Array. Browser-friendly + node-friendly. */
export function bytesToBase64(bytes: Uint8Array): string {
  if (typeof Buffer !== "undefined") {
    return Buffer.from(bytes).toString("base64");
  }
  // Browser path — chunk to avoid String.fromCharCode argument limits.
  let s = "";
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) {
    s += String.fromCharCode(...bytes.subarray(i, i + chunk));
  }
  return globalThis.btoa(s);
}

// ---------------------------------------------------------------------------
// FileUploadChannel
// ---------------------------------------------------------------------------

export interface FileUploadChannelOptions {
  /** Bytes per chunk. Must be ≤ MAX_CHUNK_SIZE. Default 64 KiB. */
  chunkSize?: number;
  /** High-water mark on bufferedAmount above which we pause sending. Default 1 MiB. */
  bufferedHighWater?: number;
  /** RTCDataChannel.bufferedAmountLowThreshold. Default 256 KiB. */
  bufferedLowWater?: number;
  /** Total upload size cap. Default 100 MiB. */
  maxTotalSize?: number;
  /** Override SHA-256 implementation for tests / non-browser runtimes. */
  sha256?: (buf: ArrayBuffer) => Promise<string>;
  /** Override UUID supplier for deterministic tests. */
  newUploadId?: () => string;
}

export interface UploadProgress {
  upload_id: string;
  bytes_sent: number;
  bytes_total: number;
}

export interface UploadOptions {
  /** CSS selector for the <input type=file> to attach the file to on the server. */
  target_selector?: string;
  /** Progress callback fired as bytes are sent (client-side, not server-acked). */
  onProgress?: (p: UploadProgress) => void;
  /** Server-side progress callback fired when file_upload_progress envelopes arrive. */
  onServerProgress?: (env: FileUploadProgressData) => void;
}

export interface UploadHandle {
  upload_id: string;
  /** Promise resolving when the server replies file_upload_complete. */
  done: Promise<{ server_path: string; attached_via: string }>;
  /** Cancel the in-flight upload — sends file_upload_cancel and rejects done. */
  cancel(): void;
}

export class FileUploadChannel {
  private readonly opts: Required<Pick<FileUploadChannelOptions,
      "chunkSize" | "bufferedHighWater" | "bufferedLowWater" | "maxTotalSize">> &
    Pick<FileUploadChannelOptions, "sha256" | "newUploadId">;

  // Per-upload pending-completion state, keyed by upload_id.
  private readonly pending = new Map<string, {
    resolve: (r: { server_path: string; attached_via: string }) => void;
    reject:  (e: FileUploadError) => void;
    onServerProgress?: (env: FileUploadProgressData) => void;
    cancelled: boolean;
  }>();

  // Resolvers waiting for `bufferedamountlow` to drain the send queue.
  private readonly bufferedLowWaiters: Array<() => void> = [];

  constructor(private readonly ch: FilesChannelSurface, opts: FileUploadChannelOptions = {}) {
    this.opts = {
      chunkSize: opts.chunkSize ?? DEFAULT_CHUNK_SIZE,
      bufferedHighWater: opts.bufferedHighWater ?? DEFAULT_BUFFERED_HIGH_WATER,
      bufferedLowWater:  opts.bufferedLowWater  ?? DEFAULT_BUFFERED_LOW_WATER,
      maxTotalSize:      opts.maxTotalSize      ?? MAX_TOTAL_SIZE,
      ...(opts.sha256 !== undefined ? { sha256: opts.sha256 } : {}),
      ...(opts.newUploadId !== undefined ? { newUploadId: opts.newUploadId } : {}),
    };
    if (this.opts.chunkSize > MAX_CHUNK_SIZE) {
      throw new FileUploadError(
        "chunk_too_large",
        `chunkSize ${this.opts.chunkSize} > MAX_CHUNK_SIZE ${MAX_CHUNK_SIZE}`,
      );
    }

    if (typeof ch.bufferedAmountLowThreshold === "number") {
      ch.bufferedAmountLowThreshold = this.opts.bufferedLowWater;
    }

    ch.addEventListener("message", (e) => this.onMessage(e.data));
    ch.addEventListener("bufferedamountlow", () => {
      const w = this.bufferedLowWaiters.splice(0);
      for (const fn of w) fn();
    });
  }

  // ----- public API -----

  /**
   * Upload a File/Blob to the cloud Chromium. Resolves with the
   * server-side path and which attach method was used; rejects with
   * a FileUploadError on any failure.
   */
  uploadFile(file: File | Blob, opts: UploadOptions = {}): UploadHandle {
    const upload_id = (this.opts.newUploadId ?? newUploadId)();
    const name = (file as File).name || "upload.bin";
    const mime_type = file.type || "application/octet-stream";
    const size = file.size;

    if (size > this.opts.maxTotalSize) {
      const err = new FileUploadError(
        "size_limit_exceeded",
        `file size ${size} > maxTotalSize ${this.opts.maxTotalSize}`,
        upload_id,
      );
      return {
        upload_id,
        done: Promise.reject(err),
        cancel: () => { /* no-op; nothing to cancel */ },
      };
    }

    let resolveDone!: (r: { server_path: string; attached_via: string }) => void;
    let rejectDone!:  (e: FileUploadError) => void;
    const done = new Promise<{ server_path: string; attached_via: string }>(
      (res, rej) => { resolveDone = res; rejectDone = rej; },
    );
    this.pending.set(upload_id, {
      resolve: resolveDone, reject: rejectDone,
      ...(opts.onServerProgress !== undefined ? { onServerProgress: opts.onServerProgress } : {}),
      cancelled: false,
    });

    void this.driveUpload(upload_id, file, name, mime_type, size, opts);

    return {
      upload_id,
      done,
      cancel: () => this.cancel(upload_id),
    };
  }

  cancel(upload_id: string): void {
    const p = this.pending.get(upload_id);
    if (!p) return;
    p.cancelled = true;
    if (this.ch.readyState === "open") {
      try {
        this.ch.send(JSON.stringify({
          v: PROTOCOL_VERSION, type: "file_upload_cancel", upload_id,
        }));
      } catch { /* swallow; the bridge will time out the upload */ }
    }
    p.reject(new FileUploadError("cancelled", "upload cancelled by client", upload_id));
    this.pending.delete(upload_id);
  }

  // ----- internal: send loop -----

  private async driveUpload(
    upload_id: string, file: File | Blob,
    name: string, mime_type: string, size: number,
    opts: UploadOptions,
  ): Promise<void> {
    try {
      const buf = await file.arrayBuffer();
      const sha256 = await (this.opts.sha256 ?? sha256Hex)(buf);

      // Upload may have been cancelled while we were hashing.
      const state0 = this.pending.get(upload_id);
      if (!state0 || state0.cancelled) return;

      const startEnv: FileUploadStartData = {
        v: PROTOCOL_VERSION, type: "file_upload_start",
        upload_id, name, mime_type, size, sha256,
        ...(opts.target_selector !== undefined ? { target_selector: opts.target_selector } : {}),
      };
      await this.sendOrThrow(JSON.stringify(startEnv));

      const view = new Uint8Array(buf);
      const cs = this.opts.chunkSize;
      let seq = 0;
      let bytes_sent = 0;

      for (let off = 0; off < view.length; off += cs) {
        const state = this.pending.get(upload_id);
        if (!state || state.cancelled) return;

        await this.waitIfBuffered();

        const slice = view.subarray(off, Math.min(off + cs, view.length));
        const chunk: FileUploadChunkData = {
          v: PROTOCOL_VERSION, type: "file_upload_chunk",
          upload_id, seq, data: bytesToBase64(slice),
        };
        await this.sendOrThrow(JSON.stringify(chunk));
        seq += 1;
        bytes_sent += slice.length;
        opts.onProgress?.({ upload_id, bytes_sent, bytes_total: size });
      }

      const endEnv: FileUploadEndData = {
        v: PROTOCOL_VERSION, type: "file_upload_end", upload_id,
      };
      await this.sendOrThrow(JSON.stringify(endEnv));
      // Resolution / rejection happens in onMessage when the server
      // replies file_upload_complete or file_upload_error.
    } catch (err) {
      const p = this.pending.get(upload_id);
      if (!p) return;
      this.pending.delete(upload_id);
      const fe = err instanceof FileUploadError
        ? err
        : new FileUploadError("internal_error", String((err as Error)?.message ?? err), upload_id);
      p.reject(fe);
    }
  }

  /** Send `s`, or throw a FileUploadError if the channel isn't open or send fails. */
  private async sendOrThrow(s: string): Promise<void> {
    if (this.ch.readyState !== "open") {
      throw new FileUploadError("internal_error", `channel not open: ${this.ch.readyState}`);
    }
    try {
      this.ch.send(s);
    } catch (err) {
      throw new FileUploadError("internal_error", String((err as Error)?.message ?? err));
    }
  }

  /** Block until bufferedAmount drops below the high-water mark. */
  private waitIfBuffered(): Promise<void> {
    if (this.ch.bufferedAmount < this.opts.bufferedHighWater) return Promise.resolve();
    return new Promise<void>((resolve) => {
      this.bufferedLowWaiters.push(resolve);
    });
  }

  // ----- internal: incoming -----

  private onMessage(raw: string): void {
    let env: ServerEnvelope;
    try {
      env = JSON.parse(raw) as ServerEnvelope;
    } catch {
      return;
    }
    if (env.v !== PROTOCOL_VERSION) return;
    const p = this.pending.get(env.upload_id);
    if (!p) return;
    switch (env.type) {
      case "file_upload_progress":
        p.onServerProgress?.(env);
        return;
      case "file_upload_complete":
        this.pending.delete(env.upload_id);
        p.resolve({ server_path: env.server_path, attached_via: env.attached_via });
        return;
      case "file_upload_error":
        this.pending.delete(env.upload_id);
        p.reject(new FileUploadError(env.code, env.error, env.upload_id));
        return;
    }
  }
}

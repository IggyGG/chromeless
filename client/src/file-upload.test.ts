// Unit tests for client/src/file-upload.ts.
//
// The class talks to an RTCDataChannel-shaped surface via send() and
// addEventListener; we substitute a FakeChannel that records sent
// strings, lets the test push messages, and lets the test fake
// bufferedAmount + bufferedamountlow events for backpressure
// scenarios.

import { describe, it, expect } from "vitest";
import {
  FileUploadChannel,
  FileUploadError,
  PROTOCOL_VERSION,
  bytesToBase64,
  newUploadId,
  MAX_TOTAL_SIZE,
} from "./file-upload.js";

// ---------------------------------------------------------------------------
// Fakes
// ---------------------------------------------------------------------------

class FakeChannel {
  readyState: "connecting" | "open" | "closing" | "closed" = "open";
  bufferedAmount = 0;
  bufferedAmountLowThreshold = 0;
  sent: string[] = [];
  private listeners: Record<string, Array<(...a: unknown[]) => void>> = {};

  send(s: string): void {
    this.sent.push(s);
  }
  addEventListener(t: string, cb: (...a: unknown[]) => void): void {
    (this.listeners[t] ??= []).push(cb);
  }
  pushMessage(s: string): void {
    for (const cb of this.listeners["message"] ?? []) cb({ data: s });
  }
  fireBufferedAmountLow(): void {
    for (const cb of this.listeners["bufferedamountlow"] ?? []) cb();
  }
}

class FakeBlob {
  constructor(private readonly bytes: Uint8Array, public readonly name = "f.bin", public readonly type = "application/octet-stream") {}
  get size(): number { return this.bytes.length; }
  arrayBuffer(): Promise<ArrayBuffer> {
    // Copy the Uint8Array into a plain ArrayBuffer (not SharedArrayBuffer-typed)
    // so the result is assignable to ArrayBuffer everywhere.
    const b = new ArrayBuffer(this.bytes.length);
    new Uint8Array(b).set(this.bytes);
    return Promise.resolve(b);
  }
}

function makeFile(bytes: Uint8Array, name = "test.pdf", type = "application/pdf") {
  return new FakeBlob(bytes, name, type) as unknown as File;
}

const FIXED_SHA = "deadbeef";  // overridden for deterministic tests

function newChannel(opts: Partial<ConstructorParameters<typeof FileUploadChannel>[1]> = {}) {
  const ch = new FakeChannel();
  // The fake's addEventListener uses a permissive `(...a: unknown[])` signature
  // for ergonomics; the real surface is overload-typed. Cast for the
  // constructor — runtime behaviour is identical.
  const fc = new FileUploadChannel(ch as unknown as ConstructorParameters<typeof FileUploadChannel>[0], {
    sha256: () => Promise.resolve(FIXED_SHA),
    newUploadId: () => "u-test",
    ...opts,
  });
  return { ch, fc };
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

describe("bytesToBase64", () => {
  it("encodes ASCII", () => {
    expect(bytesToBase64(new Uint8Array([0x68, 0x69]))).toBe("aGk=");
  });
  it("encodes 0..255 round-trip", () => {
    const a = new Uint8Array(256);
    for (let i = 0; i < 256; i++) a[i] = i;
    const b64 = bytesToBase64(a);
    const back = Buffer.from(b64, "base64");
    expect(back.length).toBe(256);
    for (let i = 0; i < 256; i++) expect(back[i]).toBe(i);
  });
});

describe("newUploadId", () => {
  it("returns a UUID-shaped string", () => {
    const id = newUploadId();
    expect(id).toMatch(/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/);
  });
});

// ---------------------------------------------------------------------------
// uploadFile happy path
// ---------------------------------------------------------------------------

describe("FileUploadChannel.uploadFile happy path", () => {
  it("sends start → chunks → end and resolves on file_upload_complete", async () => {
    const { ch, fc } = newChannel({ chunkSize: 4 });
    const data = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);  // 10 bytes
    const handle = fc.uploadFile(makeFile(data, "x.pdf", "application/pdf"), {
      target_selector: "#in",
    });

    // Wait for the start envelope to land. The async hash + send
    // happen on microtask ticks; flush a couple.
    await flush();

    const envs = ch.sent.map((s) => JSON.parse(s));
    // Expect: start, chunk seq=0 (4B), chunk seq=1 (4B), chunk seq=2 (2B), end
    const types = envs.map((e) => e.type);
    expect(types).toEqual([
      "file_upload_start",
      "file_upload_chunk",
      "file_upload_chunk",
      "file_upload_chunk",
      "file_upload_end",
    ]);

    expect(envs[0]).toMatchObject({
      v: 1, type: "file_upload_start",
      upload_id: "u-test", name: "x.pdf", mime_type: "application/pdf",
      size: 10, sha256: FIXED_SHA, target_selector: "#in",
    });
    expect(envs[1]).toMatchObject({ type: "file_upload_chunk", seq: 0 });
    expect(envs[2]).toMatchObject({ type: "file_upload_chunk", seq: 1 });
    expect(envs[3]).toMatchObject({ type: "file_upload_chunk", seq: 2 });
    expect(envs[4]).toMatchObject({ type: "file_upload_end", upload_id: "u-test" });

    // Server replies file_upload_complete; the promise resolves.
    ch.pushMessage(JSON.stringify({
      v: 1, type: "file_upload_complete",
      upload_id: "u-test",
      server_path: "/tmp/x.pdf",
      attached_via: "domSetFileInputFiles",
    }));

    await expect(handle.done).resolves.toEqual({
      server_path: "/tmp/x.pdf",
      attached_via: "domSetFileInputFiles",
    });
  });

  it("invokes onProgress per chunk and onServerProgress on server envelopes", async () => {
    const { ch, fc } = newChannel({ chunkSize: 5 });
    const data = new Uint8Array(15);
    const progressCalls: number[] = [];
    const serverCalls: number[] = [];
    const handle = fc.uploadFile(makeFile(data), {
      onProgress: (p) => progressCalls.push(p.bytes_sent),
      onServerProgress: (e) => serverCalls.push(e.bytes_received),
    });
    await flush();
    expect(progressCalls).toEqual([5, 10, 15]);

    ch.pushMessage(JSON.stringify({
      v: 1, type: "file_upload_progress",
      upload_id: "u-test", bytes_received: 7, bytes_total: 15,
    }));
    expect(serverCalls).toEqual([7]);

    ch.pushMessage(JSON.stringify({
      v: 1, type: "file_upload_complete",
      upload_id: "u-test", server_path: "/tmp/y.bin", attached_via: "fileChooser",
    }));
    await expect(handle.done).resolves.toMatchObject({ attached_via: "fileChooser" });
  });
});

// ---------------------------------------------------------------------------
// errors and cancellation
// ---------------------------------------------------------------------------

describe("FileUploadChannel error paths", () => {
  it("rejects on size_limit_exceeded without sending anything", async () => {
    const { ch, fc } = newChannel({ maxTotalSize: 4 });
    const handle = fc.uploadFile(makeFile(new Uint8Array(10)));
    await expect(handle.done).rejects.toBeInstanceOf(FileUploadError);
    expect(ch.sent).toHaveLength(0);
  });

  it("propagates server file_upload_error code", async () => {
    const { ch, fc } = newChannel();
    const handle = fc.uploadFile(makeFile(new Uint8Array([1, 2, 3])));
    await flush();
    ch.pushMessage(JSON.stringify({
      v: 1, type: "file_upload_error",
      upload_id: "u-test", code: "sha256_mismatch", error: "expected X, got Y",
    }));
    await expect(handle.done).rejects.toMatchObject({
      code: "sha256_mismatch", message: "expected X, got Y", uploadId: "u-test",
    });
  });

  it("cancel() sends file_upload_cancel and rejects done", async () => {
    const { ch, fc } = newChannel({ chunkSize: 4 });
    const handle = fc.uploadFile(makeFile(new Uint8Array(20)));
    await flush();
    handle.cancel();

    const types = ch.sent.map(s => JSON.parse(s).type);
    expect(types).toContain("file_upload_cancel");
    await expect(handle.done).rejects.toMatchObject({ code: "cancelled" });
  });

  it("FileUploadChannel rejects an oversized chunkSize at construction", () => {
    const ch = new FakeChannel();
    expect(() => new FileUploadChannel(
      ch as unknown as ConstructorParameters<typeof FileUploadChannel>[0],
      { chunkSize: MAX_TOTAL_SIZE + 1 }))
      .toThrowError(FileUploadError);
  });
});

// ---------------------------------------------------------------------------
// Backpressure
// ---------------------------------------------------------------------------

describe("FileUploadChannel backpressure", () => {
  it("waits for bufferedamountlow when bufferedAmount > high-water", async () => {
    const { ch, fc } = newChannel({
      chunkSize: 4, bufferedHighWater: 100, bufferedLowWater: 50,
    });
    // Fill the buffer BEFORE starting the upload so the chunk loop
    // parks at its very first waitIfBuffered() call (just after the
    // start envelope sends, which doesn't gate on bufferedAmount).
    ch.bufferedAmount = 200;

    const handle = fc.uploadFile(makeFile(new Uint8Array(12)));
    // Pump microtasks past the hash + start-send.
    await flush();

    // The start envelope went through (sendOrThrow doesn't check the
    // high-water mark); the first chunk loop iteration parks.
    const startTypes = ch.sent.map(s => JSON.parse(s).type);
    expect(startTypes).toEqual(["file_upload_start"]);

    // Drain.
    ch.bufferedAmount = 0;
    ch.fireBufferedAmountLow();
    await flush();

    const allTypes = ch.sent.map(s => JSON.parse(s).type);
    expect(allTypes).toEqual([
      "file_upload_start",
      "file_upload_chunk",
      "file_upload_chunk",
      "file_upload_chunk",
      "file_upload_end",
    ]);

    // Server reply resolves.
    ch.pushMessage(JSON.stringify({
      v: 1, type: "file_upload_complete",
      upload_id: "u-test", server_path: "/tmp/z", attached_via: "none",
    }));
    await expect(handle.done).resolves.toMatchObject({ attached_via: "none" });
  });
});

// ---------------------------------------------------------------------------
// utility
// ---------------------------------------------------------------------------

/** Flush microtasks. The driveUpload pipeline awaits multiple
 *  promises (arrayBuffer, sha256, send loop) so a single await is
 *  insufficient. Run several queueMicrotask cycles. */
async function flush() {
  for (let i = 0; i < 8; i++) {
    await Promise.resolve();
  }
}

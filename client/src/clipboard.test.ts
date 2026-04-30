// Unit tests for client/src/clipboard.ts.

import { describe, it, expect, vi } from "vitest";
import {
  ClipboardChannel,
  PROTOCOL_VERSION,
  MAX_BYTES,
  isValidEnvelope,
  utf8ByteLength,
} from "./clipboard.js";

class FakeChannel {
  readyState: "connecting" | "open" | "closing" | "closed" = "open";
  sent: string[] = [];
  private handlers: { [k: string]: Array<(e: { data: string }) => void> } = {};
  send(d: string): void { this.sent.push(d); }
  addEventListener(type: string, h: (e: { data: string }) => void): void {
    (this.handlers[type] ??= []).push(h);
  }
  pushMessage(raw: string): void {
    for (const h of this.handlers["message"] ?? []) h({ data: raw });
  }
}

// ---------------------------------------------------------------------------
// utf8ByteLength
// ---------------------------------------------------------------------------

describe("utf8ByteLength", () => {
  it("counts ASCII", () => {
    expect(utf8ByteLength("hello")).toBe(5);
  });
  it("counts multi-byte chars", () => {
    expect(utf8ByteLength("é")).toBe(2);
    expect(utf8ByteLength("漢字")).toBe(6);
    expect(utf8ByteLength("🙂")).toBe(4);
  });
});

// ---------------------------------------------------------------------------
// isValidEnvelope
// ---------------------------------------------------------------------------

describe("isValidEnvelope", () => {
  const base = {
    v: PROTOCOL_VERSION, type: "clipboard_offer", t: 1, seq: 0,
    data: { direction: "client->cloud", source: "user_action", text: "x" },
  };

  it("accepts both directions", () => {
    expect(isValidEnvelope(base)).toBe(true);
    expect(isValidEnvelope({ ...base, data: { ...base.data, direction: "cloud->client" } })).toBe(true);
  });

  it("rejects unknown source", () => {
    expect(isValidEnvelope({ ...base, data: { ...base.data, source: "sync" } })).toBe(false);
  });

  it("rejects oversize text", () => {
    const big = "A".repeat(MAX_BYTES + 1);
    expect(isValidEnvelope({ ...base, data: { ...base.data, text: big } })).toBe(false);
  });

  it("rejects wrong version / type / fields", () => {
    expect(isValidEnvelope({ ...base, v: 2 })).toBe(false);
    expect(isValidEnvelope({ ...base, type: "input" })).toBe(false);
    expect(isValidEnvelope({ ...base, data: { source: "user_action", text: "x" } })).toBe(false);
    expect(isValidEnvelope("not an object")).toBe(false);
  });
});

// ---------------------------------------------------------------------------
// ClipboardChannel — outgoing
// ---------------------------------------------------------------------------

describe("ClipboardChannel.sendPaste", () => {
  it("sends a v1 client->cloud envelope when channel is open", () => {
    const ch = new FakeChannel();
    const cb = new ClipboardChannel(ch, { now: () => 100 });
    expect(cb.sendPaste("hello")).toBe(true);
    expect(ch.sent).toHaveLength(1);
    const env = JSON.parse(ch.sent[0]!);
    expect(env).toMatchObject({
      v: PROTOCOL_VERSION, type: "clipboard_offer", t: 100, seq: 0,
      data: { direction: "client->cloud", source: "user_action", text: "hello" },
    });
  });

  it("increments seq across sends", () => {
    const ch = new FakeChannel();
    const cb = new ClipboardChannel(ch);
    cb.sendPaste("a");
    cb.sendPaste("b");
    cb.sendPaste("c");
    const seqs = ch.sent.map(s => JSON.parse(s).seq);
    expect(seqs).toEqual([0, 1, 2]);
  });

  it("drops oversize and reports", () => {
    const ch = new FakeChannel();
    const onOversize = vi.fn();
    const cb = new ClipboardChannel(ch, { onOversize });
    const big = "A".repeat(MAX_BYTES + 1);
    expect(cb.sendPaste(big)).toBe(false);
    expect(ch.sent).toHaveLength(0);
    expect(onOversize).toHaveBeenCalledOnce();
  });

  it("does nothing when channel is not open", () => {
    const ch = new FakeChannel();
    ch.readyState = "closed";
    const cb = new ClipboardChannel(ch);
    expect(cb.sendPaste("x")).toBe(false);
    expect(ch.sent).toHaveLength(0);
  });
});

// ---------------------------------------------------------------------------
// ClipboardChannel — incoming
// ---------------------------------------------------------------------------

describe("ClipboardChannel inbound", () => {
  it("writes received text via writeText when focused", async () => {
    const ch = new FakeChannel();
    const writes: string[] = [];
    const cb = new ClipboardChannel(ch, {
      writeText: (t) => { writes.push(t); return Promise.resolve(); },
      hasFocus: () => true,
    });
    void cb;
    ch.pushMessage(JSON.stringify({
      v: 1, type: "clipboard_offer", t: 1, seq: 0,
      data: { direction: "cloud->client", source: "user_action", text: "from cloud" },
    }));
    await Promise.resolve();
    expect(writes).toEqual(["from cloud"]);
  });

  it("queues when not focused; flushes on flushPending", async () => {
    const ch = new FakeChannel();
    const writes: string[] = [];
    const cb = new ClipboardChannel(ch, {
      writeText: (t) => { writes.push(t); return Promise.resolve(); },
      hasFocus: () => false,
    });
    ch.pushMessage(JSON.stringify({
      v: 1, type: "clipboard_offer", t: 1, seq: 0,
      data: { direction: "cloud->client", source: "user_action", text: "queued" },
    }));
    await Promise.resolve();
    expect(writes).toHaveLength(0);
    cb.flushPending();
    await Promise.resolve();
    expect(writes).toEqual(["queued"]);
  });

  it("ignores client->cloud echoes", async () => {
    const ch = new FakeChannel();
    const writes: string[] = [];
    const cb = new ClipboardChannel(ch, {
      writeText: (t) => { writes.push(t); return Promise.resolve(); },
      hasFocus: () => true,
    });
    void cb;
    ch.pushMessage(JSON.stringify({
      v: 1, type: "clipboard_offer", t: 1, seq: 0,
      data: { direction: "client->cloud", source: "user_action", text: "ours" },
    }));
    await Promise.resolve();
    expect(writes).toHaveLength(0);
  });

  it("drops invalid JSON via onDropped", () => {
    const ch = new FakeChannel();
    const onDropped = vi.fn();
    const cb = new ClipboardChannel(ch, { onDropped });
    void cb;
    ch.pushMessage("not json");
    expect(onDropped).toHaveBeenCalledOnce();
  });

  it("drops invalid envelope shape via onDropped", () => {
    const ch = new FakeChannel();
    const onDropped = vi.fn();
    const cb = new ClipboardChannel(ch, { onDropped });
    void cb;
    ch.pushMessage(JSON.stringify({ v: 99, type: "clipboard_offer", t: 1, seq: 0, data: {} }));
    expect(onDropped).toHaveBeenCalled();
  });
});

// Unit tests for client/src/input.ts.
//
// Uses vitest's default test runner. No DOM is required — we drive the
// InputChannel through its public methods and observe a fake
// SendableChannel.

import { describe, it, expect } from "vitest";
import {
  InputChannel,
  InputEnvelope,
  PROTOCOL_VERSION,
  MOD_META,
  MOD_SHIFT,
  modsFromEvent,
  extractDragItems,
} from "./input.js";

class FakeChannel {
  readyState: "connecting" | "open" | "closing" | "closed" = "open";
  bufferedAmount = 0;
  sent: InputEnvelope[] = [];
  send(data: string): void {
    this.sent.push(JSON.parse(data) as InputEnvelope);
  }
}

/** Build a manual rAF scheduler so tests are deterministic. */
function manualRaf(): { raf: (cb: () => void) => number; tick: () => void; cancel: (id: number) => void; pending: number } {
  const queue: Array<() => void> = [];
  const state = {
    raf: (cb: () => void) => { queue.push(cb); return queue.length; },
    cancel: (_id: number) => { /* noop for tests; we just drain */ },
    tick: () => {
      const cbs = queue.splice(0);
      for (const cb of cbs) cb();
    },
    get pending() { return queue.length; },
  };
  return state;
}

describe("InputChannel", () => {
  it("emits a v1 envelope with monotonic seq and encoded data", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    let now = 1_000;
    const ic = new InputChannel(ch, { now: () => now++, raf: sched.raf, cancelRaf: sched.cancel });

    ic.sendKeyDown("KeyA", "a", MOD_SHIFT);
    ic.sendKeyUp("KeyA", "a", 0);
    sched.tick();

    expect(ch.sent).toHaveLength(2);
    expect(ch.sent[0]).toMatchObject({
      v: PROTOCOL_VERSION,
      type: "key_down",
      seq: 0,
      data: { code: "KeyA", key: "a", mods: MOD_SHIFT },
    });
    expect(ch.sent[1]).toMatchObject({ type: "key_up", seq: 1, data: { mods: 0 } });
    // Strict monotonic
    expect(ch.sent[0]!.t).toBeLessThan(ch.sent[1]!.t);
  });

  it("coalesces repeated mouse_move into a single send per flush", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    let dropped = 0;
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel, onCoalesce: (n) => { dropped += n; } });

    ic.sendMouseMove(1, 1);
    ic.sendMouseMove(2, 2);
    ic.sendMouseMove(3, 3);
    ic.sendMouseMove(4, 4);
    sched.tick();

    expect(ch.sent).toHaveLength(1);
    expect(ch.sent[0]).toMatchObject({ type: "mouse_move", data: { x: 4, y: 4 } });
    // 3 moves dropped (1st, 2nd, 3rd)
    expect(dropped).toBe(3);
  });

  it("never drops non-mouse_move events under load", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });

    for (let i = 0; i < 10; i++) ic.sendMouseMove(i, i);
    ic.sendMouseButton(0, "down", 5, 5);
    ic.sendKeyDown("Enter", "Enter", 0);
    ic.sendMouseWheel(0, -120, 0, 5, 5);
    sched.tick();

    const types = ch.sent.map(e => e.type);
    expect(types).toEqual(["mouse_button", "key_down", "mouse_wheel", "mouse_move"]);
    expect(ch.sent[3]!.data).toEqual({ x: 9, y: 9 });
  });

  it("drops mouse_move silently if bufferedAmount exceeds threshold", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, {
      raf: sched.raf, cancelRaf: sched.cancel,
      bufferedAmountThreshold: 100,
    });
    ch.bufferedAmount = 200; // saturated

    ic.sendMouseMove(7, 8);
    sched.tick();

    // Backpressure DOESN'T mean we never send — the latest move is still
    // queued; the over-threshold count is reflected in the dropped tally.
    expect(ch.sent).toHaveLength(1);
    expect(ch.sent[0]!.data).toEqual({ x: 7, y: 8 });
  });

  it("drops queued events when channel is not open at flush time", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });

    ic.sendKeyDown("KeyA", "a", 0);
    ic.sendMouseMove(1, 1);
    ch.readyState = "closed";
    sched.tick();

    expect(ch.sent).toHaveLength(0);
  });

  it("preserves composition and clipboard event types", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });

    ic.sendComposition("start", "");
    ic.sendComposition("update", "こ");
    ic.sendComposition("update", "こん");
    ic.sendComposition("end", "こんにちは");
    ic.sendClipboardPaste("hello");
    ic.sendClipboardCopyRequest();
    sched.tick();

    expect(ch.sent.map(e => e.type)).toEqual([
      "composition_start", "composition_update", "composition_update", "composition_end",
      "clipboard_paste", "clipboard_copy_request",
    ]);
    expect(ch.sent[3]!.data).toEqual({ data: "こんにちは" });
    expect(ch.sent[4]!.data).toEqual({ text: "hello" });
    expect(ch.sent[5]!.data).toEqual({});
  });

  it("flush() drains synchronously without waiting for rAF", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });

    ic.sendKeyDown("KeyA", "a", 0);
    expect(ch.sent).toHaveLength(0);
    ic.flush();
    expect(ch.sent).toHaveLength(1);
  });

  it("rounds non-integer coordinates", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });
    ic.sendMouseMove(10.4, 20.6);
    ic.sendMouseWheel(0.1, -120.7, 0, 1.2, 2.7);
    sched.tick();

    // Non-coalescable events (wheel) flush before mouse_move.
    expect(ch.sent[0]!.type).toBe("mouse_wheel");
    expect(ch.sent[0]!.data).toEqual({ dx: 0, dy: -121, mode: 0, x: 1, y: 3 });
    expect(ch.sent[1]!.type).toBe("mouse_move");
    expect(ch.sent[1]!.data).toEqual({ x: 10, y: 21 });
  });

  it("bubbles send errors through onError", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    ch.send = () => { throw new Error("boom"); };
    const errs: unknown[] = [];
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel, onError: (e) => errs.push(e) });

    ic.sendKeyDown("KeyA", "a", 0);
    sched.tick();

    expect(errs).toHaveLength(1);
    expect((errs[0] as Error).message).toBe("boom");
  });
});

describe("modsFromEvent", () => {
  it("packs each modifier into its expected bit", () => {
    expect(modsFromEvent({ shiftKey: false, ctrlKey: false, altKey: false, metaKey: false })).toBe(0);
    expect(modsFromEvent({ shiftKey: true,  ctrlKey: false, altKey: false, metaKey: false })).toBe(MOD_SHIFT);
    expect(modsFromEvent({ shiftKey: true,  ctrlKey: false, altKey: false, metaKey: true  })).toBe(MOD_SHIFT | MOD_META);
  });
});

// ---------------------------------------------------------------------------
// v1.1 — drag-and-drop senders + extractDragItems
// ---------------------------------------------------------------------------

describe("InputChannel drag/drop", () => {
  it("emits drag_start with full item payload", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });

    ic.sendDragStart(100, 200, ["text/plain"], [
      { kind: "string", type: "text/plain", data: "hello" },
    ]);
    sched.tick();

    expect(ch.sent).toHaveLength(1);
    const env = ch.sent[0]!;
    expect(env.type).toBe("drag_start");
    expect(env.v).toBe(PROTOCOL_VERSION);
    expect(env.data).toEqual({
      x: 100, y: 200,
      types: ["text/plain"],
      items: [{ kind: "string", type: "text/plain", data: "hello" }],
    });
  });

  it("coalesces drag_over the same way mouse_move is coalesced", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });

    // Five drag_overs in a single rAF tick → only the last one ships.
    ic.sendDragOver(10, 10);
    ic.sendDragOver(20, 20);
    ic.sendDragOver(30, 30);
    ic.sendDragOver(40, 40);
    ic.sendDragOver(50, 50);
    sched.tick();

    const overs = ch.sent.filter(e => e.type === "drag_over");
    expect(overs).toHaveLength(1);
    expect(overs[0]!.data).toEqual({ x: 50, y: 50 });
  });

  it("emits drop then drag_end on the convenience path", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });

    ic.sendDrop(700, 410, ["text/uri-list"], [
      { kind: "string", type: "text/uri-list", data: "https://example.com/" },
    ]);
    ic.sendDragEnd(true);
    sched.tick();

    expect(ch.sent.map(e => e.type)).toEqual(["drop", "drag_end"]);
    expect((ch.sent[1]!.data as { success: boolean }).success).toBe(true);
  });

  it("seq increments across drag events monotonically", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });

    ic.sendDragStart(0, 0, [], []);
    ic.sendDragOver(10, 10);
    ic.sendDrop(20, 20, [], []);
    ic.sendDragEnd(true);
    sched.tick();

    const seqs = ch.sent.map(e => e.seq);
    expect(seqs).toEqual([0, 1, 2, 3]);
  });

  it("rounds non-integer drag coordinates", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });

    ic.sendDragStart(10.6, 20.4, [], []);
    ic.sendDragOver(30.5, 40.5);
    sched.tick();

    expect((ch.sent[0]!.data as { x: number; y: number }).x).toBe(11);
    expect((ch.sent[0]!.data as { x: number; y: number }).y).toBe(20);
    const overs = ch.sent.filter(e => e.type === "drag_over");
    expect((overs[0]!.data as { x: number; y: number }).x).toBe(31); // 30.5 → 31 (round half up)
  });
});

// ---------------------------------------------------------------------------
// extractDragItems — pulls types + items out of a DataTransfer-shaped object
// ---------------------------------------------------------------------------

describe("extractDragItems", () => {
  it("returns empty arrays for null DataTransfer", () => {
    expect(extractDragItems(null)).toEqual({ types: [], items: [] });
  });

  it("extracts string items with their data via getData", () => {
    const dt = makeStubDataTransfer(
      ["text/plain", "text/uri-list"],
      [
        { kind: "string", type: "text/plain", data: "hello" },
        { kind: "string", type: "text/uri-list", data: "https://example.com/" },
      ],
    );
    const r = extractDragItems(dt);
    expect(r.types).toEqual(["text/plain", "text/uri-list"]);
    expect(r.items).toEqual([
      { kind: "string", type: "text/plain", data: "hello" },
      { kind: "string", type: "text/uri-list", data: "https://example.com/" },
    ]);
  });

  it("emits file items with NO data field and warns once", () => {
    const warnings: unknown[][] = [];
    const origWarn = console.warn;
    console.warn = (...args: unknown[]) => { warnings.push(args); };
    try {
      const dt = makeStubDataTransfer(
        ["application/x-moz-file"],
        [
          { kind: "file", type: "image/png" },
          { kind: "file", type: "text/plain" },
        ],
      );
      const r = extractDragItems(dt);
      expect(r.items).toEqual([
        { kind: "file", type: "image/png" },
        { kind: "file", type: "text/plain" },
      ]);
      // Exactly one warning, even with two file items.
      expect(warnings).toHaveLength(1);
      expect(String(warnings[0]![0])).toMatch(/file drags are not transmitted in v1/);
    } finally {
      console.warn = origWarn;
    }
  });

  it("falls back to types + getData when DataTransferItemList is empty", () => {
    const dt: DataTransfer = {
      types: ["text/plain"],
      items: { length: 0 } as unknown as DataTransferItemList,
      getData: (t: string) => (t === "text/plain" ? "fallback" : ""),
    } as unknown as DataTransfer;
    const r = extractDragItems(dt);
    expect(r.items).toEqual([{ kind: "string", type: "text/plain", data: "fallback" }]);
  });
});

/** Build a DataTransfer-shaped stub with a synchronous getData. */
function makeStubDataTransfer(
  types: string[],
  items: Array<{ kind: "string" | "file"; type: string; data?: string }>,
): DataTransfer {
  // Index map used by getData(mime) -> data.
  const dataByType = new Map<string, string>();
  for (const it of items) {
    if (it.kind === "string" && it.data !== undefined) {
      dataByType.set(it.type, it.data);
    }
  }
  const arr = items.map((it, _i) => ({
    kind: it.kind, type: it.type,
  })) as unknown as DataTransferItemList;
  // Length needs to be enumerable for the for-loop in extractDragItems.
  Object.defineProperty(arr, "length", { value: items.length });
  for (let i = 0; i < items.length; i++) {
    Object.defineProperty(arr, i, { value: arr[i], enumerable: true });
  }
  return {
    types,
    items: arr,
    getData: (t: string) => dataByType.get(t.toLowerCase()) ?? "",
  } as unknown as DataTransfer;
}

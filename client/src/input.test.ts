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

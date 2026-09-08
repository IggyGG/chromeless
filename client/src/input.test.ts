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
  keyBelongsToClient,
  type KeyTargetLike,
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
    // T62 added optional v1.1 fields (delta_mode, phase, momentum)
    // — assert the original required fields rather than full equality.
    expect(ch.sent[0]!.data).toMatchObject({ dx: 0, dy: -121, mode: 0, x: 1, y: 3 });
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

// ---------------------------------------------------------------------------
// v1.1 (T88) — IME composition extensions
// ---------------------------------------------------------------------------

describe("InputChannel composition (T88)", () => {
  it("legacy string-payload sendComposition still works", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });

    ic.sendComposition("update", "ni hao");
    sched.tick();

    expect(ch.sent).toHaveLength(1);
    expect(ch.sent[0]!.type).toBe("composition_update");
    expect(ch.sent[0]!.data).toEqual({ data: "ni hao" });
  });

  it("v1.1 object-payload carries selection_start/end + rect", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });

    ic.sendComposition("start", {
      data: "",
      rect: { x: 100, y: 200, w: 12, h: 18 },
    });
    ic.sendComposition("update", {
      data: "ni hao",
      selection_start: 3,
      selection_end: 6,
    });
    ic.sendComposition("end", { data: "你好" });
    sched.tick();

    const types = ch.sent.map(e => e.type);
    expect(types).toEqual(["composition_start", "composition_update", "composition_end"]);

    expect(ch.sent[0]!.data).toEqual({
      data: "", rect: { x: 100, y: 200, w: 12, h: 18 },
    });
    expect(ch.sent[1]!.data).toEqual({
      data: "ni hao", selection_start: 3, selection_end: 6,
    });
    expect(ch.sent[2]!.data).toEqual({ data: "你好" });
  });

  it("sendCompositionCancel emits an empty-payload cancel envelope", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });

    ic.sendCompositionCancel();
    sched.tick();

    expect(ch.sent).toHaveLength(1);
    expect(ch.sent[0]!.type).toBe("composition_cancel");
    expect(ch.sent[0]!.data).toEqual({});
  });

  it("seq increments monotonically across a full composition lifecycle", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });

    ic.sendComposition("start", { data: "" });
    ic.sendComposition("update", { data: "n", selection_start: 1, selection_end: 1 });
    ic.sendComposition("update", { data: "ni", selection_start: 2, selection_end: 2 });
    ic.sendComposition("update", { data: "ni hao", selection_start: 6, selection_end: 6 });
    ic.sendComposition("end", { data: "你好" });
    sched.tick();

    expect(ch.sent.map(e => e.seq)).toEqual([0, 1, 2, 3, 4]);
  });
});

// ---------------------------------------------------------------------------
// v1.1 — wheel phase machine + scroll inertia
// ---------------------------------------------------------------------------

/** Manual-drive scheduler for setTimer/clearTimer. Lets tests fire the
 *  end-of-gesture timer deterministically. */
function manualTimer(): {
  setTimer: (cb: () => void, _ms: number) => unknown;
  clearTimer: (id: unknown) => void;
  fire: () => boolean;  // returns true if a timer fired
  pending: number;
} {
  const queue: Array<{ id: number; cb: () => void; cancelled: boolean }> = [];
  let nextId = 1;
  return {
    setTimer: (cb: () => void, _ms: number) => {
      const id = nextId++;
      queue.push({ id, cb, cancelled: false });
      return id;
    },
    clearTimer: (id: unknown) => {
      const e = queue.find(e => e.id === id);
      if (e) e.cancelled = true;
    },
    fire: () => {
      // Find the first non-cancelled timer; fire it; mark cancelled.
      const e = queue.find(x => !x.cancelled);
      if (!e) return false;
      e.cancelled = true;
      e.cb();
      return true;
    },
    get pending() { return queue.filter(e => !e.cancelled).length; },
  };
}

describe("InputChannel wheel inertia (v1.1)", () => {
  it("first wheel event is phase=start; subsequent are phase=changed", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const t = manualTimer();
    let now = 1000;
    const ic = new InputChannel(ch, {
      now: () => now, raf: sched.raf, cancelRaf: sched.cancel,
      setTimer: t.setTimer, clearTimer: t.clearTimer,
    });

    ic.sendMouseWheel(0, -120, 0, 100, 100);
    now += 16;
    ic.sendMouseWheel(0, -100, 0, 100, 100);
    now += 16;
    ic.sendMouseWheel(0, -80, 0, 100, 100);
    sched.tick();

    const wheels = ch.sent.filter(e => e.type === "mouse_wheel");
    expect(wheels).toHaveLength(3);
    expect((wheels[0]!.data as { phase: string }).phase).toBe("start");
    expect((wheels[1]!.data as { phase: string }).phase).toBe("changed");
    expect((wheels[2]!.data as { phase: string }).phase).toBe("changed");
  });

  it("delta_mode passes through pixel/line/page", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const t = manualTimer();
    const ic = new InputChannel(ch, {
      raf: sched.raf, cancelRaf: sched.cancel,
      setTimer: t.setTimer, clearTimer: t.clearTimer,
    });

    ic.sendMouseWheel(0, -1, 0, 0, 0);  sched.tick();
    ic.emitWheelEnd();                   sched.tick();
    ic.sendMouseWheel(0, -1, 1, 0, 0);  sched.tick();
    ic.emitWheelEnd();                   sched.tick();
    ic.sendMouseWheel(0, -1, 2, 0, 0);  sched.tick();
    sched.tick();

    const modes = ch.sent
      .filter(e => e.type === "mouse_wheel" && (e.data as { phase: string }).phase !== "end")
      .map(e => (e.data as { delta_mode: string }).delta_mode);
    expect(modes).toEqual(["pixel", "line", "page"]);
  });

  it("end-of-gesture timer fires a phase=end zero-delta envelope", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const t = manualTimer();
    const ic = new InputChannel(ch, {
      raf: sched.raf, cancelRaf: sched.cancel,
      setTimer: t.setTimer, clearTimer: t.clearTimer,
    });

    ic.sendMouseWheel(0, -120, 0, 50, 60);
    sched.tick();
    expect(t.pending).toBe(1);  // timer armed

    // Fire the timer manually — simulates 150 ms of quiet.
    expect(t.fire()).toBe(true);
    sched.tick();

    const wheels = ch.sent.filter(e => e.type === "mouse_wheel");
    expect(wheels).toHaveLength(2);
    const endEnv = wheels[1]!;
    expect((endEnv.data as { phase: string }).phase).toBe("end");
    expect((endEnv.data as { dx: number; dy: number }).dx).toBe(0);
    expect((endEnv.data as { dx: number; dy: number }).dy).toBe(0);
    // End preserves the last-known position + mode.
    expect((endEnv.data as { x: number; y: number }).x).toBe(50);
    expect((endEnv.data as { x: number; y: number }).y).toBe(60);
  });

  it("new wheel event before timer fires re-arms (no spurious end)", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const t = manualTimer();
    const ic = new InputChannel(ch, {
      raf: sched.raf, cancelRaf: sched.cancel,
      setTimer: t.setTimer, clearTimer: t.clearTimer,
    });

    ic.sendMouseWheel(0, -120, 0, 0, 0);
    ic.sendMouseWheel(0, -100, 0, 0, 0);
    ic.sendMouseWheel(0, -80, 0, 0, 0);
    sched.tick();

    // Only one timer should be active (the previous ones were cancelled).
    expect(t.pending).toBe(1);

    // Firing it fires phase=end exactly once.
    t.fire();
    sched.tick();
    const ends = ch.sent.filter(e => e.type === "mouse_wheel"
      && (e.data as { phase: string }).phase === "end");
    expect(ends).toHaveLength(1);
  });

  it("after end, the next wheel event is phase=start again", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const t = manualTimer();
    const ic = new InputChannel(ch, {
      raf: sched.raf, cancelRaf: sched.cancel,
      setTimer: t.setTimer, clearTimer: t.clearTimer,
    });

    ic.sendMouseWheel(0, -120, 0, 0, 0);  sched.tick();
    t.fire(); sched.tick();
    ic.sendMouseWheel(0, -100, 0, 0, 0);  sched.tick();

    const wheels = ch.sent.filter(e => e.type === "mouse_wheel");
    const phases = wheels.map(e => (e.data as { phase: string }).phase);
    expect(phases).toEqual(["start", "end", "start"]);
  });

  it("flags decaying-magnitude tail events as momentum", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const t = manualTimer();
    let now = 1000;
    const ic = new InputChannel(ch, {
      now: () => now, raf: sched.raf, cancelRaf: sched.cancel,
      setTimer: t.setTimer, clearTimer: t.clearTimer,
      wheelMomentumGapMs: 100,
    });

    // User drives 3 hard scrolls...
    ic.sendMouseWheel(0, -300, 0, 0, 0);
    now += 16;  // tight gap, but first event is phase=start (never momentum)
    ic.sendMouseWheel(0, -200, 0, 0, 0);  // changed; magnitude smaller; gap small → momentum
    now += 16;
    ic.sendMouseWheel(0, -100, 0, 0, 0);  // changed; smaller still → momentum
    now += 16;
    ic.sendMouseWheel(0, -50, 0, 0, 0);   // changed; smaller still → momentum
    sched.tick();

    const wheels = ch.sent.filter(e => e.type === "mouse_wheel");
    const momentum = wheels.map(e => (e.data as { momentum: boolean }).momentum);
    // start is always non-momentum; subsequent decaying-magnitude are momentum.
    expect(momentum).toEqual([false, true, true, true]);
  });

  it("does NOT flag growing-magnitude as momentum", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const t = manualTimer();
    let now = 1000;
    const ic = new InputChannel(ch, {
      now: () => now, raf: sched.raf, cancelRaf: sched.cancel,
      setTimer: t.setTimer, clearTimer: t.clearTimer,
    });
    ic.sendMouseWheel(0, -50, 0, 0, 0);
    now += 16;
    ic.sendMouseWheel(0, -100, 0, 0, 0);   // growing → user-driven
    now += 16;
    ic.sendMouseWheel(0, -200, 0, 0, 0);   // growing → user-driven
    sched.tick();

    const wheels = ch.sent.filter(e => e.type === "mouse_wheel");
    const momentum = wheels.map(e => (e.data as { momentum: boolean }).momentum);
    expect(momentum).toEqual([false, false, false]);
  });

  it("zero-delta wheel without an active gesture is dropped", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const t = manualTimer();
    const ic = new InputChannel(ch, {
      raf: sched.raf, cancelRaf: sched.cancel,
      setTimer: t.setTimer, clearTimer: t.clearTimer,
    });
    ic.sendMouseWheel(0, 0, 0, 0, 0);
    sched.tick();
    expect(ch.sent.filter(e => e.type === "mouse_wheel")).toHaveLength(0);
    expect(t.pending).toBe(0);
  });
});

// Which browser does a key belong to?
//
// Keys are listened for on the WINDOW (a <video> cannot hold focus in a way
// that delivers key events), so without this rule every key reached the
// guest — the URL typed into the client's own address bar was streamed
// keystroke by keystroke into whatever the remote page had focused, and
// Cmd/Ctrl+L / +T / +W / +R fired in both browsers at once.
describe("keyBelongsToClient", () => {
  const el = (over: Partial<KeyTargetLike> = {}): KeyTargetLike =>
    ({ tagName: "DIV", closest: () => null, ...over });

  it("claims text entry and controls the user can type into", () => {
    for (const tag of ["INPUT", "TEXTAREA", "SELECT", "BUTTON"]) {
      expect(keyBelongsToClient(el({ tagName: tag }))).toBe(true);
    }
  });

  it("is case-insensitive about the tag name", () => {
    // React/XHTML-ish DOMs and test doubles report lowercase.
    expect(keyBelongsToClient(el({ tagName: "input" }))).toBe(true);
  });

  it("claims contenteditable", () => {
    expect(keyBelongsToClient(el({ isContentEditable: true }))).toBe(true);
  });

  it("claims the control-channel overlay and the file picker", () => {
    for (const sel of [".cb-control-overlay", ".cb-filepick"]) {
      const t = el({ closest: (s: string) => (s === sel ? {} : null) });
      expect(keyBelongsToClient(t)).toBe(true);
    }
  });

  it("leaves an ordinary element to the cloud browser", () => {
    expect(keyBelongsToClient(el())).toBe(false);
    expect(keyBelongsToClient(el({ tagName: "VIDEO" }))).toBe(false);
    // isContentEditable false, not merely absent.
    expect(keyBelongsToClient(el({ isContentEditable: false }))).toBe(false);
  });

  it("forwards when there is no target at all", () => {
    // A null target must not silently swallow input: the default has to be
    // "the cloud browser gets it", or a DOM quirk turns into a dead keyboard.
    expect(keyBelongsToClient(null)).toBe(false);
    expect(keyBelongsToClient(undefined)).toBe(false);
  });

  it("survives a target with no closest()", () => {
    // document, window, and various test doubles have no closest.
    expect(keyBelongsToClient({ tagName: "DIV" })).toBe(false);
  });
});

// The RULE above is pure and easy to test; the WIRING is what actually
// stops a keystroke, and it is a separate thing that can rot on its own.
// Verified by mutation: deleting the gate from attach() left every
// keyBelongsToClient test green.
describe("attach() key gating", () => {
  /** Minimal window/target stand-ins — this package's tests run in node. */
  function fakeDom() {
    const listeners: Record<string, Array<(e: unknown) => void>> = {};
    const win = {
      addEventListener: (t: string, h: (e: unknown) => void) => {
        (listeners[t] ??= []).push(h);
      },
      removeEventListener: (t: string, h: (e: unknown) => void) => {
        listeners[t] = (listeners[t] ?? []).filter(x => x !== h);
      },
      getSelection: () => null,
    };
    const target = {
      addEventListener: () => {},
      removeEventListener: () => {},
      getBoundingClientRect: () => ({ left: 0, top: 0, width: 100, height: 100 }),
    };
    const fire = (type: string, e: Record<string, unknown>) => {
      for (const h of listeners[type] ?? []) h(e);
    };
    return { win, target, fire };
  }

  const keyEvent = (target: unknown) => ({
    code: "KeyA", key: "a", target,
    ctrlKey: false, shiftKey: false, altKey: false, metaKey: false,
  });

  it("does NOT forward a key typed into a client control", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });
    const dom = fakeDom();

    ic.attach(dom.target as unknown as HTMLElement, {
      window: dom.win as unknown as Window,
      shouldForwardKey: (e) => !keyBelongsToClient(e.target as KeyTargetLike),
    });

    dom.fire("keydown", keyEvent({ tagName: "INPUT", closest: () => null }));
    dom.fire("keyup", keyEvent({ tagName: "INPUT", closest: () => null }));
    sched.tick();

    expect(ch.sent).toHaveLength(0);
  });

  it("DOES forward a key aimed at the stream", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });
    const dom = fakeDom();

    ic.attach(dom.target as unknown as HTMLElement, {
      window: dom.win as unknown as Window,
      shouldForwardKey: (e) => !keyBelongsToClient(e.target as KeyTargetLike),
    });

    dom.fire("keydown", keyEvent({ tagName: "DIV", closest: () => null }));
    dom.fire("keyup", keyEvent({ tagName: "DIV", closest: () => null }));
    sched.tick();

    expect(ch.sent.map(e => e.type)).toEqual(["key_down", "key_up"]);
  });

  it("forwards everything when no policy is supplied", () => {
    // The default must stay permissive: every existing caller and every
    // other test in this file depends on it.
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });
    const dom = fakeDom();

    ic.attach(dom.target as unknown as HTMLElement,
              { window: dom.win as unknown as Window });

    dom.fire("keydown", keyEvent({ tagName: "INPUT", closest: () => null }));
    sched.tick();

    expect(ch.sent.map(e => e.type)).toEqual(["key_down"]);
  });
});

// The paste that went nowhere, twice.
//
// attach() used to wire the browser's `paste` event to a clipboard_paste
// envelope on the INPUT channel, while main.ts separately sent a
// clipboard_offer on the CLIPBOARD channel. The guest lists clipboard_paste
// in kKnownInputTypes — so it is not even logged as unknown — and handles
// it nowhere. Two sends, zero effect.
describe("attach() does not duplicate the paste path", () => {
  function fakeDom() {
    const listeners: Record<string, Array<(e: unknown) => void>> = {};
    const win = {
      addEventListener: (t: string, h: (e: unknown) => void) => {
        (listeners[t] ??= []).push(h);
      },
      removeEventListener: () => {},
      getSelection: () => null,
    };
    const target = {
      addEventListener: () => {}, removeEventListener: () => {},
      getBoundingClientRect: () => ({ left: 0, top: 0, width: 10, height: 10 }),
    };
    return { win, target, listeners };
  }

  it("registers no paste listener", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });
    const dom = fakeDom();

    ic.attach(dom.target as unknown as HTMLElement,
              { window: dom.win as unknown as Window });

    expect(dom.listeners["paste"]).toBeUndefined();
  });

  it("still registers copy — clipboard_copy_request IS consumed by the guest", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });
    const dom = fakeDom();

    ic.attach(dom.target as unknown as HTMLElement,
              { window: dom.win as unknown as Window });

    expect(dom.listeners["copy"]).toHaveLength(1);
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

// ---------------------------------------------------------------------------
// v1.1 — multi-touch senders
// ---------------------------------------------------------------------------

describe("InputChannel touch", () => {
  const baseTouch = {
    x: 100, y: 200, radius_x: 10, radius_y: 10, force: 0.5, twist: 0,
  };

  it("emits touch_start with all fields", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });

    ic.sendTouchStart({ identifier: 7, ...baseTouch });
    sched.tick();

    expect(ch.sent).toHaveLength(1);
    expect(ch.sent[0]!.type).toBe("touch_start");
    expect(ch.sent[0]!.data).toEqual({
      identifier: 7, x: 100, y: 200,
      radius_x: 10, radius_y: 10, force: 0.5, twist: 0,
    });
  });

  it("coalesces touch_move per identifier (latest wins per finger)", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });

    // Two fingers, three moves each in one rAF tick — expect exactly
    // 2 touch_move envelopes (one per finger, with the latest pos).
    ic.sendTouchMove({ identifier: 1, ...baseTouch, x: 10 });
    ic.sendTouchMove({ identifier: 2, ...baseTouch, x: 20 });
    ic.sendTouchMove({ identifier: 1, ...baseTouch, x: 11 });
    ic.sendTouchMove({ identifier: 2, ...baseTouch, x: 21 });
    ic.sendTouchMove({ identifier: 1, ...baseTouch, x: 12 });
    ic.sendTouchMove({ identifier: 2, ...baseTouch, x: 22 });
    sched.tick();

    const moves = ch.sent.filter(e => e.type === "touch_move");
    expect(moves).toHaveLength(2);

    // Sorted ascending by identifier per the doFlush impl.
    expect((moves[0]!.data as { identifier: number; x: number }).identifier).toBe(1);
    expect((moves[0]!.data as { identifier: number; x: number }).x).toBe(12);
    expect((moves[1]!.data as { identifier: number; x: number }).identifier).toBe(2);
    expect((moves[1]!.data as { identifier: number; x: number }).x).toBe(22);
  });

  it("flushes pending touch_move before touch_end for the same finger", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });

    // Queue a move, then end. The move must ship before the end so
    // the server sees the last-known position before lift.
    ic.sendTouchMove({ identifier: 3, ...baseTouch, x: 50 });
    ic.sendTouchEnd(3);
    sched.tick();

    // Order: pendingOther flushes first (touch_end was enqueued via
    // enqueue() AFTER we forced the touch_move into pendingOther),
    // so we expect [touch_move(50), touch_end].
    expect(ch.sent.map(e => e.type)).toEqual(["touch_move", "touch_end"]);
    expect((ch.sent[0]!.data as { x: number }).x).toBe(50);
    expect((ch.sent[1]!.data as { identifier: number }).identifier).toBe(3);
  });

  it("touch_cancel discards pending touch_move for the same finger", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });

    ic.sendTouchMove({ identifier: 4, ...baseTouch });
    ic.sendTouchCancel(4);
    sched.tick();

    // Just touch_cancel — the queued move was discarded.
    expect(ch.sent.map(e => e.type)).toEqual(["touch_cancel"]);
    expect((ch.sent[0]!.data as { identifier: number }).identifier).toBe(4);
  });

  it("rounds non-integer touch coordinates and clamps radii to >= 1", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });

    ic.sendTouchStart({
      identifier: 1, x: 10.4, y: 20.6,
      radius_x: 0.4, radius_y: 0.0,  // sub-pixel; should clamp to 1
      force: 0.5, twist: 12.7,
    });
    sched.tick();

    const env = ch.sent[0]!;
    expect(env.data).toEqual({
      identifier: 1, x: 10, y: 21,
      radius_x: 1, radius_y: 1, force: 0.5, twist: 13,
    });
  });

  it("seq increments monotonically across touch lifecycles", () => {
    const ch = new FakeChannel();
    const sched = manualRaf();
    const ic = new InputChannel(ch, { raf: sched.raf, cancelRaf: sched.cancel });

    ic.sendTouchStart({ identifier: 1, ...baseTouch });
    ic.sendTouchMove({ identifier: 1, ...baseTouch, x: 50 });
    ic.sendTouchEnd(1);
    sched.tick();

    expect(ch.sent.map(e => e.seq)).toEqual([0, 1, 2]);
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

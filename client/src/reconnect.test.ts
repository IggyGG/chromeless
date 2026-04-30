import { describe, it, expect } from "vitest";
import { ReconnectingWebSocket, ReconnectState, requestIceRecovery } from "./reconnect.js";
import type { RWSocket, RWSocketCtor } from "./reconnect.js";

// Manual fake WebSocket so tests are deterministic and synchronous.
// The runtime code only ever calls addEventListener / send / close /
// readyState / OPEN, so the surface is small.
let openSockets: FakeSocket[] = [];

class FakeSocket implements RWSocket {
  static OPEN = 1;
  static CLOSED = 3;
  readyState = 0; // CONNECTING
  url: string;
  listeners: Record<string, Set<(ev: any) => void>> = {
    open: new Set(), message: new Set(), close: new Set(), error: new Set(),
  };
  sent: string[] = [];

  constructor(url: string) {
    this.url = url;
    openSockets.push(this);
  }

  addEventListener(type: "open" | "message" | "close" | "error", listener: (ev: any) => void): void {
    this.listeners[type]!.add(listener);
  }
  removeEventListener(type: "open" | "message" | "close" | "error", listener: (ev: any) => void): void {
    this.listeners[type]!.delete(listener);
  }
  send(data: string): void {
    if (this.readyState !== FakeSocket.OPEN) throw new Error("socket not open");
    this.sent.push(data);
  }
  close(): void {
    this.readyState = FakeSocket.CLOSED;
    for (const l of this.listeners.close!) l({ wasClean: true, code: 1000, reason: "" } as CloseEvent);
  }

  // helpers
  triggerOpen(): void {
    this.readyState = FakeSocket.OPEN;
    for (const l of this.listeners.open!) l({} as Event);
  }
  triggerMessage(data: string): void {
    for (const l of this.listeners.message!) l({ data } as MessageEvent);
  }
  triggerClose(): void {
    this.readyState = FakeSocket.CLOSED;
    for (const l of this.listeners.close!) l({ wasClean: false, code: 1006, reason: "" } as CloseEvent);
  }
}

const Ctor: RWSocketCtor = FakeSocket as unknown as RWSocketCtor;

function manualClock(): {
  setTimeout: (cb: () => void, ms: number) => number;
  clearTimeout: (id: number) => void;
  advance: (ms: number) => void;
  pending: () => Array<{ at: number; ms: number }>;
} {
  let now = 0;
  const queue: Array<{ id: number; fireAt: number; cb: () => void; cancelled: boolean }> = [];
  let nextId = 1;
  return {
    setTimeout: (cb, ms) => {
      const id = nextId++;
      queue.push({ id, fireAt: now + ms, cb, cancelled: false });
      return id;
    },
    clearTimeout: (id) => {
      const e = queue.find(x => x.id === id);
      if (e) e.cancelled = true;
    },
    advance: (ms) => {
      now += ms;
      // Fire in order.
      while (true) {
        const ready = queue.filter(x => !x.cancelled && x.fireAt <= now).sort((a, b) => a.fireAt - b.fireAt);
        if (ready.length === 0) break;
        const e = ready[0]!;
        e.cancelled = true;
        e.cb();
      }
    },
    pending: () => queue.filter(x => !x.cancelled).map(x => ({ at: x.fireAt, ms: x.fireAt - now })),
  };
}

function newRws(initialBackoffMs = 1000, maxBackoffMs = 30000, maxAttempts?: number) {
  openSockets = [];
  const clock = manualClock();
  const transitions: Array<{ next: ReconnectState; prev: ReconnectState; attempt: number; nextDelayMs: number }> = [];
  const opens: number[] = [];
  const messages: MessageEvent[] = [];
  const rws = new ReconnectingWebSocket("ws://test/ws/dev", {
    webSocket: Ctor,
    setTimeout: clock.setTimeout,
    clearTimeout: clock.clearTimeout,
    initialBackoffMs,
    maxBackoffMs,
    backoffFactor: 2,
    ...(maxAttempts !== undefined ? { maxAttempts } : {}),
  });
  rws.on("stateChange", (next, prev, info) => transitions.push({ next, prev, attempt: info.attempt, nextDelayMs: info.nextDelayMs }));
  rws.on("open", () => opens.push(Date.now()));
  rws.on("message", (ev) => messages.push(ev));
  return { rws, clock, transitions, opens, messages };
}

describe("ReconnectingWebSocket", () => {
  it("transitions idle → connecting → connected on first open", () => {
    const { rws, transitions } = newRws();
    rws.connect();
    openSockets[0]!.triggerOpen();
    expect(transitions.map(t => t.next)).toEqual(["connecting", "connected"]);
    expect(rws.isConnected()).toBe(true);
  });

  it("forwards messages to subscribers", () => {
    const { rws, messages } = newRws();
    rws.connect();
    openSockets[0]!.triggerOpen();
    openSockets[0]!.triggerMessage("hello");
    expect(messages).toHaveLength(1);
    expect(messages[0]!.data).toBe("hello");
  });

  it("send returns false when disconnected and true when connected", () => {
    const { rws } = newRws();
    expect(rws.send("x")).toBe(false);
    rws.connect();
    expect(rws.send("x")).toBe(false); // still connecting
    openSockets[0]!.triggerOpen();
    expect(rws.send("x")).toBe(true);
    expect(openSockets[0]!.sent).toEqual(["x"]);
  });

  it("retries with exponential backoff after underlying close", () => {
    const { rws, clock, transitions } = newRws(1000, 30000);
    rws.connect();
    openSockets[0]!.triggerOpen();
    openSockets[0]!.triggerClose();

    // After close, scheduled retry at 1000 ms (attempt 1).
    expect(clock.pending()[0]?.ms).toBe(1000);
    clock.advance(1000);
    // Fresh socket exists; it stays in CONNECTING until we open it.
    expect(openSockets).toHaveLength(2);
    openSockets[1]!.triggerClose();

    // Next retry at 2000 ms (attempt 2).
    expect(clock.pending()[0]?.ms).toBe(2000);
    clock.advance(2000);
    openSockets[2]!.triggerClose();

    // Next retry at 4000 ms (attempt 3).
    expect(clock.pending()[0]?.ms).toBe(4000);

    // State should be reconnecting since underlying close → schedule retry.
    expect(rws.getState()).toBe("reconnecting");
    expect(rws.getAttempt()).toBe(3);
  });

  it("caps backoff at maxBackoffMs", () => {
    const { rws, clock } = newRws(1000, 4000);
    rws.connect();
    openSockets[0]!.triggerClose();
    // attempt=1: 1000
    clock.advance(1000);
    openSockets[1]!.triggerClose();
    // attempt=2: 2000
    clock.advance(2000);
    openSockets[2]!.triggerClose();
    // attempt=3: 4000 (capped, would otherwise be 4000 → fine)
    clock.advance(4000);
    openSockets[3]!.triggerClose();
    // attempt=4: still 4000 (capped from 8000)
    expect(clock.pending()[0]?.ms).toBe(4000);
    expect(rws.getAttempt()).toBe(4);
  });

  it("resets backoff on successful reconnect", () => {
    const { rws, clock } = newRws(1000, 30000);
    rws.connect();
    openSockets[0]!.triggerClose();      // attempt 1 → wait 1000
    clock.advance(1000);
    openSockets[1]!.triggerOpen();       // success: attempt resets
    expect(rws.getAttempt()).toBe(0);

    openSockets[1]!.triggerClose();      // next retry should be 1000 again
    expect(clock.pending()[0]?.ms).toBe(1000);
  });

  it("close() prevents further reconnect attempts", () => {
    const { rws, clock } = newRws();
    rws.connect();
    openSockets[0]!.triggerOpen();
    rws.close();
    expect(rws.getState()).toBe("closed");
    // Even if more time passes, no new sockets are created.
    clock.advance(60_000);
    expect(openSockets).toHaveLength(1);
  });

  it("transitions to failed after maxAttempts", () => {
    const { rws, clock } = newRws(100, 30000, 2);
    rws.connect();
    openSockets[0]!.triggerClose();   // attempt 1 → still reconnecting
    clock.advance(100);
    openSockets[1]!.triggerClose();   // attempt 2 → would be 3rd attempt, exceeds max
    expect(rws.getState()).toBe("failed");
    // No further timeouts queued.
    expect(clock.pending()).toEqual([]);
  });
});

describe("requestIceRecovery", () => {
  it("emits a request_renegotiate envelope and returns send result", () => {
    const sent: string[] = [];
    const ok = requestIceRecovery((f) => { sent.push(f); return true; }, "client");
    expect(ok).toBe(true);
    expect(JSON.parse(sent[0]!)).toEqual({ type: "request_renegotiate", from: "client", data: null });
  });

  it("returns false when send fails", () => {
    const ok = requestIceRecovery(() => false, "browser");
    expect(ok).toBe(false);
  });
});

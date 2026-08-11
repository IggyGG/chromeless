import { describe, it, expect, vi, beforeEach } from "vitest";
import { ChromelessSession, type Envelope } from "./session.js";
import type { RWSocket, RWSocketCtor } from "./reconnect.js";

// ---- fakes -----------------------------------------------------------
// FakeSocket mirrors reconnect.test.ts's shape — the machine only uses
// addEventListener / send / close / readyState / OPEN.

let openSockets: FakeSocket[] = [];

class FakeSocket implements RWSocket {
  static OPEN = 1;
  static CLOSED = 3;
  readyState = 0;
  url: string;
  listeners: Record<string, Set<(ev: unknown) => void>> = {
    open: new Set(), message: new Set(), close: new Set(), error: new Set(),
  };
  sent: string[] = [];

  constructor(url: string) {
    this.url = url;
    openSockets.push(this);
  }
  addEventListener(type: string, l: (ev: unknown) => void): void {
    this.listeners[type]!.add(l);
  }
  removeEventListener(type: string, l: (ev: unknown) => void): void {
    this.listeners[type]!.delete(l);
  }
  send(data: string): void {
    if (this.readyState !== FakeSocket.OPEN) throw new Error("socket not open");
    this.sent.push(data);
  }
  close(): void {
    this.readyState = FakeSocket.CLOSED;
    for (const l of this.listeners["close"]!) l({ wasClean: true, code: 1000, reason: "" });
  }
  // test helpers
  triggerOpen(): void {
    this.readyState = FakeSocket.OPEN;
    for (const l of this.listeners["open"]!) l({});
  }
  triggerMessage(data: string): void {
    for (const l of this.listeners["message"]!) l({ data });
  }
  sentEnvelopes(): Envelope[] {
    return this.sent.map((s) => JSON.parse(s) as Envelope);
  }
}

// Minimal fake RTCPeerConnection: enough surface for the machine's
// build + answer pipeline + teardown. Handlers are plain properties,
// matching how session.ts assigns them.
class FakePC {
  connectionState = "new";
  signalingState = "stable";
  iceConnectionState = "new";
  iceGatheringState = "new";
  onsignalingstatechange: (() => void) | null = null;
  oniceconnectionstatechange: (() => void) | null = null;
  onicegatheringstatechange: (() => void) | null = null;
  onconnectionstatechange: ((ev?: unknown) => void) | null = null;
  onicecandidate: ((ev: { candidate: { toJSON(): unknown; candidate: string } | null }) => void) | null = null;
  ontrack: ((ev: unknown) => void) | null = null;
  ondatachannel: ((ev: { channel: unknown }) => void) | null = null;

  remoteSet: unknown = null;
  localSet: unknown = null;
  closed = false;
  addedCandidates: unknown[] = [];

  async setRemoteDescription(d: unknown): Promise<void> { this.remoteSet = d; }
  async createAnswer(): Promise<{ type: string; sdp: string }> {
    // Realistic-enough SDP: one m=video with two codecs, VP9 second, so
    // prioritizeCodec has real work and classifyNegotiation sees VP9 lead.
    return {
      type: "answer",
      sdp: [
        "v=0", "o=- 0 0 IN IP4 0.0.0.0", "s=-", "t=0 0",
        "m=video 9 UDP/TLS/RTP/SAVPF 96 98",
        "a=rtpmap:96 VP8/90000",
        "a=rtpmap:98 VP9/90000",
        "",
      ].join("\r\n"),
    };
  }
  async setLocalDescription(d: unknown): Promise<void> { this.localSet = d; }
  async addIceCandidate(c: unknown): Promise<void> { this.addedCandidates.push(c); }
  close(): void { this.closed = true; }
  getStats(): Promise<Map<string, unknown>> { return Promise.resolve(new Map()); }

  // test helper: drive connectionState with handler firing
  setConnectionState(s: string): void {
    this.connectionState = s;
    this.onconnectionstatechange?.();
  }
}

function makeSession(overrides?: {
  fetchToken?: () => Promise<null>;
}) {
  const pcs: FakePC[] = [];
  const session = new ChromelessSession({
    signalingBase: "ws://test:8080/ws",
    socketCtor: FakeSocket as unknown as RWSocketCtor,
    pcFactory: () => {
      const pc = new FakePC();
      pcs.push(pc);
      return pc as unknown as RTCPeerConnection;
    },
    fetchToken: (overrides?.fetchToken ?? (async () => null)) as never,
    fetchIce: async () => ({ iceServers: [{ urls: "stun:test:3478" }] }),
    probe: (async () => null) as never,
  });
  return { session, pcs };
}

function lastSocket(): FakeSocket {
  const s = openSockets[openSockets.length - 1];
  if (!s) throw new Error("no socket dialed");
  return s;
}

beforeEach(() => {
  openSockets = [];
});

// ---- tests -----------------------------------------------------------

describe("ChromelessSession", () => {
  it("dials {base}/{sessionId} and sends the hello frame on open", async () => {
    const { session } = makeSession();
    await session.connect("dev-1");
    const ws = lastSocket();
    expect(ws.url).toBe("ws://test:8080/ws/dev-1");
    ws.triggerOpen();
    const hello = ws.sentEnvelopes()[0]!;
    expect(hello).toEqual({ type: "ice", from: "client", data: null });
  });

  it("answers an offer with the preferred codec leading the munged SDP", async () => {
    const { session, pcs } = makeSession();
    const outcomes: string[] = [];
    session.on("codecOutcome", (r) => outcomes.push(r.outcome));
    await session.connect("dev-2");
    const ws = lastSocket();
    ws.triggerOpen();

    ws.triggerMessage(JSON.stringify({
      type: "offer", from: "browser",
      data: { type: "offer", sdp: "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96 98\r\n" },
    } satisfies Envelope));
    await vi.waitFor(() => {
      expect(ws.sentEnvelopes().some((e) => e.type === "answer")).toBe(true);
    });

    const answer = ws.sentEnvelopes().find((e) => e.type === "answer")!;
    if (answer.type !== "answer") throw new Error("unreachable");
    // prioritizeCodec must have moved VP9 (PT 98) to the front.
    expect(answer.data.sdp).toContain("m=video 9 UDP/TLS/RTP/SAVPF 98 96");
    expect(pcs[0]!.localSet).toEqual({ type: "answer", sdp: answer.data.sdp });
    expect(outcomes).toEqual(["ok"]);
  });

  it("forwards non-stats data channels and keeps stats internal", async () => {
    const { session, pcs } = makeSession();
    const forwarded: string[] = [];
    session.on("dataChannel", (dc) => forwarded.push(dc.label));
    await session.connect("dev-3");
    lastSocket().triggerOpen();

    const mkDc = (label: string) => ({ label, readyState: "open", send: vi.fn(), close: vi.fn() });
    pcs[0]!.ondatachannel?.({ channel: mkDc("input") });
    pcs[0]!.ondatachannel?.({ channel: mkDc("stats") });
    pcs[0]!.ondatachannel?.({ channel: mkDc("cursor") });

    expect(forwarded).toEqual(["input", "cursor"]);
  });

  it("relays remote ICE, treats null as end-of-candidates", async () => {
    const { session, pcs } = makeSession();
    await session.connect("dev-4");
    const ws = lastSocket();
    ws.triggerOpen();

    ws.triggerMessage(JSON.stringify({
      type: "ice", from: "browser", data: { candidate: "candidate:1 1 udp 1 1.2.3.4 5 typ host" },
    } satisfies Envelope));
    await vi.waitFor(() => expect(pcs[0]!.addedCandidates.length).toBe(1));

    ws.triggerMessage(JSON.stringify({ type: "ice", from: "browser", data: null } satisfies Envelope));
    // Null marker must NOT be passed to addIceCandidate.
    expect(pcs[0]!.addedCandidates.length).toBe(1);
  });

  it("ignores frames marked from=client (echo guard)", async () => {
    const { session, pcs } = makeSession();
    await session.connect("dev-5");
    const ws = lastSocket();
    ws.triggerOpen();
    ws.triggerMessage(JSON.stringify({
      type: "ice", from: "client", data: { candidate: "candidate:x" },
    } satisfies Envelope));
    // Give the async handler a tick; nothing must land on the pc.
    await new Promise((r) => setTimeout(r, 0));
    expect(pcs[0]!.addedCandidates.length).toBe(0);
  });

  it("tears down on bye: closes pc, sends bye back, emits closed once", async () => {
    const { session, pcs } = makeSession();
    const closedReasons: string[] = [];
    session.on("closed", (reason) => closedReasons.push(reason));
    await session.connect("dev-6");
    const ws = lastSocket();
    ws.triggerOpen();

    ws.triggerMessage(JSON.stringify({ type: "bye", from: "browser" } satisfies Envelope));
    await vi.waitFor(() => expect(pcs[0]!.closed).toBe(true));
    const byes = ws.sentEnvelopes().filter((e) => e.type === "bye");
    expect(byes).toEqual([{ type: "bye", from: "client" }]);
    expect(closedReasons).toEqual(["peer said bye"]);
    expect(session.getPeerConnection()).toBeNull();
    // Double-disconnect stays silent.
    session.disconnect("again");
    expect(closedReasons).toEqual(["peer said bye"]);
  });

  it("rebuilds the peer connection on signaling reconnect", async () => {
    const { session, pcs } = makeSession();
    const created: number[] = [];
    session.on("pcCreated", () => created.push(pcs.length));
    await session.connect("dev-7");
    const ws1 = lastSocket();
    ws1.triggerOpen();
    expect(pcs.length).toBe(1);

    // Underlying close → ReconnectingWebSocket dials a new socket.
    ws1.readyState = FakeSocket.CLOSED;
    for (const l of ws1.listeners["close"]!) l({ wasClean: false, code: 1006, reason: "" });
    // Backoff timer is real setTimeout with a 1000ms initial delay
    // (reconnect.ts default) — vi.waitFor's own default timeout is
    // ALSO 1000ms, which races it. Widen the window.
    await vi.waitFor(() => expect(openSockets.length).toBe(2), { timeout: 4000 });
    const ws2 = lastSocket();
    ws2.triggerOpen();

    expect(pcs.length).toBe(2);
    expect(pcs[0]!.closed).toBe(true);
    expect(pcs[1]!.closed).toBe(false);
    expect(created.length).toBe(2);
    // Fresh hello on the new generation too.
    expect(ws2.sentEnvelopes()[0]).toEqual({ type: "ice", from: "client", data: null });
  });

  it("emits status transitions from pc connection state", async () => {
    const { session, pcs } = makeSession();
    const statuses: string[] = [];
    session.on("status", (s) => statuses.push(s));
    await session.connect("dev-8");
    lastSocket().triggerOpen();

    pcs[0]!.setConnectionState("connected");
    expect(statuses).toContain("connected");
    expect(session.isConnected()).toBe(true);

    pcs[0]!.setConnectionState("failed");
    expect(statuses).toContain("failed");
  });

  it("requestRenegotiate sends only while signaling is open", async () => {
    const { session } = makeSession();
    await session.connect("dev-9");
    const ws = lastSocket();
    // Not open yet → false, nothing sent.
    expect(session.requestRenegotiate("pre-open")).toBe(false);
    ws.triggerOpen();
    expect(session.requestRenegotiate("post-open")).toBe(true);
    const kinds = ws.sentEnvelopes().map((e) => e.type);
    expect(kinds).toContain("request_renegotiate");
  });

  it("connect() twice without disconnect throws", async () => {
    const { session } = makeSession();
    await session.connect("dev-10");
    await expect(session.connect("dev-10")).rejects.toThrow(/already connected/);
  });

  // The browser is the offerer, so an offer that never arrives used to hang
  // the client forever with no diagnosis: this class had no timeout at all.
  // The portal measured six of nine failing panels receiving 1000+ ICE
  // candidates and ZERO offers, so it is a real shape, not a hypothetical.
  describe("offer-wait watchdog", () => {
    it("asks for a fresh offer when none arrives, then fails visibly", async () => {
      vi.useFakeTimers();
      try {
        const { session } = makeSession();
        const statuses: Array<[string, string | undefined]> = [];
        session.on("status", (st, text) => statuses.push([st, text]));

        await session.connect("dev-11");
        const ws = lastSocket();
        ws.triggerOpen();

        // Nothing yet: the wait has to be generous enough to cover a cold
        // worker (~35s p99 in triform's measurements), or a normal slow boot
        // looks like a failure.
        vi.advanceTimersByTime(30_000);
        expect(ws.sentEnvelopes().some((e) => e.type === "request_renegotiate")).toBe(false);

        vi.advanceTimersByTime(20_000); // past the 45s wait
        expect(ws.sentEnvelopes().some((e) => e.type === "request_renegotiate")).toBe(true);
        expect(statuses.some(([, t]) => t === "no offer; retrying")).toBe(true);

        // Still nothing after the rescue window → report failure rather than
        // spinning forever.
        vi.advanceTimersByTime(25_000);
        expect(statuses.some(([st]) => st === "failed")).toBe(true);
      } finally {
        vi.useRealTimers();
      }
    });

    it("stays quiet when the offer arrives", async () => {
      vi.useFakeTimers();
      try {
        const { session } = makeSession();
        const statuses: string[] = [];
        session.on("status", (st) => statuses.push(st));

        await session.connect("dev-12");
        const ws = lastSocket();
        ws.triggerOpen();
        ws.triggerMessage(JSON.stringify({
          type: "offer", from: "browser",
          data: { type: "offer", sdp: "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n" },
        } satisfies Envelope));
        await vi.advanceTimersByTimeAsync(0);

        vi.advanceTimersByTime(120_000);
        expect(ws.sentEnvelopes().some((e) => e.type === "request_renegotiate")).toBe(false);
        expect(statuses).not.toContain("failed");
      } finally {
        vi.useRealTimers();
      }
    });

    // A timer surviving teardown would emit "failed" onto a session the
    // caller has already closed.
    it("does not fire after disconnect", async () => {
      vi.useFakeTimers();
      try {
        const { session } = makeSession();
        const statuses: string[] = [];
        session.on("status", (st) => statuses.push(st));

        await session.connect("dev-13");
        lastSocket().triggerOpen();
        session.disconnect("test");
        statuses.length = 0;

        vi.advanceTimersByTime(120_000);
        expect(statuses).toEqual([]);
      } finally {
        vi.useRealTimers();
      }
    });
  });
});

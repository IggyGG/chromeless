import { describe, it, expect } from "vitest";
import { fetchSessionToken, withToken, TokenRefresher } from "./auth.js";

describe("withToken", () => {
  it("appends ?token to a clean URL", () => {
    expect(withToken("ws://h/ws/abc", "TOK")).toBe("ws://h/ws/abc?token=TOK");
  });
  it("appends &token when query already present", () => {
    expect(withToken("ws://h/ws/abc?other=1", "T O K")).toBe("ws://h/ws/abc?other=1&token=T%20O%20K");
  });
});

describe("fetchSessionToken", () => {
  const base = "ws://localhost:8080/ws";

  it("returns null on fetch reject", async () => {
    const tok = await fetchSessionToken("dev", "client", {
      signalingBase: base,
      fetchImpl: () => Promise.reject(new Error("offline")),
    });
    expect(tok).toBeNull();
  });

  it("returns null on non-2xx", async () => {
    const tok = await fetchSessionToken("dev", "client", {
      signalingBase: base,
      fetchImpl: () => Promise.resolve(new Response("nope", { status: 401 })) as Promise<Response>,
    });
    expect(tok).toBeNull();
  });

  it("returns null on shape mismatch", async () => {
    const tok = await fetchSessionToken("dev", "client", {
      signalingBase: base,
      fetchImpl: () => Promise.resolve(new Response(JSON.stringify({ junk: 1 }), { status: 200, headers: { "Content-Type": "application/json" } })) as Promise<Response>,
    });
    expect(tok).toBeNull();
  });

  it("returns the parsed body on happy path", async () => {
    const body = { token: "x.y.z", exp: 12345, role: "client", sid: "dev", sub: "t" };
    const tok = await fetchSessionToken("dev", "client", {
      signalingBase: base,
      fetchImpl: () => Promise.resolve(new Response(JSON.stringify(body), { status: 200, headers: { "Content-Type": "application/json" } })) as Promise<Response>,
    });
    expect(tok).toEqual(body);
  });

  it("translates ws:// to http:// when issuing the request", async () => {
    let captured: string | URL = "";
    const fetchImpl: typeof fetch = (url) => {
      captured = url as string;
      return Promise.resolve(new Response(JSON.stringify({ token: "t", exp: 0, role: "client", sid: "s", sub: "u" }), { status: 200 })) as Promise<Response>;
    };
    await fetchSessionToken("s", "client", { signalingBase: base, fetchImpl });
    expect(String(captured)).toMatch(/^http:\/\/localhost:8080\/issue-token\?/);
    expect(String(captured)).toContain("role=client");
    expect(String(captured)).toContain("session_id=s");
  });
});

// ---------------------------------------------------------------------------
// T89 — TokenRefresher
// ---------------------------------------------------------------------------

describe("TokenRefresher", () => {
  /** Manual setTimeout/now scheduler for deterministic testing. */
  function manualClock() {
    let now = 0;
    const queue: Array<{ id: number; at: number; cb: () => void; cancelled: boolean }> = [];
    let nextId = 1;
    return {
      get now() { return now; },
      setTimeout: (cb: () => void, ms: number) => {
        const id = nextId++;
        queue.push({ id, at: now + ms, cb, cancelled: false });
        return id;
      },
      clearTimeout: (id: number) => {
        const e = queue.find(x => x.id === id);
        if (e) e.cancelled = true;
      },
      advance: async (ms: number) => {
        now += ms;
        // Drain in fire-time order. After each callback we let
        // several microtask cycles run so a `refresh()` chain that
        // awaits fetch + body parse + listener dispatch can settle
        // before we check the next deadline.
        while (true) {
          const ready = queue.filter(x => !x.cancelled && x.at <= now).sort((a, b) => a.at - b.at);
          if (ready.length === 0) break;
          const e = ready[0]!;
          e.cancelled = true;
          e.cb();
          for (let i = 0; i < 10; i++) await Promise.resolve();
        }
      },
    };
  }

  function tokFor(exp: number) {
    return new Response(
      JSON.stringify({ token: "t-" + exp, exp, role: "client", sid: "dev", sub: "tenant-a" }),
      { status: 200 },
    );
  }

  it("schedules a refresh leadMs before exp and re-fetches", async () => {
    const clock = manualClock();
    const fetched: number[] = [];
    let nextExp = 1000; // exp at t=1_000_000 ms
    const fetchImpl: typeof fetch = () => {
      fetched.push(clock.now);
      const e = nextExp;
      nextExp += 600; // every refresh extends by 10 min wall-clock
      return Promise.resolve(tokFor(e)) as Promise<Response>;
    };
    const r = new TokenRefresher("dev", "client", {
      fetchImpl,
      refreshLeadMs: 60_000,
      setTimeout: clock.setTimeout,
      clearTimeout: clock.clearTimeout,
      now: () => clock.now,
    });
    await r.start();
    expect(fetched).toEqual([0]); // initial
    expect(r.current()?.token).toBe("t-1000");

    // Initial token: exp=1000s, lead=60s, so refresh at t=940_000 ms.
    await clock.advance(940_000);
    expect(fetched).toEqual([0, 940_000]);
    expect(r.current()?.token).toBe("t-1600");

    r.stop();
  });

  it("notifies listeners on refresh", async () => {
    const clock = manualClock();
    let exp = 100;
    const fetchImpl: typeof fetch = () => {
      const e = exp;
      exp += 100;
      return Promise.resolve(tokFor(e)) as Promise<Response>;
    };
    const r = new TokenRefresher("dev", "client", {
      fetchImpl, refreshLeadMs: 10_000,
      setTimeout: clock.setTimeout, clearTimeout: clock.clearTimeout, now: () => clock.now,
    });
    const seen: string[] = [];
    r.onRefresh((t) => seen.push(t.token));
    await r.start();
    // First exp=100s; lead=10s; refresh at t=90_000.
    await clock.advance(90_000);
    expect(seen).toEqual(["t-200"]);
    r.stop();
  });

  it("retries 30s after a failed refresh", async () => {
    const clock = manualClock();
    let calls = 0;
    const fetchImpl: typeof fetch = () => {
      calls++;
      if (calls === 1) return Promise.resolve(tokFor(60)) as Promise<Response>;
      if (calls === 2) return Promise.reject(new Error("offline")); // refresh fails
      return Promise.resolve(tokFor(120)) as Promise<Response>;
    };
    const r = new TokenRefresher("dev", "client", {
      fetchImpl, refreshLeadMs: 10_000,
      setTimeout: clock.setTimeout, clearTimeout: clock.clearTimeout, now: () => clock.now,
    });
    await r.start();
    // exp=60s, lead=10s ⇒ refresh at t=50_000.
    await clock.advance(50_000);
    expect(calls).toBe(2);
    // Failed refresh schedules retry at +30_000.
    await clock.advance(30_000);
    expect(calls).toBe(3);
    expect(r.current()?.token).toBe("t-120");
    r.stop();
  });

  it("stop() cancels the next refresh", async () => {
    const clock = manualClock();
    const fetchImpl: typeof fetch = () => Promise.resolve(tokFor(60)) as Promise<Response>;
    const r = new TokenRefresher("dev", "client", {
      fetchImpl, refreshLeadMs: 10_000,
      setTimeout: clock.setTimeout, clearTimeout: clock.clearTimeout, now: () => clock.now,
    });
    let refreshes = 0;
    r.onRefresh(() => refreshes++);
    await r.start();
    r.stop();
    await clock.advance(1_000_000);
    expect(refreshes).toBe(0);
  });

  it("returns null and schedules nothing when initial fetch fails", async () => {
    const clock = manualClock();
    const fetchImpl: typeof fetch = () => Promise.reject(new Error("offline"));
    const r = new TokenRefresher("dev", "client", {
      fetchImpl, refreshLeadMs: 10_000,
      setTimeout: clock.setTimeout, clearTimeout: clock.clearTimeout, now: () => clock.now,
    });
    const got = await r.start();
    expect(got).toBeNull();
    expect(r.current()).toBeNull();
    let fired = 0;
    r.onRefresh(() => fired++);
    await clock.advance(1_000_000);
    expect(fired).toBe(0);
    r.stop();
  });
});

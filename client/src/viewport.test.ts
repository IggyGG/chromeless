import { describe, it, expect, vi, afterEach } from "vitest";
import { evenDown, requestViewport, followViewport, type ViewportResult } from "./viewport.js";

describe("evenDown", () => {
  it("rounds down to an even integer, never below zero", () => {
    expect(evenDown(1281.7)).toBe(1280);
    expect(evenDown(721)).toBe(720);
    expect(evenDown(0.5)).toBe(0);
    expect(evenDown(-4)).toBe(0);
  });
});

describe("requestViewport", () => {
  afterEach(() => vi.unstubAllGlobals());

  it("posts even CSS sizes with the session cookie and reports the applied size", async () => {
    const calls: Array<{ path: string; init: RequestInit }> = [];
    vi.stubGlobal("fetch", ((path: string, init: RequestInit) => {
      calls.push({ path, init });
      return Promise.resolve(new Response(JSON.stringify({ width: 1280, height: 720, deviceScaleFactor: 1 }), { status: 200 }));
    }) as typeof fetch);

    const r = await requestViewport(1281, 721);
    expect(calls[0]?.path).toBe("/api/viewport");
    expect(calls[0]?.init.credentials).toBe("same-origin");
    expect(JSON.parse(String(calls[0]?.init.body))).toEqual({ width: 1280, height: 720 });
    expect(r).toEqual({ ok: true, applied: { width: 1280, height: 720, deviceScaleFactor: 1 }, clamped: false });
  });

  // The guest clamps to [200, 2560]; a 4K stage comes back smaller than asked.
  // The caller must learn that, not assume its request landed.
  it("flags a clamped response", async () => {
    vi.stubGlobal("fetch", (() =>
      Promise.resolve(new Response(JSON.stringify({ width: 2560, height: 2160, deviceScaleFactor: 1 }), { status: 200 }))
    ) as typeof fetch);
    const r = await requestViewport(3840, 2160);
    expect(r.ok && r.clamped).toBe(true);
  });

  it("surfaces the gateway's error text", async () => {
    vi.stubGlobal("fetch", (() =>
      Promise.resolve(new Response(JSON.stringify({ error: "Cb.setViewport: 'Cb.setViewport' wasn't found" }), { status: 502 }))
    ) as typeof fetch);
    const r = await requestViewport(800, 600);
    expect(r).toEqual({ ok: false, error: "Cb.setViewport: 'Cb.setViewport' wasn't found" });
  });

  it("refuses an empty stage without a network call", async () => {
    const fetchSpy = vi.fn();
    vi.stubGlobal("fetch", fetchSpy);
    expect(await requestViewport(0, 600)).toEqual({ ok: false, error: "empty stage" });
    expect(fetchSpy).not.toHaveBeenCalled();
  });
});

// A deterministic harness: a fake observer we drive by hand, fake timers we
// fire by hand, and a fake request that records what it was asked.
function harness(results: ViewportResult[]) {
  let resizeCb: ((w: number, h: number) => void) | null = null;
  const timers: Array<() => void> = [];
  const asked: Array<{ w: number; h: number }> = [];
  const stage = { getBoundingClientRect: () => ({ width: 1000, height: 500 }) } as unknown as Element;
  const follower = followViewport(stage, {
    observe: (_el, cb) => { resizeCb = cb; return () => { resizeCb = null; }; },
    setTimer: (cb) => { timers.push(cb); return timers.length; },
    clearTimer: (id) => { timers[(id as number) - 1] = () => {}; },
    request: async (w, h) => { asked.push({ w, h }); return results.shift() ?? { ok: false, error: "no scripted result" }; },
  });
  const resize = (w: number, h: number) => resizeCb?.(w, h);
  const flushTimers = async () => { const t = timers.splice(0); for (const cb of t) cb(); await Promise.resolve(); await Promise.resolve(); };
  return { follower, resize, flushTimers, asked };
}

describe("followViewport", () => {
  const ok = (w: number, h: number): ViewportResult => ({ ok: true, applied: { width: w, height: h, deviceScaleFactor: 1 }, clamped: false });

  it("debounces a burst of resizes into one request for the last size", async () => {
    const h = harness([ok(1600, 900)]);
    h.resize(1200, 700);
    h.resize(1400, 800);
    h.resize(1601, 901);
    await h.flushTimers();
    expect(h.asked).toEqual([{ w: 1600, h: 900 }]);
  });

  it("does not repeat a size the guest already confirmed", async () => {
    const h = harness([ok(1600, 900)]);
    h.resize(1600, 900);
    await h.flushTimers();
    h.resize(1600, 900);
    await h.flushTimers();
    expect(h.asked).toHaveLength(1);
  });

  // An old guest without Cb.setViewport must cost ONE failed request, not one
  // per resize event for the rest of the session.
  it("stops after a failure and resumes only when asked", async () => {
    const h = harness([{ ok: false, error: "wasn't found" }, ok(800, 600)]);
    h.resize(1600, 900);
    await h.flushTimers();
    h.resize(800, 600);
    await h.flushTimers();
    expect(h.asked).toHaveLength(1);
    h.follower.resume();
    await h.flushTimers();
    expect(h.asked).toHaveLength(2);
  });

  it("sync() sends the stage's current size immediately", async () => {
    const h = harness([ok(1000, 500)]);
    h.follower.sync();
    await Promise.resolve();
    expect(h.asked).toEqual([{ w: 1000, h: 500 }]);
  });

  it("dispose() detaches and cancels a pending request", async () => {
    const h = harness([ok(1, 1)]);
    h.resize(1600, 900);
    h.follower.dispose();
    await h.flushTimers();
    expect(h.asked).toHaveLength(0);
  });
});

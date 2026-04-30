import { describe, it, expect } from "vitest";
import {
  estimateConnectionQuality,
  resolveProbeURL,
  probeToMaxBitrateBps,
} from "./probe.js";

describe("resolveProbeURL", () => {
  it("defaults to a same-origin path", () => {
    expect(resolveProbeURL(undefined)).toBe("/probe");
    expect(resolveProbeURL("")).toBe("/probe");
  });
  it("rewrites ws:// → http://", () => {
    expect(resolveProbeURL("ws://localhost:8080/ws")).toBe("http://localhost:8080/probe");
  });
  it("rewrites wss:// → https://", () => {
    expect(resolveProbeURL("wss://signaling.example.com/ws")).toBe("https://signaling.example.com/probe");
  });
  it("appends ?token= when authToken is provided", () => {
    expect(resolveProbeURL("https://h", "tok")).toBe("https://h/probe?token=tok");
  });
  it("URL-encodes the token", () => {
    expect(resolveProbeURL("https://h", "a&b=c")).toContain("token=a%26b%3Dc");
  });
});

describe("probeToMaxBitrateBps", () => {
  it("clamps to the 200 kbps floor", () => {
    expect(
      probeToMaxBitrateBps({ rtt_ms: 30, downlink_kbps: 100, uplink_kbps: 50, samples: 5 }),
    ).toBe(200_000);
  });
  it("clamps to the 8 Mbps ceiling", () => {
    expect(
      probeToMaxBitrateBps({
        rtt_ms: 5,
        downlink_kbps: 100_000,
        uplink_kbps: 100_000,
        samples: 5,
      }),
    ).toBe(8_000_000);
  });
  it("applies the 0.8 headroom multiplier on the lower of up/down", () => {
    // min(10000, 5000) = 5000 kbps × 0.8 = 4000 kbps = 4_000_000 bps.
    expect(
      probeToMaxBitrateBps({
        rtt_ms: 30,
        downlink_kbps: 10_000,
        uplink_kbps: 5_000,
        samples: 5,
      }),
    ).toBe(4_000_000);
  });
});

describe("estimateConnectionQuality", () => {
  // Build a fake fetch that returns octet-stream of N zero-bytes for GET
  // and {received: <len>} for POST. now() advances by a fixed amount per
  // call so we get deterministic timings.
  function makeFakeStack({
    rttMs = 20,
    bandwidthMs = 200,
    bandwidthBytes = 256 * 1024,
  }: { rttMs?: number; bandwidthMs?: number; bandwidthBytes?: number } = {}) {
    let t = 0;
    const now = () => t;
    const calls: Array<{ url: string; method: string }> = [];
    const fetchImpl: typeof fetch = (async (
      input: RequestInfo | URL,
      init?: RequestInit,
    ) => {
      const url = typeof input === "string" ? input : input.toString();
      const method = init?.method ?? "GET";
      calls.push({ url, method });
      // Decide whether this is a small (RTT) or large (bandwidth) probe.
      const sizeMatch = /[?&]size=(\d+)/.exec(url);
      const wantedBytes = sizeMatch ? parseInt(sizeMatch[1]!, 10) : 0;
      const isBandwidthGet = method === "GET" && wantedBytes >= bandwidthBytes;
      const isPost = method === "POST";
      // Advance the clock to model the round-trip duration.
      t += isBandwidthGet || isPost ? bandwidthMs : rttMs;
      const body =
        method === "GET"
          ? new ArrayBuffer(wantedBytes)
          : new TextEncoder().encode(JSON.stringify({ received: bandwidthBytes })).buffer;
      return {
        ok: true,
        status: 200,
        arrayBuffer: async () => body,
      } as unknown as Response;
    }) as typeof fetch;
    return { fetchImpl, now, calls };
  }

  it("returns sane numbers given good responses", async () => {
    const { fetchImpl, now, calls } = makeFakeStack({
      rttMs: 50,
      bandwidthMs: 200,
      bandwidthBytes: 256 * 1024,
    });
    const r = await estimateConnectionQuality({
      signalingBase: "http://h",
      fetchImpl,
      nowMs: now,
      durationMs: 30_000,
      rttSamples: 5,
      bandwidthBytes: 256 * 1024,
    });
    expect(r).not.toBeNull();
    if (!r) return;
    expect(r.rtt_ms).toBe(50);
    expect(r.samples).toBe(5);
    // 256KiB in 200ms = 256*1024*8 / 0.2 = ~10485760 bps ≈ 10485 kbps.
    expect(r.downlink_kbps).toBeGreaterThan(8_000);
    expect(r.downlink_kbps).toBeLessThan(12_000);
    expect(r.uplink_kbps).toBeGreaterThan(8_000);
    expect(r.uplink_kbps).toBeLessThan(12_000);
    // 5 RTT GETs + 1 bandwidth GET + 1 POST = 7 calls.
    expect(calls.length).toBe(7);
  });

  it("returns null on HTTP error", async () => {
    const fetchImpl: typeof fetch = (async () => ({
      ok: false,
      status: 500,
      arrayBuffer: async () => new ArrayBuffer(0),
    })) as unknown as typeof fetch;
    const r = await estimateConnectionQuality({
      signalingBase: "http://h",
      fetchImpl,
    });
    expect(r).toBeNull();
  });

  it("returns null when fetch throws", async () => {
    const fetchImpl: typeof fetch = (async () => {
      throw new Error("network down");
    }) as unknown as typeof fetch;
    const r = await estimateConnectionQuality({
      signalingBase: "http://h",
      fetchImpl,
    });
    expect(r).toBeNull();
  });

  it("appends the auth token to every request when set", async () => {
    const { fetchImpl, now, calls } = makeFakeStack();
    await estimateConnectionQuality({
      signalingBase: "http://h",
      authToken: "abc.def.ghi",
      fetchImpl,
      nowMs: now,
      rttSamples: 2,
      bandwidthBytes: 1024,
    });
    expect(calls.length).toBeGreaterThan(0);
    for (const c of calls) {
      expect(c.url).toContain("token=abc.def.ghi");
    }
  });

  it("stops after durationMs even if more samples were requested", async () => {
    // Each fetch takes 100 ms in fake-time; budget = 250 ms allows ~2 RTTs
    // before the loop notices the deadline and returns null (we never reach
    // bandwidth probes, so the result is null).
    const { fetchImpl, now } = makeFakeStack({ rttMs: 100, bandwidthMs: 100 });
    const r = await estimateConnectionQuality({
      signalingBase: "http://h",
      fetchImpl,
      nowMs: now,
      durationMs: 250,
      rttSamples: 100,
    });
    expect(r).toBeNull();
  });
});

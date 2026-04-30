import { describe, it, expect } from "vitest";
import { StatsSampler, StatsSample, formatSummary, STATS_PROTOCOL_VERSION } from "./stats.js";

/** Minimal fake RTCStatsReport that supports forEach. */
function makeReport(entries: Array<Record<string, unknown>>): RTCStatsReport {
  const map = new Map<string, Record<string, unknown>>();
  for (const e of entries) map.set(String(e["id"] ?? Math.random()), e);
  return {
    forEach: (cb: (value: any, key: string, parent: any) => void) => {
      for (const [k, v] of map.entries()) cb(v, k, map);
    },
    size: map.size,
    [Symbol.iterator]: () => map.entries(),
  } as unknown as RTCStatsReport;
}

const VIDEO_INBOUND = {
  id: "RTCInboundRtp_video",
  type: "inbound-rtp",
  kind: "video",
  bytesReceived: 100_000,
  packetsReceived: 200,
  packetsLost: 1,
  jitter: 0.005,
  framesPerSecond: 30,
  framesDropped: 0,
  framesReceived: 60,
  totalDecodeTime: 0.42,
};
const AUDIO_OUTBOUND = {
  id: "RTCOutboundRtp_audio",
  type: "outbound-rtp",
  kind: "audio",
  bytesSent: 50_000,
  packetsSent: 80,
  framesPerSecond: null,
  framesEncoded: null,
  totalEncodeTime: 0,
  qualityLimitationReason: null,
};
const PAIR_SELECTED = {
  id: "PAIR_1",
  type: "candidate-pair",
  selected: true,
  localCandidateId: "L1",
  remoteCandidateId: "R1",
  currentRoundTripTime: 0.072,
  availableOutgoingBitrate: 1_500_000,
  availableIncomingBitrate: 4_000_000,
};
const LOCAL_CAND = { id: "L1", type: "local-candidate", candidateType: "host" };
const REMOTE_CAND = { id: "R1", type: "remote-candidate", candidateType: "srflx" };

describe("StatsSampler.extract", () => {
  it("extracts inbound, outbound, and selected candidate-pair", () => {
    const sampler = new StatsSampler({ getStats: async () => makeReport([]) });
    const sample = sampler.extract(makeReport([VIDEO_INBOUND, AUDIO_OUTBOUND, PAIR_SELECTED, LOCAL_CAND, REMOTE_CAND]));
    expect(sample.v).toBe(STATS_PROTOCOL_VERSION);
    expect(sample.inbound).toHaveLength(1);
    expect(sample.inbound[0]).toMatchObject({ kind: "video", bytesReceived: 100_000, framesPerSecond: 30 });
    expect(sample.outbound).toHaveLength(1);
    expect(sample.outbound[0]).toMatchObject({ kind: "audio", bytesSent: 50_000 });
    expect(sample.candidatePair).toMatchObject({
      currentRoundTripTime: 0.072,
      localCandidateType: "host",
      remoteCandidateType: "srflx",
    });
  });

  it("returns null candidatePair when no pair is selected", () => {
    const sampler = new StatsSampler({ getStats: async () => makeReport([]) });
    const sample = sampler.extract(makeReport([VIDEO_INBOUND]));
    expect(sample.candidatePair).toBeNull();
  });

  it("falls back to transport.selectedCandidatePairId when no pair has selected:true", () => {
    const sampler = new StatsSampler({ getStats: async () => makeReport([]) });
    const pairNominated = { ...PAIR_SELECTED, selected: false, nominated: false };
    const transport = { id: "T1", type: "transport", selectedCandidatePairId: "PAIR_1" };
    const sample = sampler.extract(makeReport([pairNominated, LOCAL_CAND, REMOTE_CAND, transport]));
    expect(sample.candidatePair).not.toBeNull();
    expect(sample.candidatePair!.localCandidateType).toBe("host");
  });

  it("coerces missing/non-number fields to 0 or null appropriately", () => {
    const sampler = new StatsSampler({ getStats: async () => makeReport([]) });
    const partial = { id: "x", type: "inbound-rtp", kind: "video" };
    const sample = sampler.extract(makeReport([partial]));
    expect(sample.inbound[0]).toMatchObject({
      bytesReceived: 0, packetsReceived: 0, packetsLost: 0, jitter: 0,
      framesPerSecond: null, framesDropped: null, framesReceived: null, totalDecodeTime: null,
    });
  });

  it("extracts remote-inbound-rtp with RTT", () => {
    const sampler = new StatsSampler({ getStats: async () => makeReport([]) });
    const remoteInb = {
      id: "RemoteInbound_video",
      type: "remote-inbound-rtp",
      kind: "video",
      roundTripTime: 0.08,
      packetsLost: 5,
      fractionLost: 0.01,
    };
    const sample = sampler.extract(makeReport([remoteInb]));
    expect(sample.remoteInbound).toHaveLength(1);
    expect(sample.remoteInbound[0]).toMatchObject({ kind: "video", roundTripTime: 0.08, packetsLost: 5 });
  });
});

describe("StatsSampler.start/stop", () => {
  it("samples on start and at interval", async () => {
    let now = 0;
    const ticks: Array<{ ms: number; cb: () => void }> = [];
    const sampler = new StatsSampler(
      { getStats: async () => makeReport([VIDEO_INBOUND]) },
      {
        intervalMs: 1000,
        now: () => now,
        setInterval: (cb, ms) => { ticks.push({ ms, cb }); return ticks.length; },
        clearInterval: (id) => { ticks[id - 1] = { ms: 0, cb: () => {} }; },
      },
    );
    const samples: StatsSample[] = [];
    sampler.on(s => samples.push(s));
    sampler.start();
    // Immediate sample is async; wait a tick.
    await new Promise(r => setTimeout(r, 0));
    expect(samples).toHaveLength(1);
    // Tick the interval.
    now = 1000;
    ticks[0]!.cb();
    await new Promise(r => setTimeout(r, 0));
    expect(samples).toHaveLength(2);
    sampler.stop();
  });

  it("returns null and stays alive when getStats rejects", async () => {
    const sampler = new StatsSampler({ getStats: () => Promise.reject(new Error("nope")) });
    const got = await sampler.sampleOnce();
    expect(got).toBeNull();
  });
});

describe("formatSummary", () => {
  it("computes kbps from byte deltas across two samples", () => {
    const sampler = new StatsSampler({ getStats: async () => makeReport([]) });
    const a = sampler.extract(makeReport([VIDEO_INBOUND, AUDIO_OUTBOUND, PAIR_SELECTED, LOCAL_CAND, REMOTE_CAND]));
    a.t = 0;
    const inboundLater = { ...VIDEO_INBOUND, bytesReceived: 100_000 + 125_000 }; // +1000 kbit/s over 1s
    const outboundVideoLater = {
      ...AUDIO_OUTBOUND,
      id: "RTCOutboundRtp_video",
      kind: "video",
      bytesSent: 0 + 25_000, // +200 kbit/s over 1s (assuming first sample's outbound had 0 video bytes)
      framesPerSecond: 30,
    };
    // Need first-sample outbound video too for delta:
    const firstOutboundVideo = { ...AUDIO_OUTBOUND, id: "RTCOutboundRtp_video", kind: "video", bytesSent: 0, framesPerSecond: 30 };
    const a2 = sampler.extract(makeReport([VIDEO_INBOUND, firstOutboundVideo, PAIR_SELECTED, LOCAL_CAND, REMOTE_CAND]));
    a2.t = 0;
    const b = sampler.extract(makeReport([inboundLater, outboundVideoLater, PAIR_SELECTED, LOCAL_CAND, REMOTE_CAND]));
    b.t = 1000;
    const line = formatSummary(b, a2);
    expect(line).toContain("rtt=72ms");
    expect(line).toContain("fps=30");
    expect(line).toContain("in=1000kb/s");
    expect(line).toContain("out=200kb/s");
    expect(line).toContain("lost=1");
  });

  it("uses placeholders when no previous sample", () => {
    const sampler = new StatsSampler({ getStats: async () => makeReport([]) });
    const s = sampler.extract(makeReport([VIDEO_INBOUND, PAIR_SELECTED, LOCAL_CAND, REMOTE_CAND]));
    const line = formatSummary(s);
    expect(line).toContain("in=—kb/s");
    expect(line).toContain("out=—kb/s");
  });
});

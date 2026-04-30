// Periodic RTCPeerConnection.getStats() sampler.
//
// Pulls the subset of stats we actually use: per-track inbound/outbound
// RTP, the selected ICE candidate-pair, and remote-inbound-rtp for
// upstream RTT. Emits a flat StatsSample to subscribers (the debug
// panel) and, optionally, over a "stats" data channel for the server
// to scrape. See docs/protocols/stats-channel.md.
//
// Pure logic; tests inject getStats() and a manual clock.

export const STATS_PROTOCOL_VERSION = 1 as const;

export interface InboundStats {
  trackId: string;
  kind: "audio" | "video";
  bytesReceived: number;
  packetsReceived: number;
  packetsLost: number;
  jitter: number;
  framesPerSecond: number | null;
  framesDropped: number | null;
  framesReceived: number | null;
  /** seconds, cumulative; consumer computes deltas. */
  totalDecodeTime: number | null;
}

export interface OutboundStats {
  trackId: string;
  kind: "audio" | "video";
  bytesSent: number;
  packetsSent: number;
  framesPerSecond: number | null;
  framesEncoded: number | null;
  totalEncodeTime: number | null;
  qualityLimitationReason: string | null;
}

export interface RemoteInboundStats {
  trackId: string;
  kind: "audio" | "video";
  roundTripTime: number | null;
  packetsLost: number | null;
  fractionLost: number | null;
}

export interface CandidatePairStats {
  currentRoundTripTime: number | null;
  availableOutgoingBitrate: number | null;
  availableIncomingBitrate: number | null;
  localCandidateType: string | null;
  remoteCandidateType: string | null;
}

export interface StatsSample {
  v: typeof STATS_PROTOCOL_VERSION;
  t: number;
  inbound: InboundStats[];
  outbound: OutboundStats[];
  remoteInbound: RemoteInboundStats[];
  candidatePair: CandidatePairStats | null;
}

/** Subset of RTCPeerConnection we depend on. */
export interface StatsSource {
  getStats(): Promise<RTCStatsReport>;
}

export interface StatsSamplerOptions {
  intervalMs?: number;
  now?: () => number;
  setInterval?: (cb: () => void, ms: number) => number;
  clearInterval?: (id: number) => void;
}

export class StatsSampler {
  private readonly src: StatsSource;
  private readonly opts: Required<StatsSamplerOptions>;
  private timer: number | null = null;
  private subs = new Set<(s: StatsSample) => void>();

  constructor(src: StatsSource, opts: StatsSamplerOptions = {}) {
    this.src = src;
    this.opts = {
      intervalMs: opts.intervalMs ?? 1000,
      now: opts.now ?? Date.now,
      setInterval: opts.setInterval ?? ((cb, ms) => globalThis.setInterval(cb, ms) as unknown as number),
      clearInterval: opts.clearInterval ?? ((id) => globalThis.clearInterval(id)),
    };
  }

  on(listener: (s: StatsSample) => void): () => void {
    this.subs.add(listener);
    return () => { this.subs.delete(listener); };
  }

  start(): void {
    if (this.timer !== null) return;
    // Fire one sample immediately; subsequent ones on the interval.
    void this.sampleOnce();
    this.timer = this.opts.setInterval(() => { void this.sampleOnce(); }, this.opts.intervalMs);
  }

  stop(): void {
    if (this.timer === null) return;
    this.opts.clearInterval(this.timer);
    this.timer = null;
  }

  /** Take one sample without scheduling. Public for tests. */
  async sampleOnce(): Promise<StatsSample | null> {
    let report: RTCStatsReport;
    try {
      report = await this.src.getStats();
    } catch {
      return null;
    }
    const sample = this.extract(report);
    for (const l of this.subs) l(sample);
    return sample;
  }

  /** Pure transform; exported for tests. */
  extract(report: RTCStatsReport): StatsSample {
    const inbound: InboundStats[] = [];
    const outbound: OutboundStats[] = [];
    const remoteInbound: RemoteInboundStats[] = [];

    let selectedPairId: string | null = null;
    const candidatePairs = new Map<string, RTCStats & Record<string, unknown>>();
    const localCandidates = new Map<string, RTCStats & Record<string, unknown>>();
    const remoteCandidates = new Map<string, RTCStats & Record<string, unknown>>();
    const transports: Array<RTCStats & Record<string, unknown>> = [];

    report.forEach((entry) => {
      const e = entry as RTCStats & Record<string, unknown>;
      switch (e.type) {
        case "inbound-rtp": {
          const kind = (e["kind"] as string) === "audio" ? "audio" : "video";
          inbound.push({
            trackId: String(e["id"] ?? ""),
            kind,
            bytesReceived: numOr0(e["bytesReceived"]),
            packetsReceived: numOr0(e["packetsReceived"]),
            packetsLost: numOr0(e["packetsLost"]),
            jitter: numOr0(e["jitter"]),
            framesPerSecond: numOrNull(e["framesPerSecond"]),
            framesDropped: numOrNull(e["framesDropped"]),
            framesReceived: numOrNull(e["framesReceived"]),
            totalDecodeTime: numOrNull(e["totalDecodeTime"]),
          });
          break;
        }
        case "outbound-rtp": {
          const kind = (e["kind"] as string) === "audio" ? "audio" : "video";
          outbound.push({
            trackId: String(e["id"] ?? ""),
            kind,
            bytesSent: numOr0(e["bytesSent"]),
            packetsSent: numOr0(e["packetsSent"]),
            framesPerSecond: numOrNull(e["framesPerSecond"]),
            framesEncoded: numOrNull(e["framesEncoded"]),
            totalEncodeTime: numOrNull(e["totalEncodeTime"]),
            qualityLimitationReason: typeof e["qualityLimitationReason"] === "string" ? (e["qualityLimitationReason"] as string) : null,
          });
          break;
        }
        case "remote-inbound-rtp": {
          const kind = (e["kind"] as string) === "audio" ? "audio" : "video";
          remoteInbound.push({
            trackId: String(e["id"] ?? ""),
            kind,
            roundTripTime: numOrNull(e["roundTripTime"]),
            packetsLost: numOrNull(e["packetsLost"]),
            fractionLost: numOrNull(e["fractionLost"]),
          });
          break;
        }
        case "candidate-pair":
          candidatePairs.set(String(e["id"] ?? ""), e);
          break;
        case "local-candidate":
          localCandidates.set(String(e["id"] ?? ""), e);
          break;
        case "remote-candidate":
          remoteCandidates.set(String(e["id"] ?? ""), e);
          break;
        case "transport":
          transports.push(e);
          break;
      }
    });

    // Selected pair: prefer the per-pair `selected: true` (legacy); fall
    // back to the transport's selectedCandidatePairId.
    for (const [id, p] of candidatePairs.entries()) {
      if (p["selected"] === true || p["nominated"] === true) {
        selectedPairId = id;
        if (p["selected"] === true) break;
      }
    }
    if (!selectedPairId) {
      for (const tr of transports) {
        const sid = tr["selectedCandidatePairId"];
        if (typeof sid === "string" && candidatePairs.has(sid)) {
          selectedPairId = sid;
          break;
        }
      }
    }

    let candidatePair: CandidatePairStats | null = null;
    if (selectedPairId) {
      const p = candidatePairs.get(selectedPairId)!;
      const local = typeof p["localCandidateId"] === "string" ? localCandidates.get(p["localCandidateId"] as string) : undefined;
      const remote = typeof p["remoteCandidateId"] === "string" ? remoteCandidates.get(p["remoteCandidateId"] as string) : undefined;
      candidatePair = {
        currentRoundTripTime: numOrNull(p["currentRoundTripTime"]),
        availableOutgoingBitrate: numOrNull(p["availableOutgoingBitrate"]),
        availableIncomingBitrate: numOrNull(p["availableIncomingBitrate"]),
        localCandidateType: local && typeof local["candidateType"] === "string" ? (local["candidateType"] as string) : null,
        remoteCandidateType: remote && typeof remote["candidateType"] === "string" ? (remote["candidateType"] as string) : null,
      };
    }

    return {
      v: STATS_PROTOCOL_VERSION,
      t: this.opts.now(),
      inbound,
      outbound,
      remoteInbound,
      candidatePair,
    };
  }
}

function numOr0(v: unknown): number {
  return typeof v === "number" && Number.isFinite(v) ? v : 0;
}
function numOrNull(v: unknown): number | null {
  return typeof v === "number" && Number.isFinite(v) ? v : null;
}

/** Compute a compact one-line summary: rtt, fps, kbps_in/out, lost. */
export function formatSummary(s: StatsSample, prev?: StatsSample): string {
  const rtt = s.candidatePair?.currentRoundTripTime;
  const inb = s.inbound.find(x => x.kind === "video");
  const out = s.outbound.find(x => x.kind === "video");
  let kbpsIn = "—", kbpsOut = "—";
  if (prev && inb) {
    const prevInb = prev.inbound.find(x => x.kind === "video" && x.trackId === inb.trackId);
    if (prevInb) {
      const dt = (s.t - prev.t) / 1000;
      const dB = inb.bytesReceived - prevInb.bytesReceived;
      if (dt > 0) kbpsIn = ((dB * 8) / 1000 / dt).toFixed(0);
    }
  }
  if (prev && out) {
    const prevOut = prev.outbound.find(x => x.kind === "video" && x.trackId === out.trackId);
    if (prevOut) {
      const dt = (s.t - prev.t) / 1000;
      const dB = out.bytesSent - prevOut.bytesSent;
      if (dt > 0) kbpsOut = ((dB * 8) / 1000 / dt).toFixed(0);
    }
  }
  const fps = inb?.framesPerSecond ?? out?.framesPerSecond ?? null;
  const lost = inb?.packetsLost ?? null;
  const rttMs = typeof rtt === "number" ? (rtt * 1000).toFixed(0) : "—";
  return `rtt=${rttMs}ms fps=${fps ?? "—"} in=${kbpsIn}kb/s out=${kbpsOut}kb/s lost=${lost ?? "—"}`;
}

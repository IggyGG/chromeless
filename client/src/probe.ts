// T102: pre-call connection-quality probe.
//
// Runs HTTP probes against the signaling server's /probe endpoint to
// estimate RTT and rough up/down throughput before WebRTC negotiation.
// The streamer uses the result to seed its initial encoder bitrate
// ceiling so the first few seconds aren't degraded while libwebrtc's
// BWE ramps up.
//
// Best-effort by design: any failure resolves to null and the caller
// proceeds without the probe_result envelope. See
// docs/protocols/probe-protocol.md for the full design.

export interface ProbeResult {
  rtt_ms: number;
  downlink_kbps: number;
  uplink_kbps: number;
  samples: number;
}

export interface EstimateOptions {
  /** Same shape as turn.ts: `ws://host/ws` or `http://host`. */
  signalingBase?: string;
  /** Auth token (T48). Appended as ?token=… when set. */
  authToken?: string;
  /**
   * Wall-clock budget for the entire probe. We never run longer than
   * this; if a single sub-fetch hangs, AbortController kills it.
   * Default 2000 ms — about half the time-to-first-frame budget.
   */
  durationMs?: number;
  /**
   * Number of small RTT samples to take before the bandwidth probes.
   * Default 5. Median is reported.
   */
  rttSamples?: number;
  /** Bytes for the bandwidth GET/POST. Default 256 KiB each. */
  bandwidthBytes?: number;
  /** Override fetch (for tests). */
  fetchImpl?: typeof fetch;
  /** Override now() (for tests). */
  nowMs?: () => number;
}

const DEFAULT_DURATION_MS = 2000;
const DEFAULT_RTT_SAMPLES = 5;
const DEFAULT_BANDWIDTH_BYTES = 256 * 1024;
const RTT_PROBE_SIZE = 1; // single byte; we want pure RTT.

/**
 * Estimate connection quality. Returns null on any error — the caller
 * should treat null as "no hint, use defaults."
 */
export async function estimateConnectionQuality(
  opts: EstimateOptions = {},
): Promise<ProbeResult | null> {
  const fetchImpl = opts.fetchImpl ?? fetch;
  const now = opts.nowMs ?? (() => performance.now());
  const durationMs = opts.durationMs ?? DEFAULT_DURATION_MS;
  const rttSamples = Math.max(1, opts.rttSamples ?? DEFAULT_RTT_SAMPLES);
  const bandwidthBytes = opts.bandwidthBytes ?? DEFAULT_BANDWIDTH_BYTES;

  const baseUrl = resolveProbeURL(opts.signalingBase, opts.authToken);
  if (!baseUrl) return null;

  const overallDeadline = now() + durationMs;
  const controller = new AbortController();
  const overallTimer = setTimeout(
    () => controller.abort(),
    Math.max(0, overallDeadline - now()),
  );

  try {
    // 1) RTT samples: small GETs.
    const rtts: number[] = [];
    for (let i = 0; i < rttSamples; i++) {
      if (now() >= overallDeadline) break;
      const t0 = now();
      const r = await fetchImpl(`${baseUrl}${baseUrl.includes("?") ? "&" : "?"}size=${RTT_PROBE_SIZE}`, {
        method: "GET",
        signal: controller.signal,
      });
      if (!r.ok) return null;
      // Drain the body so the connection is timed end-to-end.
      await r.arrayBuffer();
      rtts.push(now() - t0);
    }
    if (rtts.length === 0) return null;

    // 2) Downlink: one sized GET.
    if (now() >= overallDeadline) return null;
    const dlT0 = now();
    const dlResp = await fetchImpl(`${baseUrl}${baseUrl.includes("?") ? "&" : "?"}size=${bandwidthBytes}`, {
      method: "GET",
      signal: controller.signal,
    });
    if (!dlResp.ok) return null;
    const dlBytes = (await dlResp.arrayBuffer()).byteLength;
    const dlMs = now() - dlT0;
    if (dlMs <= 0 || dlBytes === 0) return null;

    // 3) Uplink: one sized POST.
    if (now() >= overallDeadline) return null;
    const ulPayload = new Uint8Array(bandwidthBytes); // zeros are fine
    const ulT0 = now();
    const ulResp = await fetchImpl(baseUrl, {
      method: "POST",
      body: ulPayload,
      signal: controller.signal,
    });
    if (!ulResp.ok) return null;
    // Wait for the server's JSON ack so we capture the full RTT.
    await ulResp.arrayBuffer();
    const ulMs = now() - ulT0;
    if (ulMs <= 0) return null;

    return {
      rtt_ms: Math.round(median(rtts)),
      downlink_kbps: bytesToKbps(dlBytes, dlMs),
      uplink_kbps: bytesToKbps(bandwidthBytes, ulMs),
      samples: rtts.length,
    };
  } catch {
    // Aborted, network error, parse error — caller treats null as "no hint."
    return null;
  } finally {
    clearTimeout(overallTimer);
  }
}

/**
 * Resolve the /probe endpoint URL from a signalingBase that may use
 * `ws://` or `http://`. Mirrors turn.ts's resolveCredentialsURL.
 */
export function resolveProbeURL(
  signalingBase: string | undefined,
  authToken?: string,
): string {
  let path = "/probe";
  let url: string;
  if (!signalingBase) {
    url = path;
  } else {
    const httpBase = signalingBase.replace(/^ws(s?):/i, "http$1:");
    try {
      const u = new URL(httpBase);
      url = new URL(path, u).toString();
    } catch {
      url = `${httpBase.replace(/\/+$/, "")}${path}`;
    }
  }
  if (authToken) {
    url += `${url.includes("?") ? "&" : "?"}token=${encodeURIComponent(authToken)}`;
  }
  return url;
}

/**
 * Compute the maxBitrate (bits per second) the streamer should seed
 * into setParameters() given a probe result. Capped to [200 kbps, 8 Mbps]
 * with a 0.8 headroom multiplier as documented in probe-protocol.md.
 *
 * Lives here so client and streamer can share the formula via a
 * tiny copy or test-fixture import; streamer.js inlines an equivalent
 * (the streamer is plain JS).
 */
export function probeToMaxBitrateBps(p: ProbeResult): number {
  const lower = Math.min(p.uplink_kbps, p.downlink_kbps);
  const target = lower * 0.8 * 1000; // kbps→bps
  return clamp(target, 200_000, 8_000_000);
}

function median(xs: number[]): number {
  const s = [...xs].sort((a, b) => a - b);
  const mid = s.length >> 1;
  return s.length % 2 ? s[mid]! : (s[mid - 1]! + s[mid]!) / 2;
}

function bytesToKbps(bytes: number, ms: number): number {
  // bytes * 8 bits/byte / (ms/1000) seconds → bps; /1000 → kbps.
  return Math.max(1, Math.round((bytes * 8) / ms));
}

function clamp(x: number, lo: number, hi: number): number {
  return Math.max(lo, Math.min(hi, x));
}

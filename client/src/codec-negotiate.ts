// Codec negotiation inspector.
//
// After the offer/answer exchange completes, the client knows what
// codec the pipeline will actually use: the first payload type in the
// m=video line of the negotiated SDP, looked up in the matching
// `a=rtpmap` entry. T54 surfaces that to the debug panel and to
// telemetry, and flags two outcomes that need to be visible:
//
//   - "fallback"    — the negotiated codec is in our preference list
//                     but is NOT the top preference (e.g., we
//                     wanted VP9, got H264). Useful for diagnosing
//                     "why is my latency / quality off."
//   - "no_codec"    — the m=video section has no usable codec
//                     (encoder mismatch, all codecs stripped, etc.).
//                     Caller must surface this as a hard error
//                     rather than letting the pc sit indefinitely.
//
// Scope: pure SDP parsing. No DOM, no peer connection access.
//
// In the post-T34 answerer flow, the relevant SDP is the answer the
// client itself just produced (visible in `pc.localDescription.sdp`
// after `setLocalDescription(answer)`). In an offerer-role flow, it
// would instead be the answer received from the remote
// (`pc.remoteDescription.sdp`). Either way the function below cares
// only about an SDP string with a single negotiated m=video section.
//
// See docs/protocols/codec-fallback.md for the design doc.

export type CodecName = string;

/** Default browser preference list. Tightest-latency-first. */
export const DEFAULT_PREFERRED: CodecName[] = ["AV1", "VP9", "VP8", "H264"];

export type NegotiationOutcome = "ok" | "fallback" | "no_codec";

export interface NegotiationResult {
  /** The codec actually negotiated, or `null` if none. */
  negotiated: CodecName | null;
  /** The full PT-ordered codec list seen on the m=video line. */
  videoCodecs: CodecName[];
  /** The preferred list passed in. */
  preferred: CodecName[];
  /**
   * `ok`        — top preference negotiated.
   * `fallback`  — a non-top preference negotiated (still acceptable).
   * `no_codec`  — m=video section absent or has no rtpmap-resolvable codec.
   */
  outcome: NegotiationOutcome;
}

/**
 * Parse the negotiated codec from an SDP string. Returns the codec
 * name from the first PT in the m=video line.
 */
export function inspectNegotiatedCodec(sdp: string): CodecName | null {
  if (!sdp) return null;
  const lines = sdp.split(/\r\n|\r|\n/);
  // Sections start with m=. Find the m=video section, then pull rtpmap
  // entries WITHIN it (until next m= or EOF).
  let inVideo = false;
  let firstPt: string | null = null;
  const rtpmap = new Map<string, string>();
  for (const ln of lines) {
    if (ln.startsWith("m=")) {
      if (inVideo) break; // stop at next section
      if (ln.startsWith("m=video")) {
        inVideo = true;
        const parts = ln.split(/\s+/);
        // m=video <port> <proto> <pt> <pt> ...
        if (parts.length >= 4) firstPt = parts[3]!;
      }
      continue;
    }
    if (!inVideo) continue;
    const m = ln.match(/^a=rtpmap:(\d+)\s+([^/]+)\//);
    if (m) rtpmap.set(m[1]!, m[2]!);
  }
  if (firstPt === null) return null;
  return rtpmap.get(firstPt) ?? null;
}

/**
 * Return the full list of video codecs in the order they appear on
 * the m=video PT list. Excludes rtx, red, ulpfec, flexfec.
 */
export function listVideoCodecs(sdp: string): CodecName[] {
  if (!sdp) return [];
  const lines = sdp.split(/\r\n|\r|\n/);
  let inVideo = false;
  let pts: string[] = [];
  const rtpmap = new Map<string, string>();
  for (const ln of lines) {
    if (ln.startsWith("m=")) {
      if (inVideo) break;
      if (ln.startsWith("m=video")) {
        inVideo = true;
        const parts = ln.split(/\s+/);
        if (parts.length >= 4) pts = parts.slice(3);
      }
      continue;
    }
    if (!inVideo) continue;
    const m = ln.match(/^a=rtpmap:(\d+)\s+([^/]+)\//);
    if (m) rtpmap.set(m[1]!, m[2]!);
  }
  const skip = new Set(["rtx", "red", "ulpfec", "flexfec-03", "flexfec-2d"]);
  const out: CodecName[] = [];
  for (const pt of pts) {
    const c = rtpmap.get(pt);
    if (!c) continue;
    if (skip.has(c.toLowerCase())) continue;
    out.push(c);
  }
  return out;
}

/**
 * Classify the negotiation outcome. The first non-rtx codec on the
 * m=video PT list determines `negotiated`. Comparison is
 * case-insensitive (Chromium uses "VP9", spec is "VP9", but Firefox
 * sometimes lowercases — be lenient).
 */
export function classifyNegotiation(sdp: string, preferred: CodecName[] = DEFAULT_PREFERRED): NegotiationResult {
  const videoCodecs = listVideoCodecs(sdp);
  const negotiated = videoCodecs[0] ?? null;
  const preferredLc = preferred.map((c) => c.toLowerCase());
  let outcome: NegotiationOutcome;
  if (negotiated === null) {
    outcome = "no_codec";
  } else if (preferredLc[0] !== undefined && negotiated.toLowerCase() === preferredLc[0]) {
    outcome = "ok";
  } else if (preferredLc.includes(negotiated.toLowerCase())) {
    outcome = "fallback";
  } else {
    // Negotiated something we didn't ask for at all. Treat as fallback
    // (degraded but not broken); main.ts will surface accordingly.
    outcome = "fallback";
  }
  return { negotiated, videoCodecs, preferred, outcome };
}

/** Human-readable summary for the debug panel. */
export function describeOutcome(r: NegotiationResult): string {
  switch (r.outcome) {
    case "ok":
      return `negotiated_codec=${r.negotiated} (top preference)`;
    case "fallback":
      return `negotiated_codec=${r.negotiated} (fallback; preferred=${r.preferred.join(",")})`;
    case "no_codec":
      return `connection-failed: no video codec negotiated`;
  }
}

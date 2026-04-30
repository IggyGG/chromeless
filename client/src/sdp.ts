// Pure SDP transformation utilities.
//
// Why text-mungeing instead of `RTCRtpTransceiver.setCodecPreferences`?
// Two reasons:
//   1. setCodecPreferences is the right tool for ordering CODECS, but it
//      can't rewrite an `fmtp` line we want, and Safari ignores it for
//      some H.264 profiles in some versions.
//   2. We also want to surgically rewrite the H.264 profile-level-id and
//      strip header extensions; that has to happen at the SDP text level.
//
// Everything in this module operates on raw SDP strings. The functions
// are pure: same input → same output, no globals, no side effects. They
// preserve unrelated lines verbatim (including unusual whitespace) so a
// browser parsing the result still sees a faithful round-trip of the
// original offer/answer.
//
// See docs/protocols/sdp-munging.md for the rationale, the specific
// profile-level-id values we use, and when each transform applies.

const CRLF = "\r\n";

/**
 * Re-order the m=video line's payload-type list so payload types
 * matching `codec` (case-insensitive) are offered first. Pure function;
 * leaves m=audio and m=application sections untouched.
 *
 * If the codec is not present in the SDP, the input is returned as-is.
 *
 * @param sdp Full SDP text (uses CRLF line endings on output).
 * @param codec One of "VP9", "VP8", "H264", "AV1" — matched against
 *              `a=rtpmap:<pt> CODEC/<clock>` entries.
 */
export function prioritizeCodec(sdp: string, codec: string): string {
  const lines = splitSdp(sdp);
  const sections = splitSections(lines);
  let mutated = false;
  for (const section of sections) {
    if (!section[0]?.startsWith("m=video")) continue;
    const wantedPrimary = collectPayloadTypesForCodec(section, codec);
    if (wantedPrimary.length === 0) continue;

    // Pull rtx (retransmission) PTs along with their primary so the
    // primary/rtx pairing is preserved. rtx PTs are identified by an
    // `a=fmtp:<rtxPt> apt=<primaryPt>` line.
    const rtxByPrimary = collectRtxPairing(section);
    const wantedSet = new Set<string>();
    for (const pt of wantedPrimary) {
      wantedSet.add(pt);
      const rtx = rtxByPrimary.get(pt);
      if (rtx !== undefined) for (const r of rtx) wantedSet.add(r);
    }

    const m = section[0]!;
    const head = m.match(/^(m=video\s+\S+\s+\S+)\s+(.*)$/);
    if (!head) continue;
    const ptList = head[2]!.split(/\s+/).filter(Boolean);
    const reordered = [
      ...ptList.filter(pt => wantedSet.has(pt)),
      ...ptList.filter(pt => !wantedSet.has(pt)),
    ];

    // Skip rewrite if order is already what we'd produce — preserves
    // input bytes verbatim and gives us idempotency / no-op semantics.
    if (reordered.every((pt, i) => pt === ptList[i])) continue;

    section[0] = `${head[1]} ${reordered.join(" ")}`;
    mutated = true;
  }
  return mutated ? joinSections(sections) : sdp;
}

/**
 * Rewrite all H.264 `fmtp` lines so they advertise the given
 * profile-level-id and the matching no-B-frames params (packetization-mode,
 * level-asymmetry-allowed). Other fmtp parameters are preserved.
 *
 * Common profile-level-id values:
 *   - "42e01f" — Constrained Baseline 3.1 (no B-frames; broad compatibility)
 *   - "42001f" — Baseline 3.1
 *   - "4d001f" — Main 3.1 (some encoders use B-frames here; pair with
 *               app-level encoder config to disable them)
 *   - "640c1f" — High 3.1 (rarely a good choice for low-latency)
 *
 * Default (`42e01f`) is our recommended low-latency target.
 */
export function forceH264Profile(sdp: string, profileLevelId: string = "42e01f"): string {
  const lines = splitSdp(sdp);
  const sections = splitSections(lines);
  let mutated = false;
  for (const section of sections) {
    if (!section[0]?.startsWith("m=video")) continue;
    const h264Pts = collectPayloadTypesForCodec(section, "H264");
    if (h264Pts.length === 0) continue;
    const ptSet = new Set(h264Pts);

    for (let i = 0; i < section.length; i++) {
      const ln = section[i]!;
      const m = ln.match(/^a=fmtp:(\d+)\s+(.*)$/);
      if (!m) continue;
      if (!ptSet.has(m[1]!)) continue;
      const params = parseFmtpParams(m[2]!);
      params.set("level-asymmetry-allowed", "1");
      params.set("packetization-mode", "1");
      params.set("profile-level-id", profileLevelId);
      section[i] = `a=fmtp:${m[1]} ${serializeFmtpParams(params)}`;
      mutated = true;
    }
  }
  return mutated ? joinSections(sections) : sdp;
}

/**
 * Strip RTP header extensions whose URI matches any entry in `extUris`.
 * Matching is exact-string against the URI (not URL-prefix). The
 * corresponding `a=extmap-allow-mixed` is left in place if present —
 * removing it would require deeper renegotiation logic.
 *
 * Useful targets:
 *   - "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time" — strip
 *     when an old peer mishandles the experiment.
 *   - "urn:3gpp:video-orientation" — usually safe to strip for static
 *     screen-share (no rotation).
 */
export function stripExtensions(sdp: string, extUris: string[]): string {
  if (extUris.length === 0) return sdp;
  const targets = new Set(extUris);
  const lines = splitSdp(sdp);
  let mutated = false;
  const out: string[] = [];
  for (const ln of lines) {
    const m = ln.match(/^a=extmap:\d+(\/[^\s]+)?\s+(\S+)/);
    if (m && targets.has(m[2]!)) { mutated = true; continue; }
    out.push(ln);
  }
  return mutated ? out.join(CRLF) + (sdp.endsWith("\n") ? CRLF : "") : sdp;
}

// ---------- internals ----------

/** Split SDP into lines, dropping the trailing empty line if present. */
function splitSdp(sdp: string): string[] {
  // SDP normatively uses CRLF, but be permissive on input.
  const lines = sdp.split(/\r\n|\r|\n/);
  if (lines.length > 0 && lines[lines.length - 1] === "") lines.pop();
  return lines;
}

/**
 * Group lines into sections. The first section is the session-level
 * preamble (everything before the first m= line); subsequent sections
 * each start with their own m= line.
 */
function splitSections(lines: string[]): string[][] {
  const sections: string[][] = [[]];
  for (const ln of lines) {
    if (ln.startsWith("m=")) sections.push([ln]);
    else sections[sections.length - 1]!.push(ln);
  }
  return sections;
}

function joinSections(sections: string[][]): string {
  return sections.flat().join(CRLF) + CRLF;
}

/**
 * Find all payload types for `codec` (case-insensitive) within a single
 * media section. Looks at `a=rtpmap:<pt> CODEC/<clock>` lines.
 */
/**
 * Map primary payload type → list of rtx PTs that carry retransmissions
 * for it. Built from `a=fmtp:<rtxPt> apt=<primary>` lines within a
 * single media section.
 */
function collectRtxPairing(section: string[]): Map<string, string[]> {
  const out = new Map<string, string[]>();
  for (const ln of section) {
    const m = ln.match(/^a=fmtp:(\d+)\s+apt=(\d+)/);
    if (!m) continue;
    const rtxPt = m[1]!;
    const primary = m[2]!;
    let arr = out.get(primary);
    if (arr === undefined) { arr = []; out.set(primary, arr); }
    arr.push(rtxPt);
  }
  return out;
}

function collectPayloadTypesForCodec(section: string[], codec: string): string[] {
  const want = codec.toLowerCase();
  const out: string[] = [];
  for (const ln of section) {
    const m = ln.match(/^a=rtpmap:(\d+)\s+([^/]+)\/\d+/);
    if (!m) continue;
    if (m[2]!.toLowerCase() === want) out.push(m[1]!);
  }
  return out;
}

function parseFmtpParams(s: string): Map<string, string> {
  const out = new Map<string, string>();
  for (const part of s.split(";")) {
    const t = part.trim();
    if (!t) continue;
    const eq = t.indexOf("=");
    if (eq < 0) {
      out.set(t, "");
    } else {
      out.set(t.slice(0, eq).trim(), t.slice(eq + 1).trim());
    }
  }
  return out;
}

function serializeFmtpParams(p: Map<string, string>): string {
  const out: string[] = [];
  for (const [k, v] of p.entries()) {
    out.push(v === "" ? k : `${k}=${v}`);
  }
  return out.join(";");
}

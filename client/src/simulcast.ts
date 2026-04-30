// Simulcast SDP utilities (T77).
//
// Sibling to client/src/sdp.ts. Pure functions over SDP strings; no
// DOM, no RTCPeerConnection access. See docs/protocols/simulcast.md
// for the design.
//
// Three things this module does:
//   1. parseSimulcast(sdp): pull out the rid list + direction
//      ("send" | "recv") from the m=video section's `a=simulcast:`
//      attribute. null if no simulcast advertised.
//   2. addSimulcastReceive(sdp, rids): rewrite an answer SDP so it
//      advertises `a=simulcast:recv <rids>` plus matching
//      `a=rid:<id> recv` lines. Used when the answerer wants to
//      subscribe to a subset of the offer's layers.
//   3. forceSimulcastLayers(sdp, layers): rewrite an SDP's
//      `a=rid:* send` / `a=simulcast:send` to a fresh set of layers.
//      Useful for tests + manual experiments; rarely needed in
//      production because the streamer's setParameters() drives the
//      offer SDP directly.

const CRLF = "\r\n";

export type SimulcastDirection = "send" | "recv";

export interface SimulcastDecl {
  direction: SimulcastDirection;
  /**
   * List of rid identifiers in priority order (the order the SDP
   * declared). For an alternative-list (`rid0,rid1`) we flatten —
   * v1 doesn't use alternatives, so a flat list matches the wire
   * shape we both produce and consume.
   */
  rids: string[];
}

/**
 * Parse the m=video section's `a=simulcast:` line.
 * Returns null if there is no `m=video` section, or no
 * `a=simulcast:` attribute, or it's malformed.
 */
export function parseSimulcast(sdp: string): SimulcastDecl | null {
  const sections = splitVideo(sdp);
  if (sections === null) return null;
  for (const ln of sections) {
    const m = ln.match(/^a=simulcast:(send|recv)\s+(.+)$/);
    if (!m) continue;
    const dir = m[1] as SimulcastDirection;
    // Each entry in the spec can be `rid` or `rid,alt-rid`. v1 doesn't
    // use alternatives, so we just split on `;` and keep the first
    // alternative (the canonical rid) of each entry.
    const rids = m[2]!
      .split(";")
      .map((s) => s.trim())
      .filter(Boolean)
      .map((entry) => entry.split(",")[0]!.trim());
    if (rids.length === 0) return null;
    return { direction: dir, rids };
  }
  return null;
}

/**
 * Add or replace simulcast-receive directives in the m=video section.
 * Strips any existing `a=rid:*` / `a=simulcast:` lines, then injects
 * `a=rid:<id> recv` lines + a single `a=simulcast:recv <rids>` line.
 * Other m=video lines are preserved verbatim and in order.
 *
 * Returns the input unchanged if there is no m=video section.
 */
export function addSimulcastReceive(sdp: string, rids: string[]): string {
  if (rids.length === 0) return sdp;
  return rewriteVideo(sdp, (videoLines) => {
    const cleaned = videoLines.filter(
      (ln) => !ln.startsWith("a=rid:") && !ln.startsWith("a=simulcast:"),
    );
    const inject = [
      ...rids.map((r) => `a=rid:${r} recv`),
      `a=simulcast:recv ${rids.join(";")}`,
    ];
    // Insert before the first `a=` (typical anchor) or at the end.
    return spliceAttributes(cleaned, inject);
  });
}

/**
 * Replace existing simulcast-send layers with a custom set. Used by
 * tests and manual diagnostic flows; the streamer's `setParameters`
 * normally bakes the layers into the SDP for us.
 */
export function forceSimulcastLayers(
  sdp: string,
  layers: Array<{ rid: string; restrictions?: string }>,
): string {
  if (layers.length === 0) return sdp;
  return rewriteVideo(sdp, (videoLines) => {
    const cleaned = videoLines.filter(
      (ln) => !ln.startsWith("a=rid:") && !ln.startsWith("a=simulcast:"),
    );
    const inject: string[] = layers.map((l) => {
      const tail = l.restrictions ? ` ${l.restrictions}` : "";
      return `a=rid:${l.rid} send${tail}`;
    });
    inject.push(`a=simulcast:send ${layers.map((l) => l.rid).join(";")}`);
    return spliceAttributes(cleaned, inject);
  });
}

// ---------- internals ----------

function splitLines(sdp: string): string[] {
  const ls = sdp.split(/\r\n|\r|\n/);
  if (ls.length > 0 && ls[ls.length - 1] === "") ls.pop();
  return ls;
}

/** Return the lines of the first m=video section, or null. */
function splitVideo(sdp: string): string[] | null {
  const lines = splitLines(sdp);
  let inVideo = false;
  const out: string[] = [];
  for (const ln of lines) {
    if (ln.startsWith("m=")) {
      if (inVideo) break;
      if (ln.startsWith("m=video")) {
        inVideo = true;
        out.push(ln);
      }
      continue;
    }
    if (inVideo) out.push(ln);
  }
  return inVideo ? out : null;
}

/**
 * Run `transform` against the m=video section's lines and stitch the
 * result back into the full SDP. Preserves the rest of the document
 * verbatim. Returns the input unchanged if there is no m=video.
 */
function rewriteVideo(sdp: string, transform: (videoLines: string[]) => string[]): string {
  const lines = splitLines(sdp);
  let videoStart = -1;
  let videoEnd = lines.length;
  for (let i = 0; i < lines.length; i++) {
    if (lines[i]!.startsWith("m=")) {
      if (videoStart >= 0) {
        videoEnd = i;
        break;
      }
      if (lines[i]!.startsWith("m=video")) {
        videoStart = i;
      }
    }
  }
  if (videoStart < 0) return sdp;
  const before = lines.slice(0, videoStart);
  const video = lines.slice(videoStart, videoEnd);
  const after = lines.slice(videoEnd);
  const newVideo = transform(video);
  return [...before, ...newVideo, ...after].join(CRLF) + CRLF;
}

/**
 * Insert `inject` lines into a section after the last `a=`-prefixed
 * line, so they appear at the natural end of the attribute block but
 * before any further m= section. If no `a=` lines exist, append at
 * the end.
 */
function spliceAttributes(section: string[], inject: string[]): string[] {
  let lastAttr = -1;
  for (let i = 0; i < section.length; i++) {
    if (section[i]!.startsWith("a=")) lastAttr = i;
  }
  if (lastAttr < 0) return [...section, ...inject];
  return [...section.slice(0, lastAttr + 1), ...inject, ...section.slice(lastAttr + 1)];
}

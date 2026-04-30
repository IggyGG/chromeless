// Tests for SDP munging utilities.
//
// Fixtures live in `__fixtures__/`:
//   - chromium-offer.sdp — captured from real headless Chromium 147 via
//     Playwright at fixture-generation time (see docs/protocols/sdp-munging.md).
//   - firefox-offer.sdp / safari-offer.sdp — representative shapes;
//     hand-curated to mirror the canonical structure each browser emits.
//
// We test on real bytes rather than mocks because the whole point of
// these utilities is being correct against the SDP each browser
// actually produces.

import { describe, it, expect } from "vitest";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import {
  prioritizeCodec,
  forceH264Profile,
  stripExtensions,
} from "./sdp.js";

const here = dirname(fileURLToPath(import.meta.url));
const fx = (name: string) => readFileSync(join(here, "__fixtures__", name), "utf8");

const CHROMIUM = fx("chromium-offer.sdp");
const FIREFOX  = fx("firefox-offer.sdp");
const SAFARI   = fx("safari-offer.sdp");

/** Extract the m=video payload-type list from an SDP. */
function videoPts(sdp: string): string[] {
  const m = sdp.split(/\r\n|\n/).find(l => l.startsWith("m=video"));
  if (!m) throw new Error("no m=video line");
  return m.split(/\s+/).slice(3);
}

/** Map payload type → codec name from a=rtpmap entries within m=video. */
function ptCodecs(sdp: string): Map<string, string> {
  const out = new Map<string, string>();
  let inVideo = false;
  for (const ln of sdp.split(/\r\n|\n/)) {
    if (ln.startsWith("m=")) inVideo = ln.startsWith("m=video");
    if (!inVideo) continue;
    const m = ln.match(/^a=rtpmap:(\d+)\s+([^/]+)\//);
    if (m) out.set(m[1]!, m[2]!);
  }
  return out;
}

describe("prioritizeCodec", () => {
  it("puts VP9 PTs first on Chromium offer", () => {
    const out = prioritizeCodec(CHROMIUM, "VP9");
    const pts = videoPts(out);
    const codecs = ptCodecs(out);
    // First few PTs should be VP9 + their associated rtx (rtx is codec-paired
    // but our reorder works on the named codec only — rtx may stay later).
    expect(codecs.get(pts[0]!)).toBe("VP9");
    // Original Chromium offer leads with VP8 (96), so we know we mutated.
    expect(videoPts(CHROMIUM)[0]).not.toBe(pts[0]);
    // No PTs were lost.
    expect(pts.sort()).toEqual(videoPts(CHROMIUM).sort());
  });

  it("puts H264 PTs first on Firefox offer (case-insensitive)", () => {
    const out = prioritizeCodec(FIREFOX, "h264");
    const pts = videoPts(out);
    const codecs = ptCodecs(out);
    expect(codecs.get(pts[0]!)).toBe("H264");
    expect(pts.sort()).toEqual(videoPts(FIREFOX).sort());
  });

  it("is idempotent — Safari already leads with H264", () => {
    const before = videoPts(SAFARI);
    const after  = videoPts(prioritizeCodec(SAFARI, "H264"));
    expect(after).toEqual(before);
  });

  it("returns input unchanged when codec is absent", () => {
    // Theora is not present in any of our captured fixtures.
    expect(prioritizeCodec(CHROMIUM, "Theora")).toBe(CHROMIUM);
    expect(prioritizeCodec(FIREFOX,  "Theora")).toBe(FIREFOX);
    expect(prioritizeCodec(SAFARI,   "Theora")).toBe(SAFARI);
  });

  it("does not touch m=audio payload ordering", () => {
    const out = prioritizeCodec(FIREFOX, "VP9");
    const audioBefore = FIREFOX.split(/\r\n|\n/).find(l => l.startsWith("m=audio"));
    const audioAfter  = out.split(/\r\n|\n/).find(l => l.startsWith("m=audio"));
    expect(audioAfter).toBe(audioBefore);
  });

  it("preserves session-level lines verbatim", () => {
    const out = prioritizeCodec(CHROMIUM, "VP9");
    expect(out).toContain("a=group:BUNDLE 0 1");
    expect(out).toContain("a=msid-semantic: WMS");
  });
});

describe("forceH264Profile", () => {
  it("rewrites every H264 fmtp on Safari to 42e01f packetization-mode=1", () => {
    const out = forceH264Profile(SAFARI);
    const pts = ptCodecs(out);
    const h264Pts = [...pts.entries()].filter(([, c]) => c === "H264").map(([pt]) => pt);
    expect(h264Pts.length).toBeGreaterThan(0);
    for (const pt of h264Pts) {
      const re = new RegExp(`^a=fmtp:${pt}\\s+(.*)$`, "m");
      const m = out.match(re);
      expect(m, `fmtp for pt=${pt}`).not.toBeNull();
      const params = m![1]!;
      expect(params).toContain("profile-level-id=42e01f");
      expect(params).toContain("packetization-mode=1");
      expect(params).toContain("level-asymmetry-allowed=1");
    }
  });

  it("accepts a custom profile-level-id and applies it everywhere", () => {
    const out = forceH264Profile(SAFARI, "640c1f");
    expect(out).toContain("profile-level-id=640c1f");
    expect(out).not.toContain("profile-level-id=42e01f");
    expect(out).not.toContain("profile-level-id=4d001f");
  });

  it("does not touch fmtp lines for non-H264 PTs", () => {
    const out = forceH264Profile(CHROMIUM);
    // VP9 has fmtp:98 profile-id=0 etc. — must be intact.
    expect(out).toContain("a=fmtp:98 profile-id=0");
    expect(out).toContain("a=fmtp:100 profile-id=2");
  });

  it("returns the original SDP unchanged when there is no H264", () => {
    // Synthesize VP9-only SDP and assert no mutation.
    const onlyVp9 = [
      "v=0", "o=- 1 2 IN IP4 0.0.0.0", "s=-", "t=0 0",
      "m=video 9 UDP/TLS/RTP/SAVPF 96",
      "a=rtpmap:96 VP9/90000",
      "a=fmtp:96 profile-id=0",
      "",
    ].join("\r\n");
    expect(forceH264Profile(onlyVp9)).toBe(onlyVp9);
  });
});

describe("stripExtensions", () => {
  it("removes a single extmap URI from Chromium offer", () => {
    const target = "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time";
    expect(CHROMIUM).toContain(target);
    const out = stripExtensions(CHROMIUM, [target]);
    expect(out).not.toContain(target);
  });

  it("removes multiple URIs and leaves others alone", () => {
    const removed = [
      "urn:3gpp:video-orientation",
      "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay",
    ];
    const out = stripExtensions(CHROMIUM, removed);
    for (const u of removed) expect(out).not.toContain(u);
    // sdes:mid is canonical; must remain.
    expect(out).toContain("urn:ietf:params:rtp-hdrext:sdes:mid");
  });

  it("handles Firefox's recvonly direction modifier on extmap", () => {
    const target = "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay";
    expect(FIREFOX).toContain(`a=extmap:6/recvonly ${target}`);
    const out = stripExtensions(FIREFOX, [target]);
    expect(out).not.toContain(target);
  });

  it("returns input unchanged when none of the URIs are present", () => {
    expect(stripExtensions(SAFARI, ["urn:does-not-exist"])).toBe(SAFARI);
  });

  it("returns input unchanged for empty filter list", () => {
    expect(stripExtensions(SAFARI, [])).toBe(SAFARI);
  });
});

describe("composability", () => {
  it("applying all three transforms still yields a parseable SDP", () => {
    let s = CHROMIUM;
    s = prioritizeCodec(s, "VP9");
    s = forceH264Profile(s);
    s = stripExtensions(s, ["urn:3gpp:video-orientation"]);
    // Sanity: still has the must-have lines for a Chromium offer.
    expect(s).toMatch(/^v=0/);
    expect(s).toContain("a=group:BUNDLE 0 1");
    expect(s).toContain("m=video");
    // The video PT list still tokenises cleanly.
    const pts = videoPts(s);
    expect(pts.length).toBeGreaterThan(5);
    expect(pts.every(pt => /^\d+$/.test(pt))).toBe(true);
  });
});

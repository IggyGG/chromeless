import { describe, it, expect } from "vitest";
import {
  inspectNegotiatedCodec,
  listVideoCodecs,
  classifyNegotiation,
  describeOutcome,
  DEFAULT_PREFERRED,
} from "./codec-negotiate.js";

// Compact synthesised answer SDPs. Real-world fixtures (chromium-offer.sdp
// etc.) live alongside sdp.test.ts; these test the answer side, where we
// expect a single negotiated codec at the top of the PT list.

const ANSWER_VP9_TOP = [
  "v=0", "o=- 1 2 IN IP4 0.0.0.0", "s=-", "t=0 0",
  "m=video 9 UDP/TLS/RTP/SAVPF 98 99 96 97",
  "c=IN IP4 0.0.0.0",
  "a=rtpmap:98 VP9/90000",
  "a=rtpmap:99 rtx/90000",
  "a=fmtp:99 apt=98",
  "a=rtpmap:96 VP8/90000",
  "a=rtpmap:97 rtx/90000",
  "a=fmtp:97 apt=96",
  "",
].join("\r\n");

const ANSWER_H264_TOP = [
  "v=0", "o=- 1 2 IN IP4 0.0.0.0", "s=-", "t=0 0",
  "m=video 9 UDP/TLS/RTP/SAVPF 102 103 96",
  "a=rtpmap:102 H264/90000",
  "a=fmtp:102 packetization-mode=1;profile-level-id=42e01f",
  "a=rtpmap:103 rtx/90000",
  "a=fmtp:103 apt=102",
  "a=rtpmap:96 VP8/90000",
  "",
].join("\r\n");

const ANSWER_VP9_AND_H264 = [
  "v=0", "o=- 1 2 IN IP4 0.0.0.0", "s=-", "t=0 0",
  "m=video 9 UDP/TLS/RTP/SAVPF 98 99 102 103",
  "a=rtpmap:98 VP9/90000",
  "a=rtpmap:99 rtx/90000", "a=fmtp:99 apt=98",
  "a=rtpmap:102 H264/90000",
  "a=rtpmap:103 rtx/90000", "a=fmtp:103 apt=102",
  "",
].join("\r\n");

const ANSWER_NO_VIDEO_PT = [
  "v=0", "o=- 1 2 IN IP4 0.0.0.0", "s=-", "t=0 0",
  "m=video 0 UDP/TLS/RTP/SAVPF",
  "c=IN IP4 0.0.0.0",
  "a=inactive",
  "",
].join("\r\n");

const ANSWER_NO_RTPMAP_FOR_FIRST_PT = [
  "v=0", "o=- 1 2 IN IP4 0.0.0.0", "s=-", "t=0 0",
  "m=video 9 UDP/TLS/RTP/SAVPF 200",
  // PT 200 has no rtpmap. Treated as "no codec".
  "",
].join("\r\n");

const ANSWER_NO_VIDEO_SECTION = [
  "v=0", "o=- 1 2 IN IP4 0.0.0.0", "s=-", "t=0 0",
  "m=audio 9 UDP/TLS/RTP/SAVPF 111",
  "a=rtpmap:111 opus/48000/2",
  "",
].join("\r\n");

describe("inspectNegotiatedCodec", () => {
  it("returns the codec name of the first m=video PT", () => {
    expect(inspectNegotiatedCodec(ANSWER_VP9_TOP)).toBe("VP9");
    expect(inspectNegotiatedCodec(ANSWER_H264_TOP)).toBe("H264");
  });
  it("returns null when m=video section has port 0 and no PTs", () => {
    expect(inspectNegotiatedCodec(ANSWER_NO_VIDEO_PT)).toBeNull();
  });
  it("returns null when first PT has no rtpmap", () => {
    expect(inspectNegotiatedCodec(ANSWER_NO_RTPMAP_FOR_FIRST_PT)).toBeNull();
  });
  it("returns null when there is no m=video at all", () => {
    expect(inspectNegotiatedCodec(ANSWER_NO_VIDEO_SECTION)).toBeNull();
  });
  it("returns null for empty input", () => {
    expect(inspectNegotiatedCodec("")).toBeNull();
  });
});

describe("listVideoCodecs", () => {
  it("excludes rtx PTs from the list", () => {
    expect(listVideoCodecs(ANSWER_VP9_TOP)).toEqual(["VP9", "VP8"]);
    expect(listVideoCodecs(ANSWER_VP9_AND_H264)).toEqual(["VP9", "H264"]);
  });
  it("returns empty array for SDPs without video", () => {
    expect(listVideoCodecs(ANSWER_NO_VIDEO_SECTION)).toEqual([]);
  });
});

describe("classifyNegotiation", () => {
  it("VP9 preferred, VP9 negotiated → ok", () => {
    const r = classifyNegotiation(ANSWER_VP9_TOP, ["VP9", "H264"]);
    expect(r.outcome).toBe("ok");
    expect(r.negotiated).toBe("VP9");
  });

  it("VP9 preferred, H264 negotiated → fallback", () => {
    const r = classifyNegotiation(ANSWER_H264_TOP, ["VP9", "H264"]);
    expect(r.outcome).toBe("fallback");
    expect(r.negotiated).toBe("H264");
  });

  it("VP9 preferred, both VP9 and H264 offered → VP9 wins (ok)", () => {
    const r = classifyNegotiation(ANSWER_VP9_AND_H264, ["VP9", "H264"]);
    expect(r.outcome).toBe("ok");
    expect(r.negotiated).toBe("VP9");
    expect(r.videoCodecs).toEqual(["VP9", "H264"]);
  });

  it("no video codec at all → no_codec", () => {
    const r = classifyNegotiation(ANSWER_NO_VIDEO_PT);
    expect(r.outcome).toBe("no_codec");
    expect(r.negotiated).toBeNull();
  });

  it("first PT has no rtpmap → no_codec", () => {
    const r = classifyNegotiation(ANSWER_NO_RTPMAP_FOR_FIRST_PT);
    expect(r.outcome).toBe("no_codec");
  });

  it("no m=video section → no_codec", () => {
    const r = classifyNegotiation(ANSWER_NO_VIDEO_SECTION);
    expect(r.outcome).toBe("no_codec");
  });

  it("uses default preference list when none passed", () => {
    const r = classifyNegotiation(ANSWER_VP9_TOP);
    expect(r.preferred).toEqual(DEFAULT_PREFERRED);
    // VP9 is index 1 (after AV1) in default → fallback.
    expect(r.outcome).toBe("fallback");
  });

  it("case-insensitive matching", () => {
    const lc = ANSWER_VP9_TOP.replace(/VP9/g, "vp9");
    const r = classifyNegotiation(lc, ["VP9", "H264"]);
    expect(r.outcome).toBe("ok");
    expect(r.negotiated).toBe("vp9");
  });

  it("classifies an entirely-unexpected codec as fallback", () => {
    const sdp = ANSWER_VP9_TOP.replace(/VP9/g, "Theora");
    const r = classifyNegotiation(sdp, ["VP9", "H264"]);
    expect(r.outcome).toBe("fallback");
    expect(r.negotiated).toBe("Theora");
  });
});

describe("describeOutcome", () => {
  it("formats the three outcomes", () => {
    expect(describeOutcome(classifyNegotiation(ANSWER_VP9_TOP, ["VP9"])))
      .toContain("top preference");
    expect(describeOutcome(classifyNegotiation(ANSWER_H264_TOP, ["VP9", "H264"])))
      .toContain("fallback");
    expect(describeOutcome(classifyNegotiation(ANSWER_NO_VIDEO_PT)))
      .toContain("no video codec negotiated");
  });
});

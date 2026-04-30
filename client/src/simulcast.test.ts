import { describe, it, expect } from "vitest";
import {
  parseSimulcast,
  addSimulcastReceive,
  forceSimulcastLayers,
} from "./simulcast.js";

// Compact synthesized SDPs that exercise specific simulcast shapes.
// Real-world fixtures (chromium-offer.sdp etc.) live under
// __fixtures__/ but those don't have simulcast — Chromium only emits
// it when setParameters has explicit `encodings`.

const PLAIN_VIDEO = [
  "v=0", "o=- 1 2 IN IP4 0.0.0.0", "s=-", "t=0 0",
  "m=video 9 UDP/TLS/RTP/SAVPF 96",
  "c=IN IP4 0.0.0.0",
  "a=rtpmap:96 VP9/90000",
  "a=fmtp:96 profile-id=0",
  "a=sendonly",
  "",
].join("\r\n");

const SIMULCAST_SEND = [
  "v=0", "o=- 1 2 IN IP4 0.0.0.0", "s=-", "t=0 0",
  "m=video 9 UDP/TLS/RTP/SAVPF 96",
  "c=IN IP4 0.0.0.0",
  "a=rtpmap:96 VP9/90000",
  "a=rid:layer0 send",
  "a=rid:layer1 send",
  "a=rid:layer2 send",
  "a=simulcast:send layer0;layer1;layer2",
  "a=sendonly",
  "",
].join("\r\n");

const SIMULCAST_RECV_THREE = [
  "v=0", "o=- 1 2 IN IP4 0.0.0.0", "s=-", "t=0 0",
  "m=video 9 UDP/TLS/RTP/SAVPF 96",
  "c=IN IP4 0.0.0.0",
  "a=rtpmap:96 VP9/90000",
  "a=rid:layer0 recv",
  "a=rid:layer1 recv",
  "a=rid:layer2 recv",
  "a=simulcast:recv layer0;layer1;layer2",
  "a=recvonly",
  "",
].join("\r\n");

const NO_VIDEO = [
  "v=0", "o=- 1 2 IN IP4 0.0.0.0", "s=-", "t=0 0",
  "m=audio 9 UDP/TLS/RTP/SAVPF 111",
  "a=rtpmap:111 opus/48000/2",
  "",
].join("\r\n");

const SIMULCAST_WITH_ALTERNATIVES = [
  "v=0", "o=- 1 2 IN IP4 0.0.0.0", "s=-", "t=0 0",
  "m=video 9 UDP/TLS/RTP/SAVPF 96",
  "a=rtpmap:96 VP9/90000",
  "a=rid:hi send",
  "a=rid:hi-alt send",
  "a=rid:lo send",
  "a=simulcast:send hi,hi-alt;lo",
  "",
].join("\r\n");

describe("parseSimulcast", () => {
  it("returns null for SDPs without simulcast", () => {
    expect(parseSimulcast(PLAIN_VIDEO)).toBeNull();
  });

  it("returns null for SDPs without m=video", () => {
    expect(parseSimulcast(NO_VIDEO)).toBeNull();
  });

  it("extracts a send-side simulcast declaration", () => {
    expect(parseSimulcast(SIMULCAST_SEND)).toEqual({
      direction: "send",
      rids: ["layer0", "layer1", "layer2"],
    });
  });

  it("extracts a recv-side declaration", () => {
    expect(parseSimulcast(SIMULCAST_RECV_THREE)).toEqual({
      direction: "recv",
      rids: ["layer0", "layer1", "layer2"],
    });
  });

  it("flattens alternatives to the canonical rid (first per group)", () => {
    expect(parseSimulcast(SIMULCAST_WITH_ALTERNATIVES)).toEqual({
      direction: "send",
      rids: ["hi", "lo"],
    });
  });

  it("returns null on an empty rid list", () => {
    const malformed = SIMULCAST_SEND.replace("a=simulcast:send layer0;layer1;layer2", "a=simulcast:send ");
    expect(parseSimulcast(malformed)).toBeNull();
  });
});

describe("addSimulcastReceive", () => {
  it("injects a=rid:<id> recv + a=simulcast:recv into a plain video SDP", () => {
    const out = addSimulcastReceive(PLAIN_VIDEO, ["layer0", "layer1", "layer2"]);
    expect(out).toContain("a=rid:layer0 recv");
    expect(out).toContain("a=rid:layer1 recv");
    expect(out).toContain("a=rid:layer2 recv");
    expect(out).toContain("a=simulcast:recv layer0;layer1;layer2");
    // Pre-existing m=video attributes are preserved.
    expect(out).toContain("a=rtpmap:96 VP9/90000");
    expect(out).toContain("a=fmtp:96 profile-id=0");
    expect(out).toContain("a=sendonly");
  });

  it("replaces an existing recv-side simulcast block", () => {
    const out = addSimulcastReceive(SIMULCAST_RECV_THREE, ["layer0"]);
    expect(out).toContain("a=simulcast:recv layer0");
    expect(out).toContain("a=rid:layer0 recv");
    expect(out).not.toContain("a=simulcast:recv layer0;layer1;layer2");
    expect(out).not.toContain("a=rid:layer1 recv");
    expect(out).not.toContain("a=rid:layer2 recv");
  });

  it("strips a send-side simulcast and substitutes recv (uncommon but valid)", () => {
    const out = addSimulcastReceive(SIMULCAST_SEND, ["layer0", "layer1"]);
    expect(out).not.toContain("a=simulcast:send");
    expect(out).not.toContain("a=rid:layer0 send");
    expect(out).toContain("a=simulcast:recv layer0;layer1");
    expect(out).toContain("a=rid:layer0 recv");
  });

  it("returns input unchanged for empty rid list", () => {
    expect(addSimulcastReceive(PLAIN_VIDEO, [])).toBe(PLAIN_VIDEO);
  });

  it("returns input unchanged when there is no m=video", () => {
    expect(addSimulcastReceive(NO_VIDEO, ["layer0"])).toBe(NO_VIDEO);
  });
});

describe("forceSimulcastLayers", () => {
  it("rewrites send-side rid + simulcast lines wholesale", () => {
    const out = forceSimulcastLayers(PLAIN_VIDEO, [
      { rid: "L0", restrictions: "max-width=1920;max-height=1080" },
      { rid: "L1", restrictions: "max-width=960;max-height=540" },
      { rid: "L2", restrictions: "max-width=480;max-height=270" },
    ]);
    expect(out).toContain("a=rid:L0 send max-width=1920;max-height=1080");
    expect(out).toContain("a=rid:L1 send max-width=960;max-height=540");
    expect(out).toContain("a=rid:L2 send max-width=480;max-height=270");
    expect(out).toContain("a=simulcast:send L0;L1;L2");
  });

  it("strips an existing simulcast block before injecting the new one", () => {
    const out = forceSimulcastLayers(SIMULCAST_SEND, [
      { rid: "lo" },
      { rid: "hi" },
    ]);
    expect(out).toContain("a=simulcast:send lo;hi");
    expect(out).toContain("a=rid:lo send");
    expect(out).toContain("a=rid:hi send");
    expect(out).not.toContain("a=simulcast:send layer0;layer1;layer2");
    expect(out).not.toContain("a=rid:layer0 send");
  });

  it("noop on empty list", () => {
    expect(forceSimulcastLayers(SIMULCAST_SEND, [])).toBe(SIMULCAST_SEND);
  });

  it("noop when m=video absent", () => {
    expect(forceSimulcastLayers(NO_VIDEO, [{ rid: "x" }])).toBe(NO_VIDEO);
  });
});

describe("integration: roundtrip through parseSimulcast", () => {
  it("forced layers can be parsed back as a SimulcastDecl", () => {
    const after = forceSimulcastLayers(PLAIN_VIDEO, [
      { rid: "x" }, { rid: "y" }, { rid: "z" },
    ]);
    expect(parseSimulcast(after)).toEqual({
      direction: "send",
      rids: ["x", "y", "z"],
    });
  });

  it("addSimulcastReceive output parses as a recv-side decl", () => {
    const after = addSimulcastReceive(PLAIN_VIDEO, ["q", "r"]);
    expect(parseSimulcast(after)).toEqual({
      direction: "recv",
      rids: ["q", "r"],
    });
  });
});

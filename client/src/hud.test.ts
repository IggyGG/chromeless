// Tests for the HUD summariser.
//
// The point of these is the DELTAS. Every counter in a StatsSample is
// cumulative, so a summariser that forgets to diff renders numbers that only
// climb — which looks plausible and is wrong. Each test below fails if the
// diff is dropped.

import { describe, it, expect } from "vitest";
import { summarise, formatResolution } from "./hud.js";
import type { StatsSample } from "./stats.js";

function sample(over: {
  t: number;
  bytesReceived?: number;
  packetsReceived?: number;
  packetsLost?: number;
  framesPerSecond?: number | null;
  framesReceived?: number | null;
  rtt?: number | null;
}): StatsSample {
  return {
    v: 1,
    t: over.t,
    inbound: [{
      trackId: "v", kind: "video",
      bytesReceived: over.bytesReceived ?? 0,
      packetsReceived: over.packetsReceived ?? 0,
      packetsLost: over.packetsLost ?? 0,
      jitter: 0,
      framesPerSecond: over.framesPerSecond ?? null,
      framesDropped: null,
      framesReceived: over.framesReceived ?? null,
      totalDecodeTime: null,
    }],
    outbound: [],
    remoteInbound: [],
    candidatePair: over.rtt === undefined ? null : {
      currentRoundTripTime: over.rtt,
      availableOutgoingBitrate: null,
      availableIncomingBitrate: null,
      localCandidateType: null,
      remoteCandidateType: null,
    },
  } as StatsSample;
}

describe("summarise", () => {
  it("says '—', not 0, with no sample at all", () => {
    // "0 fps" and "I don't know yet" are different claims, and the first
    // reads as a broken stream.
    const s = summarise(undefined);
    expect(s).toEqual({
      fps: "—", bitrate: "—", rtt: "—", loss: "—", resolution: "—",
      quality: "unknown",
    });
  });

  it("says '—' for rates on the FIRST sample — there is nothing to diff", () => {
    const s = summarise(sample({ t: 1000, bytesReceived: 500_000 }));
    expect(s.bitrate).toBe("—");
    expect(s.fps).toBe("—");
  });

  it("computes bitrate from the byte DELTA over the interval", () => {
    // 125_000 bytes in 1s = 1_000_000 bits/s = 1000 kbps -> "1.0 Mbps".
    const a = sample({ t: 1000, bytesReceived: 1_000_000 });
    const b = sample({ t: 2000, bytesReceived: 1_125_000 });
    expect(summarise(b, a).bitrate).toBe("1.0 Mbps");
  });

  it("renders sub-megabit rates in kbps", () => {
    const a = sample({ t: 1000, bytesReceived: 0 });
    const b = sample({ t: 2000, bytesReceived: 25_000 });   // 200 kbps
    expect(summarise(b, a).bitrate).toBe("200 kbps");
  });

  it("prefers the browser's own framesPerSecond", () => {
    const s = summarise(sample({ t: 1000, framesPerSecond: 29.6 }));
    expect(s.fps).toBe("30 fps");
  });

  it("falls back to a frame delta where framesPerSecond is absent", () => {
    const a = sample({ t: 1000, framesReceived: 100 });
    const b = sample({ t: 2000, framesReceived: 125 });
    expect(summarise(b, a).fps).toBe("25 fps");
  });

  it("reports loss over the INTERVAL, not since the session began", () => {
    // 200 lost early, none since. A cumulative ratio would keep accusing a
    // connection that is currently perfect.
    const a = sample({ t: 1000, packetsLost: 200, packetsReceived: 1_000 });
    const b = sample({ t: 2000, packetsLost: 200, packetsReceived: 2_000 });
    expect(summarise(b, a).loss).toBe("0.0%");
  });

  it("does report loss that is actually happening now", () => {
    const a = sample({ t: 1000, packetsLost: 0, packetsReceived: 1_000 });
    const b = sample({ t: 2000, packetsLost: 10, packetsReceived: 1_990 });
    expect(summarise(b, a).loss).toBe("1.0%");
  });

  it("converts RTT from seconds to milliseconds", () => {
    expect(summarise(sample({ t: 1, rtt: 0.042 })).rtt).toBe("42 ms");
  });

  it("does not divide by zero when two samples share a timestamp", () => {
    // Rendered "Infinity fps" before the guard, which reads as a triumph.
    const a = sample({ t: 1000, bytesReceived: 0, framesReceived: 0 });
    const b = sample({ t: 1000, bytesReceived: 999_999, framesReceived: 999 });
    const s = summarise(b, a);
    expect(s.bitrate).toBe("—");
    expect(s.fps).toBe("—");
  });

  describe("quality verdict", () => {
    const two = (over: Omit<Parameters<typeof sample>[0], "t">) =>
      summarise(sample({ ...over, t: 2000 }),
                sample({ t: 1000, packetsReceived: 0, packetsLost: 0 })).quality;

    it("is good on a healthy connection", () => {
      expect(two({ framesPerSecond: 30, rtt: 0.03, packetsReceived: 1000 }))
        .toBe("good");
    });
    it("is fair when latency is noticeable", () => {
      expect(two({ framesPerSecond: 30, rtt: 0.2, packetsReceived: 1000 }))
        .toBe("fair");
    });
    it("is poor when latency is bad", () => {
      expect(two({ framesPerSecond: 30, rtt: 0.4, packetsReceived: 1000 }))
        .toBe("poor");
    });
    it("is poor when the frame rate collapses", () => {
      expect(two({ framesPerSecond: 4, rtt: 0.02, packetsReceived: 1000 }))
        .toBe("poor");
    });
    it("is poor on heavy loss", () => {
      expect(two({ framesPerSecond: 30, rtt: 0.02,
                   packetsReceived: 900, packetsLost: 100 })).toBe("poor");
    });
    it("is unknown before anything is known", () => {
      expect(summarise(undefined).quality).toBe("unknown");
    });
  });
});

describe("formatResolution", () => {
  it("formats a real size", () => {
    expect(formatResolution(1280, 720)).toBe("1280x720");
  });
  it("says '—' before the video has one", () => {
    // A <video> with no frame yet reports 0x0; "0x0" reads as a defect.
    expect(formatResolution(0, 0)).toBe("—");
  });
});

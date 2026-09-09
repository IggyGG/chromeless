// HUD — the connection, in five numbers a person can read.
//
// WHY THIS EXISTS
// ---------------
// `ChromelessSession` has emitted a `stats` event every second since T82,
// carrying a full StatsSample, and NOTHING in the client subscribed to it.
// The whole sample was assembled, shipped over the stats data channel to the
// broker, and dropped on the floor locally. So a viewer watching a stream
// degrade had no way to tell a slow network from a broken guest — the panel
// showed `connected` either way, which is the least useful thing it could
// say at exactly the moment someone needs to know.
//
// This file is deliberately PURE: `summarise()` turns two consecutive
// samples into display strings and a quality verdict, with no DOM and no
// clock. That is what makes it testable in this package (tests run in node,
// there is no document), and the rendering is a thin wrapper over it.
//
// The deltas matter. Every counter in a StatsSample is CUMULATIVE — bytes,
// packets, frames — so a HUD that renders them raw shows numbers that only
// ever go up and mean nothing. fps and bitrate are computed against the
// previous sample's timestamp; with no previous sample they read "—" rather
// than a fabricated zero, because "0 fps" and "I don't know yet" are
// different claims and the first one looks like a bug.

import type { StatsSample } from "./stats.js";

/** Overall verdict, for the status dot. */
export type HudQuality = "good" | "fair" | "poor" | "unknown";

export interface HudSummary {
  fps: string;
  bitrate: string;
  rtt: string;
  loss: string;
  resolution: string;
  quality: HudQuality;
}

const EMPTY: HudSummary = {
  fps: "—", bitrate: "—", rtt: "—", loss: "—", resolution: "—",
  quality: "unknown",
};

function inboundVideo(s: StatsSample | undefined) {
  return s?.inbound.find((i) => i.kind === "video");
}

/**
 * Turn the newest sample (and the one before it) into display strings.
 *
 * `prev` is optional: the first sample of a session has nothing to diff
 * against, and every rate below is a diff.
 */
export function summarise(
  sample: StatsSample | undefined,
  prev?: StatsSample,
): HudSummary {
  if (!sample) return EMPTY;
  const v = inboundVideo(sample);
  const pv = inboundVideo(prev);
  const out: HudSummary = { ...EMPTY };

  // Seconds between samples. Guard against a zero or negative interval: two
  // samples with the same timestamp would divide by zero and render
  // "Infinity fps", which looks like a spectacular success.
  const dt = prev ? (sample.t - prev.t) / 1000 : 0;
  const usable = dt > 0.05;

  if (v) {
    // framesPerSecond is reported directly by getStats where the browser
    // has it; fall back to a frame delta where it does not (Firefox).
    if (typeof v.framesPerSecond === "number") {
      out.fps = `${Math.round(v.framesPerSecond)} fps`;
    } else if (usable && pv && typeof v.framesReceived === "number" &&
               typeof pv.framesReceived === "number") {
      out.fps = `${Math.round((v.framesReceived - pv.framesReceived) / dt)} fps`;
    }

    if (usable && pv) {
      const kbps = ((v.bytesReceived - pv.bytesReceived) * 8) / dt / 1000;
      out.bitrate = kbps >= 1000
        ? `${(kbps / 1000).toFixed(1)} Mbps`
        : `${Math.round(kbps)} kbps`;
    }

    // Loss over the INTERVAL, not since the session began. A session that
    // lost 200 packets in its first second and none since is healthy now,
    // and a cumulative ratio would keep accusing it for as long as it runs.
    if (usable && pv) {
      const dLost = v.packetsLost - pv.packetsLost;
      const dRecv = v.packetsReceived - pv.packetsReceived;
      const total = dLost + dRecv;
      if (total > 0) out.loss = `${((dLost / total) * 100).toFixed(1)}%`;
    }
  }

  const rttSec = sample.candidatePair?.currentRoundTripTime;
  if (typeof rttSec === "number") out.rtt = `${Math.round(rttSec * 1000)} ms`;

  out.quality = verdict(out, rttSec ?? null);
  return out;
}

/**
 * One dot, three states. Thresholds are about what a person NOTICES, not
 * about a network ideal: ~150ms is where interaction starts to feel
 * indirect, and a few percent loss is where video visibly breaks up.
 */
function verdict(s: HudSummary, rttSec: number | null): HudQuality {
  if (s.fps === "—" && s.rtt === "—") return "unknown";
  const lossPct = s.loss === "—" ? 0 : parseFloat(s.loss);
  const fps = s.fps === "—" ? null : parseInt(s.fps, 10);
  const rttMs = rttSec === null ? null : rttSec * 1000;

  if ((rttMs !== null && rttMs > 300) || lossPct > 5 ||
      (fps !== null && fps < 10)) {
    return "poor";
  }
  if ((rttMs !== null && rttMs > 150) || lossPct > 1 ||
      (fps !== null && fps < 20)) {
    return "fair";
  }
  return "good";
}

/** Resolution is read off the <video>, not from getStats — see attachHud. */
export function formatResolution(w: number, h: number): string {
  return w > 0 && h > 0 ? `${w}x${h}` : "—";
}

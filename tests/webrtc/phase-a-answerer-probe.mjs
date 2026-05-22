#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
//
// CV2 Phase A answerer probe — minimum-viable. Authored 2026-05-18 to
// close the "4 DataChannels OPEN" verdict bar after CV2-69 cleared the
// offerer-side architectural + threading layers (see commit 03aa198 on
// cv2/m7-integration-staged + verdict /tmp/cv2-phase-a-rt5-verdict.md).
//
// What this is:
//   A standalone Node script that acts as the role=client side of the
//   CV2 native-peer signaling handshake. It dials the signaling broker,
//   registers as client, receives the worker's SDP offer from the
//   broker's replay buffer (T96/T104), generates + sends an SDP answer,
//   exchanges ICE candidates, and asserts that each of the four
//   DataChannels created by the worker (input / cursor / clipboard /
//   files) transitions to onopen. Exits 0 on PASS, 1 on FAIL.
//
// What this is NOT:
//   Not a Phase B input scenario (no mouse/keyboard dispatch). Not a
//   video sink (no RTCVideoSink / ffmpeg). Not a long-lived peer (it
//   exits as soon as the verdict resolves). Phase B uses
//   scenarios/runner.mjs against the streamer-page; this probe is the
//   isolated "connect + assert DC.onopen" smoke test the harness was
//   missing.
//
// Wire contract:
//   The broker (signaling/server.go) enforces a JSON envelope shape
//   `{type, from, data?}` with six valid type tags: offer / answer /
//   ice / bye / request_renegotiate / probe_result (cb_wire_envelope.h).
//   It reads the first envelope to learn the peer's role from the
//   `from` field; unknown type tags are logged and dropped but
//   registration still succeeds. This probe sends a `hello`-tagged
//   first envelope: that's not in validTypes, so the broker registers
//   us as role=client + replays the buffered offer to us, but does
//   NOT forward the hello to the browser peer. Cleanest registration
//   primitive given the wire contract.
//
// Threading / ICE:
//   No iceServers — both peers run in the same pod's netns, so host
//   candidates pair via loopback without STUN. Outbound STUN to
//   stun.l.google.com:19302 is blocked by the pod NetworkPolicy
//   anyway (worker logs `stun_port.cc:123 Binding request timed out`),
//   which is by design for the test pod — host pairing is the only
//   needed path.

import WebSocket from "ws";
import wrtc from "@roamhq/wrtc";

const { RTCPeerConnection, RTCIceCandidate, RTCSessionDescription } = wrtc;

// ───────── config ─────────

const BROKER_URL = process.env.BROKER_URL
  || "ws://localhost:8080/api/webrtc/signaling/cv2-phase-a-001";
const TIMEOUT_MS = parseInt(process.env.PROBE_TIMEOUT_MS || "60000", 10);
// Match cloud_browser_browser_main_parts.cc:609/619/629/643 — the four
// labels the worker creates on the PC. "files" (not "file-upload") is
// the hard-fixed Trap #1 from the v3 narrative.
const EXPECTED_LABELS = Object.freeze(["input", "cursor", "clipboard", "files"]);

// ───────── log helper (single-line JSON, mirrors broker's slog) ─────────

function log(level, msg, extra) {
  const line = { ts: new Date().toISOString(), level, msg, ...(extra || {}) };
  console.log(JSON.stringify(line));
}

// ───────── main ─────────

async function main() {
  log("info", "probe start", {
    broker: BROKER_URL,
    timeout_ms: TIMEOUT_MS,
    expected_labels: EXPECTED_LABELS,
  });

  const ws = new WebSocket(BROKER_URL);
  const pc = new RTCPeerConnection({ iceServers: [] });

  const openedLabels = new Set();
  const seenChannels = new Map();
  let resolveDone;
  let rejectDone;
  const done = new Promise((res, rej) => { resolveDone = res; rejectDone = rej; });

  const timeout = setTimeout(() => {
    rejectDone(new Error(`timeout ${TIMEOUT_MS}ms; opened=[${[...openedLabels].join(",")}]`
      + ` seen=[${[...seenChannels.keys()].join(",")}]`
      + ` ice=${pc.iceConnectionState} conn=${pc.connectionState}`));
  }, TIMEOUT_MS);

  function send(env) {
    if (ws.readyState !== WebSocket.OPEN) {
      log("warn", "ws not open; dropping send", { type: env.type });
      return;
    }
    ws.send(JSON.stringify(env));
  }

  // ───── PeerConnection wiring ─────

  pc.onicecandidate = (ev) => {
    if (!ev.candidate) {
      // End-of-candidates: per the wire contract, `data: null`.
      send({ type: "ice", from: "client", data: null });
      log("info", "ICE: emitted end-of-candidates");
      return;
    }
    send({ type: "ice", from: "client", data: ev.candidate.toJSON() });
  };

  pc.oniceconnectionstatechange = () =>
    log("info", `iceConnectionState=${pc.iceConnectionState}`);
  pc.onconnectionstatechange = () =>
    log("info", `connectionState=${pc.connectionState}`);
  pc.onicegatheringstatechange = () =>
    log("info", `iceGatheringState=${pc.iceGatheringState}`);
  pc.onsignalingstatechange = () =>
    log("info", `signalingState=${pc.signalingState}`);

  pc.ondatachannel = (ev) => {
    const dc = ev.channel;
    log("info", "ondatachannel", { label: dc.label, readyState: dc.readyState });
    seenChannels.set(dc.label, dc);
    // If a DC arrives already-open, count it immediately (wrtc may
    // deliver an already-open channel under fast-handshake conditions).
    if (dc.readyState === "open") {
      markOpen(dc.label);
    }
    dc.onopen = () => markOpen(dc.label);
    dc.onerror = (err) => log("err", `DC.onerror "${dc.label}"`, { err: String(err?.error || err) });
    dc.onclose = () => log("info", `DC.onclose "${dc.label}"`);
  };

  function markOpen(label) {
    if (openedLabels.has(label)) return;
    openedLabels.add(label);
    log("ok", `DC.onopen "${label}"`, { opened: [...openedLabels] });
    if (EXPECTED_LABELS.every((l) => openedLabels.has(l))) {
      log("ok", "ALL 4 DataChannels OPEN — acceptance bar cleared");
      clearTimeout(timeout);
      resolveDone();
    }
  }

  // ───── WebSocket wiring ─────

  ws.on("open", () => {
    log("info", "ws open; registering as client via hello envelope");
    // `hello` is NOT a valid wire type — the broker logs a warning,
    // drops the frame (no forward to browser), but completes our
    // registration as role=client and replays the buffered offer to
    // us. See signaling/server.go:560-566.
    send({ type: "hello", from: "client" });
  });

  ws.on("message", async (raw) => {
    let env;
    try {
      env = JSON.parse(raw.toString("utf8"));
    } catch (e) {
      log("err", "envelope: invalid JSON",
        { raw: raw.toString("utf8").slice(0, 200), err: String(e) });
      return;
    }
    try {
      switch (env.type) {
        case "offer": {
          const sdp = env.data?.sdp;
          if (!sdp) { log("err", "offer envelope missing data.sdp", { env }); return; }
          log("info", "offer received", { sdp_len: sdp.length });
          await pc.setRemoteDescription(new RTCSessionDescription({ type: "offer", sdp }));
          log("info", "setRemoteDescription(offer) OK");
          const answer = await pc.createAnswer();
          await pc.setLocalDescription(answer);
          log("info", "setLocalDescription(answer) OK", { sdp_len: answer.sdp.length });
          send({ type: "answer", from: "client",
            data: { type: "answer", sdp: answer.sdp } });
          log("info", "answer sent");
          break;
        }
        case "ice": {
          if (env.data === null || env.data === undefined) {
            log("info", "ICE: peer signalled end-of-candidates");
            // Some wrtc builds accept addIceCandidate(null) for EOC, others
            // raise. Tolerate either; the host-only pairing doesn't depend
            // on the explicit EOC marker (libwebrtc handles missing-EOC).
            try { await pc.addIceCandidate(null); }
            catch (e) { log("warn", "addIceCandidate(null) rejected", { err: String(e) }); }
            break;
          }
          await pc.addIceCandidate(new RTCIceCandidate(env.data));
          break;
        }
        case "bye":
          log("warn", "peer sent bye");
          break;
        case "request_renegotiate":
          log("info", "peer requested renegotiate (ignored — not in Phase A scope)");
          break;
        default:
          log("debug", "ignoring envelope type", { type: env.type });
      }
    } catch (e) {
      log("err", "envelope handler threw",
        { type: env.type, err: String(e), stack: e?.stack });
    }
  });

  ws.on("error", (err) => {
    log("err", "ws error", { err: String(err) });
    rejectDone(err);
  });
  ws.on("close", (code, reason) =>
    log("info", "ws close", { code, reason: reason?.toString() ?? "" }));

  // ───── exit ─────

  try {
    await done;
    log("ok", "VERDICT: PASS", { opened: [...openedLabels] });
    try { pc.close(); } catch {}
    try { ws.close(1000, "probe done"); } catch {}
    // wrtc keeps an event loop alive; force-exit cleanly.
    process.exit(0);
  } catch (e) {
    log("err", "VERDICT: FAIL", {
      err: String(e),
      opened: [...openedLabels],
      seen: [...seenChannels.keys()],
      iceConnectionState: pc.iceConnectionState,
      connectionState: pc.connectionState,
      signalingState: pc.signalingState,
    });
    try { pc.close(); } catch {}
    try { ws.close(1011, "probe failed"); } catch {}
    process.exit(1);
  }
}

main().catch((e) => {
  log("err", "main threw", { err: String(e), stack: e?.stack });
  process.exit(1);
});

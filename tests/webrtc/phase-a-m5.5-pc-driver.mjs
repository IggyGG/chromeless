#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
//
// CV2-82 M5.5 PC-driver peer — verification-lead 2026-05-20.
//
// WHY THIS EXISTS
// ----------------
// phase-a-m5.5-r1-pulse-init.mjs verifies the audio ADM by reading
// chromium.err.log PASSIVELY for the LS_INFO line
//   "native AudioDeviceModule constructed AND initialized
//    (kPlatformDefaultAudio / built-in PulseAudio)"
// emitted from CreateCloudBrowserNativeAudioDeviceModule()
// (cb_audio_device_module.cc:116).
//
// But that function runs inside BuildPCF -> EnableMedia (cloud_browser_pcf.cc),
// and the media-PCF is built LAZILY — on PeerConnection creation, not at
// worker boot. An idle signaling-connected worker with NO peer never builds
// the media-PCF, so the ADM Init never runs and the R1 harness reads
// "LS_INFO absent" — a false-NEGATIVE (the upstream gate, a PC existing,
// was never satisfied), NOT a CV2-82 defect.
//
// This driver is the gate-satisfier: it joins the worker's signaling
// session as a WebRTC peer, completes the handshake so the worker builds
// its PeerConnection (-> media-PCF -> ADM Init -> the LS_INFO line lands),
// holds briefly so the log write settles, then exits. Run it BEFORE
// phase-a-m5.5-r1-pulse-init.mjs when R1 would otherwise read an idle
// worker.
//
// It deliberately does NOT verify anything itself — it only drives the
// worker into the state the R1 log-read harness expects. Single-variable
// discipline: this script owns the WIRE side; the R1 harness owns the
// verdict.
//
// USAGE
//   node tests/webrtc/phase-a-m5.5-pc-driver.mjs
//     [BROKER_URL env — default ws://localhost:8080/api/webrtc/signaling/cv2-82-real-pulse]
//   Exit 0 = handshake reached (worker PC built); exit 2 = handshake timeout.
//
// Requires a kubectl port-forward of the signaling broker Service to
// localhost:8080 (see cv2-wave2-reverify-runbook.sh).

import WebSocket from "ws";
import wrtc from "@roamhq/wrtc";

const { RTCPeerConnection, RTCIceCandidate, RTCSessionDescription } = wrtc;

const BROKER_URL = process.env.BROKER_URL
  || "ws://localhost:8080/api/webrtc/signaling/cv2-82-real-pulse";
const HANDSHAKE_TIMEOUT_MS = parseInt(process.env.HANDSHAKE_TIMEOUT_MS || "60000", 10);
// Hold after handshake so the worker finishes BuildPCF -> EnableMedia ->
// CreateCloudBrowserNativeAudioDeviceModule -> adm->Init() and flushes the
// LS_INFO/LS_WARNING line to chromium.err.log before we drop the peer.
const POST_HANDSHAKE_HOLD_MS = parseInt(process.env.POST_HANDSHAKE_HOLD_MS || "8000", 10);

function log(level, msg, extra) {
  console.log(JSON.stringify({ ts: new Date().toISOString(), level, msg, ...(extra || {}) }));
}

async function main() {
  log("info", "m5.5 pc-driver start", {
    broker: BROKER_URL,
    handshake_timeout_ms: HANDSHAKE_TIMEOUT_MS,
    post_handshake_hold_ms: POST_HANDSHAKE_HOLD_MS,
  });

  const ws = new WebSocket(BROKER_URL);
  const pc = new RTCPeerConnection({ iceServers: [] });

  const openedLabels = new Set();
  let resolveHs, rejectHs;
  const handshakeDone = new Promise((res, rej) => { resolveHs = res; rejectHs = rej; });
  const hsTimeout = setTimeout(
    () => rejectHs(new Error(`handshake timeout ${HANDSHAKE_TIMEOUT_MS}ms; opened=[${[...openedLabels]}]`)),
    HANDSHAKE_TIMEOUT_MS);

  function send(env) {
    if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(env));
  }

  // Either signal that the worker's PeerConnection is fully up is enough to
  // prove the media-PCF was built: connectionState=connected, OR all DCs open.
  function maybeDone(reason) {
    clearTimeout(hsTimeout);
    log("ok", "handshake established — worker PC built", { reason, opened: [...openedLabels] });
    resolveHs();
  }

  pc.onicecandidate = (ev) => {
    if (!ev.candidate) { send({ type: "ice", from: "client", data: null }); return; }
    send({ type: "ice", from: "client", data: ev.candidate.toJSON() });
  };
  pc.oniceconnectionstatechange = () => log("info", `iceConnectionState=${pc.iceConnectionState}`);
  pc.onconnectionstatechange = () => {
    log("info", `connectionState=${pc.connectionState}`);
    if (pc.connectionState === "connected") maybeDone("connectionState=connected");
  };
  pc.ondatachannel = (ev) => {
    const dc = ev.channel;
    log("info", "ondatachannel", { label: dc.label });
    const onOpen = () => {
      openedLabels.add(dc.label);
      log("ok", `DC.onopen "${dc.label}"`, { opened: [...openedLabels] });
      // 4 DCs is the worker's full set (input/cursor/clipboard/files); any
      // open DC already proves SCTP + the PC negotiated, but wait for the
      // worker's standard 4 to be sure media negotiation also completed.
      if (openedLabels.size >= 4) maybeDone("all DCs open");
    };
    if (dc.readyState === "open") onOpen();
    dc.onopen = onOpen;
  };

  ws.on("open", () => send({ type: "hello", from: "client" }));
  ws.on("message", async (raw) => {
    let env;
    try { env = JSON.parse(raw.toString("utf8")); } catch { return; }
    if (env.type === "offer") {
      const sdp = env.data?.sdp;
      if (!sdp) return;
      log("info", "offer received", { sdp_len: sdp.length });
      await pc.setRemoteDescription(new RTCSessionDescription({ type: "offer", sdp }));
      const answer = await pc.createAnswer();
      await pc.setLocalDescription(answer);
      send({ type: "answer", from: "client", data: { type: "answer", sdp: answer.sdp } });
    } else if (env.type === "ice") {
      if (env.data == null) { try { await pc.addIceCandidate(null); } catch {} return; }
      try { await pc.addIceCandidate(new RTCIceCandidate(env.data)); }
      catch (e) { log("warn", "addIceCandidate failed", { err: String(e) }); }
    }
  });
  ws.on("error", (err) => { log("err", "ws error", { err: String(err) }); rejectHs(err); });

  try {
    await handshakeDone;
  } catch (e) {
    log("err", "VERDICT: handshake FAILED — worker PC not driven", { err: String(e) });
    try { pc.close(); ws.close(1011); } catch {}
    process.exit(2);
  }

  log("info", "holding so worker finishes BuildPCF -> ADM Init", { hold_ms: POST_HANDSHAKE_HOLD_MS });
  await new Promise((r) => setTimeout(r, POST_HANDSHAKE_HOLD_MS));

  log("ok", "PC-DRIVER COMPLETE — media-PCF/ADM construction driven; "
    + "now run phase-a-m5.5-r1-pulse-init.mjs to read the audio verdict");
  try { pc.close(); ws.close(1000); } catch {}
  process.exit(0);
}

main().catch((e) => {
  log("err", "main threw", { err: String(e), stack: e?.stack });
  process.exit(1);
});

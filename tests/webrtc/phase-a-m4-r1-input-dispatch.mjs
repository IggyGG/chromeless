#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
//
// CV2 M4 R1 input-dispatch verdict harness. Extends phase-a-answerer-probe.mjs
// by sending a synthetic input envelope after the 4-DC handshake completes,
// then waiting briefly so cb-chromium has time to: (a) deliver via SCTP to
// CbInputDispatch::OnMessage on the libwebrtc signaling thread, (b) decode
// the v1 envelope, (c) PostTask to BrowserThread::UI, (d) invoke
// CbInputLoggingDelegate::OnInputEvent which LOG(INFO) emits
// `CbInputDispatch: dispatched type=<type> seq=<seq> t=<t>` (cb_input_dispatch.cc:301).
//
// Verdict split — single-variable discipline:
//   - This script handles the WIRE side (handshake + envelope send).
//   - The kubectl-logs grep happens OUTSIDE (orchestrator), to keep the
//     wire-success vs log-evidence axes separate. Worker-stderr grep is
//     done by `kubectl logs ... -c cb-chromium | grep "CbInputDispatch:"`.
//   - Script exits 0 if wire-handshake + envelope send succeeds; the M4 R1
//     PASS/FAIL verdict is derived from the log grep, not from this exit code.
//
// Pre-traffic gate (CV2-75 startup-confirm):
//   The orchestrator's pre-traffic grep should find at least:
//     - CV2-75: "input" DC observer = CbInputDispatch (R1 logging delegate)
//     - CV2-75: "clipboard" DC observer = CbClipboardRelay
//     - CV2-75: "files" DC observer = CbFileUploadRelay
//   "cursor" startup-confirm is expected ABSENT (M5 R6 deferred to CV2-77).
//
// Per-event verdict:
//   - PASS: `CbInputDispatch: dispatched type=keydown seq=1` within ~2s post-send
//   - FAIL classes (each maps to a distinct log signature):
//     - decode-error WARNING `CbInputDispatch: decode error: <reason>`: envelope shape bad
//     - unknown-v1-type WARNING `CbInputDispatch: unknown v1 type="..."`: type whitelist drift
//     - silence (no match): observer not receiving OnMessage → R0 wiring failed
//     - worker crash post-send: thread-discipline regression → build-czar
//
// Envelope shape per docs/protocols/input-channel.md + cb_input_dispatch.cc
// kKnownInputTypes:
//   { v: 1, type: "keydown", t: <epoch_ms>, seq: <int>, data: {...} }
//
// NB: "keydown" maps to cb_input_dispatch.cc:55 kKnownInputTypes entry "key_down"
// — the camelCase vs snake_case is wire-spec; this script uses snake_case for
// strict match. If the worker rejects with unknown_v1_type, the test-harness
// has the type-name drift, not the worker.

import WebSocket from "ws";
import wrtc from "@roamhq/wrtc";

const { RTCPeerConnection, RTCIceCandidate, RTCSessionDescription } = wrtc;

const BROKER_URL = process.env.BROKER_URL
  || "ws://localhost:8080/api/webrtc/signaling/cv2-m4-r1-input-dispatch-001";
const HANDSHAKE_TIMEOUT_MS = parseInt(process.env.HANDSHAKE_TIMEOUT_MS || "60000", 10);
const POST_SEND_WAIT_MS = parseInt(process.env.POST_SEND_WAIT_MS || "3000", 10);
const EXPECTED_LABELS = Object.freeze(["input", "cursor", "clipboard", "files"]);

function log(level, msg, extra) {
  const line = { ts: new Date().toISOString(), level, msg, ...(extra || {}) };
  console.log(JSON.stringify(line));
}

async function main() {
  log("info", "m4-r1 input-dispatch test start", {
    broker: BROKER_URL,
    handshake_timeout_ms: HANDSHAKE_TIMEOUT_MS,
    post_send_wait_ms: POST_SEND_WAIT_MS,
    expected_labels: EXPECTED_LABELS,
  });

  const ws = new WebSocket(BROKER_URL);
  const pc = new RTCPeerConnection({ iceServers: [] });

  const openedLabels = new Set();
  const dcs = new Map();
  let resolveHandshake, rejectHandshake;
  const handshakeDone = new Promise((res, rej) => { resolveHandshake = res; rejectHandshake = rej; });

  const handshakeTimeout = setTimeout(() => {
    rejectHandshake(new Error(`handshake timeout ${HANDSHAKE_TIMEOUT_MS}ms; opened=[${[...openedLabels].join(",")}]`));
  }, HANDSHAKE_TIMEOUT_MS);

  function send(env) {
    if (ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify(env));
  }

  pc.onicecandidate = (ev) => {
    if (!ev.candidate) { send({ type: "ice", from: "client", data: null }); return; }
    send({ type: "ice", from: "client", data: ev.candidate.toJSON() });
  };
  pc.oniceconnectionstatechange = () => log("info", `iceConnectionState=${pc.iceConnectionState}`);
  pc.onconnectionstatechange = () => log("info", `connectionState=${pc.connectionState}`);

  pc.ondatachannel = (ev) => {
    const dc = ev.channel;
    log("info", "ondatachannel", { label: dc.label, readyState: dc.readyState });
    dcs.set(dc.label, dc);
    const onOpen = () => {
      if (openedLabels.has(dc.label)) return;
      openedLabels.add(dc.label);
      log("ok", `DC.onopen "${dc.label}"`, { opened: [...openedLabels] });
      if (EXPECTED_LABELS.every((l) => openedLabels.has(l))) {
        log("ok", "handshake complete — all 4 DCs open");
        clearTimeout(handshakeTimeout);
        resolveHandshake();
      }
    };
    if (dc.readyState === "open") onOpen();
    dc.onopen = onOpen;
    dc.onerror = (err) => log("err", `DC.onerror "${dc.label}"`, { err: String(err?.error || err) });
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
      if (env.data === null || env.data === undefined) {
        try { await pc.addIceCandidate(null); } catch {}
        return;
      }
      try { await pc.addIceCandidate(new RTCIceCandidate(env.data)); } catch (e) {
        log("warn", "addIceCandidate failed", { err: String(e) });
      }
    }
  });
  ws.on("error", (err) => { log("err", "ws error", { err: String(err) }); rejectHandshake(err); });

  // ───── Phase 1: wait for 4-DC handshake ─────
  try {
    await handshakeDone;
  } catch (e) {
    log("err", "VERDICT: HANDSHAKE FAIL", { err: String(e) });
    try { pc.close(); ws.close(1011); } catch {}
    process.exit(2);  // exit 2 = handshake regression, NOT M4 R1 defect
  }

  // ───── Phase 2: send synthetic input envelope on input DC ─────
  const inputDc = dcs.get("input");
  if (!inputDc) {
    log("err", "VERDICT: NO INPUT DC", {});
    process.exit(3);
  }

  const synthetic = {
    v: 1,
    type: "key_down",  // snake_case per kKnownInputTypes in cb_input_dispatch.cc:55
    t: Date.now(),
    seq: 1,
    data: { code: "KeyA", modifiers: 0 },
  };
  const payload = JSON.stringify(synthetic);
  log("info", "sending synthetic input envelope", { envelope: synthetic, dc_state: inputDc.readyState });
  try {
    inputDc.send(payload);
    log("ok", "envelope sent on input DC", { bytes: payload.length });
  } catch (e) {
    log("err", "VERDICT: DC SEND FAIL", { err: String(e) });
    process.exit(4);
  }

  // ───── Phase 3: wait briefly so worker has time to log the dispatch ─────
  await new Promise((r) => setTimeout(r, POST_SEND_WAIT_MS));

  log("ok", "WIRE VERDICT: SEND COMPLETE; verdict derived externally from cb-chromium logs", {
    grep_for_pass: 'CbInputDispatch: dispatched type=key_down seq=1',
    grep_for_decode_error: 'CbInputDispatch: decode error:',
    grep_for_unknown_type: 'CbInputDispatch: unknown v1 type=',
    grep_for_startup_wiring: 'CV2-75: "input" DC observer = CbInputDispatch',
  });
  try { pc.close(); ws.close(1000); } catch {}
  process.exit(0);
}

main().catch((e) => {
  log("err", "main threw", { err: String(e), stack: e?.stack });
  process.exit(1);
});

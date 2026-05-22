#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
//
// CV2-83 focused native cursor probe.
//
// This harness keeps the stimulus narrow: it uses CDP only to create a
// rendered `cursor:pointer` target and to call Cb.startFrameSinkCapture, then
// sends one `mouse_move` envelope over the WebRTC input data channel. The
// verdict remains external: grep cb-chromium stderr for
// `CbCursorClient::SetCursor.*new_type=2` and, on diagnostic images,
// `CV2-83-PROBE`.

import WebSocket from "ws";
import wrtc from "@roamhq/wrtc";
import CDP from "chrome-remote-interface";

const { RTCPeerConnection, RTCIceCandidate, RTCSessionDescription } = wrtc;

const BROKER_URL = process.env.BROKER_URL
  || "ws://localhost:8080/api/webrtc/signaling/cv2-83-cursor-native";
const CDP_HOST = process.env.CDP_HOST || "localhost";
const CDP_PORT = parseInt(process.env.CDP_PORT || "9222", 10);
const CDP_CONNECT_TIMEOUT_MS = parseInt(process.env.CDP_CONNECT_TIMEOUT_MS || "60000", 10);
const HANDSHAKE_TIMEOUT_MS = parseInt(process.env.HANDSHAKE_TIMEOUT_MS || "60000", 10);
const POST_SEND_WAIT_MS = parseInt(process.env.POST_SEND_WAIT_MS || "5000", 10);

const STIMULUS_TARGET_HTML =
  '<html><body style="margin:0">'
  + '<a id="target" href="#" style="display:inline-block;width:120px;height:80px;cursor:pointer;background:#ddd;color:#111">target</a>'
  + '</body></html>';
const STIMULUS_TARGET_URL = process.env.STIMULUS_TARGET_URL
  || ("data:text/html," + encodeURIComponent(STIMULUS_TARGET_HTML));

function log(level, msg, extra) {
  const line = { ts: new Date().toISOString(), level, msg, ...(extra || {}) };
  console.log(JSON.stringify(line));
}

async function waitForCdpReady() {
  const deadline = Date.now() + CDP_CONNECT_TIMEOUT_MS;
  let lastErr;
  while (Date.now() < deadline) {
    try {
      const targets = await CDP.List({ host: CDP_HOST, port: CDP_PORT });
      if (targets && targets.length > 0) {
        log("ok", "cb-chromium CDP up", { targets: targets.length });
        return;
      }
      lastErr = new Error("CDP up but no targets");
    } catch (e) {
      lastErr = e;
    }
    await new Promise((r) => setTimeout(r, 500));
  }
  throw new Error(`CDP not ready after ${CDP_CONNECT_TIMEOUT_MS}ms: ${lastErr}`);
}

async function prepareRendererTarget() {
  await waitForCdpReady();

  let client;
  try {
    client = await CDP({ host: CDP_HOST, port: CDP_PORT, local: true });
    log("ok", "cdp client attached");
  } catch (e) {
    log("err", "CDP attach failed", { err: String(e) });
    throw e;
  }

  const { Page, Runtime } = client;
  try {
    await Page.enable();
    await Runtime.enable();

    const navStart = Date.now();
    await Page.navigate({ url: STIMULUS_TARGET_URL });
    await Page.loadEventFired();
    await new Promise((r) => setTimeout(r, 300));
    log("ok", "navigation complete", { nav_ms: Date.now() - navStart });

    const probe = await Runtime.evaluate({
      expression: `(() => {
        const el = document.querySelector("#target");
        if (!el) return null;
        const r = el.getBoundingClientRect();
        return {
          x: r.left,
          y: r.top,
          w: r.width,
          h: r.height,
          center_x: r.left + r.width / 2,
          center_y: r.top + r.height / 2,
          cursor: getComputedStyle(el).cursor,
          any_link: el.matches(":any-link"),
        };
      })()`,
      returnByValue: true,
    });
    const rect = probe.result?.value;
    log("info", "target probe", { rect });
    if (!rect || rect.w <= 0 || rect.h <= 0) {
      throw new Error(`renderer target not laid out: ${JSON.stringify(rect)}`);
    }

    const captureResult = await client.send("Cb.startFrameSinkCapture");
    log("ok", "Cb.startFrameSinkCapture complete", captureResult || {});

    return {
      x: Math.round(rect.center_x),
      y: Math.round(rect.center_y),
      rect,
    };
  } finally {
    try { await client.close(); } catch {}
  }
}

async function connectInputDataChannel() {
  const ws = new WebSocket(BROKER_URL);
  const pc = new RTCPeerConnection({ iceServers: [] });

  let resolveHandshake;
  let rejectHandshake;
  const handshakeDone = new Promise((res, rej) => {
    resolveHandshake = res;
    rejectHandshake = rej;
  });

  const handshakeTimeout = setTimeout(() => {
    rejectHandshake(new Error(`handshake timeout ${HANDSHAKE_TIMEOUT_MS}ms`));
  }, HANDSHAKE_TIMEOUT_MS);

  function send(env) {
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(env));
    }
  }

  pc.onicecandidate = (ev) => {
    if (!ev.candidate) {
      send({ type: "ice", from: "client", data: null });
      return;
    }
    send({ type: "ice", from: "client", data: ev.candidate.toJSON() });
  };
  pc.oniceconnectionstatechange = () => log("info", `iceConnectionState=${pc.iceConnectionState}`);
  pc.onconnectionstatechange = () => log("info", `connectionState=${pc.connectionState}`);

  pc.ondatachannel = (ev) => {
    const dc = ev.channel;
    log("info", "ondatachannel", { label: dc.label, readyState: dc.readyState });
    if (dc.label !== "input") {
      return;
    }
    const onOpen = () => {
      log("ok", "input DC open");
      clearTimeout(handshakeTimeout);
      resolveHandshake({ pc, ws, inputDc: dc });
    };
    if (dc.readyState === "open") {
      onOpen();
    }
    dc.onopen = onOpen;
    dc.onerror = (err) => log("err", "input DC error", { err: String(err?.error || err) });
  };

  ws.on("open", () => send({ type: "hello", from: "client" }));
  ws.on("message", async (raw) => {
    let env;
    try {
      env = JSON.parse(raw.toString("utf8"));
    } catch {
      return;
    }
    if (env.type === "offer") {
      const sdp = env.data?.sdp;
      if (!sdp) {
        return;
      }
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
      try {
        await pc.addIceCandidate(new RTCIceCandidate(env.data));
      } catch (e) {
        log("warn", "addIceCandidate failed", { err: String(e) });
      }
    }
  });
  ws.on("error", (err) => {
    log("err", "ws error", { err: String(err) });
    rejectHandshake(err);
  });

  return handshakeDone;
}

async function main() {
  log("info", "m5-r1 native cursor probe start", {
    broker: BROKER_URL,
    cdp: `${CDP_HOST}:${CDP_PORT}`,
    handshake_timeout_ms: HANDSHAKE_TIMEOUT_MS,
    post_send_wait_ms: POST_SEND_WAIT_MS,
  });

  const target = await prepareRendererTarget();
  const { pc, ws, inputDc } = await connectInputDataChannel();

  const envelope = {
    v: 1,
    type: "mouse_move",
    t: 1,
    seq: 1,
    data: { x: target.x, y: target.y },
  };
  const payload = JSON.stringify(envelope);
  log("info", "sending native cursor mouse_move", {
    envelope,
    bytes: payload.length,
    target_rect: target.rect,
  });
  inputDc.send(payload);

  await new Promise((r) => setTimeout(r, POST_SEND_WAIT_MS));

  log("ok", "WIRE VERDICT: native cursor stimulus complete; verdict derived externally", {
    grep_for_pass: "CbCursorClient::SetCursor.*new_type=2",
    grep_for_probe: "CV2-83-PROBE",
    wait_window_ms: POST_SEND_WAIT_MS,
  });

  try { pc.close(); ws.close(1000); } catch {}
}

main().catch((e) => {
  log("err", "main threw", { err: String(e), stack: e?.stack });
  process.exit(1);
});

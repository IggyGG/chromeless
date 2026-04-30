// v0 browser client for cloud-browser-webrtc.
//
// Per docs/capture/path-of-least-resistance.md (T15) and T23, the
// streamer (capture/streamer-page) holds the media and is therefore
// the WebRTC offerer. This client is the **answerer**: it opens the
// signaling websocket, waits for the streamer's offer, replies with an
// SDP answer, and trickles ICE.
//
// Scope:
//   - Connect to the signaling server (T13) at ws://localhost:8080/ws/{session}.
//   - Fetch ICE config (T25) before constructing the RTCPeerConnection.
//   - Wait for `{type: "offer", from: "browser"}` from the streamer.
//   - createAnswer → prioritizeCodec(VP9) → setLocalDescription → send answer.
//   - Receive any incoming media tracks via `pc.ontrack`; attach to <video>.
//   - Receive any incoming data channels via `pc.ondatachannel`. When the
//     streamer creates the "input" channel, wrap it with InputChannel
//     (T20) and attach DOM listeners that forward mouse/keyboard.
//   - Surface signaling/ICE/connection state to the debug panel.
//
// Out of scope: T37 reconnect logic, T34 follow-up (streamer-side
// creation of the "input" data channel — until that lands the channel
// just won't appear and InputChannel stays detached).

import { InputChannel } from "./src/input.js";
import { fetchTurnConfig } from "./src/turn.js";
import { prioritizeCodec } from "./src/sdp.js";

const DEFAULT_SIGNALING = "ws://localhost:8080/ws";

type Envelope =
  | { type: "offer";  from: "client" | "browser"; data: RTCSessionDescriptionInit }
  | { type: "answer"; from: "client" | "browser"; data: RTCSessionDescriptionInit }
  | { type: "ice";    from: "client" | "browser"; data: RTCIceCandidateInit | null }
  | { type: "bye";    from: "client" | "browser"; data?: undefined };

type LogLevel = "info" | "ok" | "warn" | "err";

// ---------- DOM ----------

const $ = <T extends HTMLElement>(id: string): T => {
  const el = document.getElementById(id);
  if (!el) throw new Error(`#${id} not found`);
  return el as T;
};

const els = {
  connect: $<HTMLButtonElement>("connect"),
  sessionId: $<HTMLInputElement>("session-id"),
  status: $<HTMLSpanElement>("status"),
  statusText: $<HTMLSpanElement>("status-text"),
  video: $<HTMLVideoElement>("remote"),
  log: $<HTMLPreElement>("log"),
  sig: $<HTMLElement>("state-sig"),
  ice: $<HTMLElement>("state-ice"),
  iceg: $<HTMLElement>("state-iceg"),
  conn: $<HTMLElement>("state-conn"),
  dc: $<HTMLElement>("state-dc"),
};

function setStatus(state: "idle" | "connecting" | "connected" | "failed" | "closed", text?: string): void {
  els.status.dataset["state"] = state;
  els.statusText.textContent = text ?? state;
}

function log(level: LogLevel, msg: string, extra?: unknown): void {
  const ts = new Date().toISOString().slice(11, 23);
  const tail = extra === undefined ? "" : "  " + safeStringify(extra);
  const line = document.createElement("div");
  line.innerHTML =
    `<span class="ts">${ts}</span> <span class="lvl-${level}">${level.toUpperCase().padEnd(4)}</span> ${escapeHtml(msg)}${escapeHtml(tail)}`;
  els.log.appendChild(line);
  els.log.scrollTop = els.log.scrollHeight;
  // Mirror to devtools.
  const c = level === "err" ? console.error : level === "warn" ? console.warn : console.log;
  c(`[${level}]`, msg, extra ?? "");
}

function safeStringify(v: unknown): string {
  try { return typeof v === "string" ? v : JSON.stringify(v); } catch { return String(v); }
}

function escapeHtml(s: string): string {
  return s.replace(/[&<>"']/g, ch => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  }[ch] ?? ch));
}

// ---------- session ----------

interface Session {
  ws: WebSocket;
  pc: RTCPeerConnection;
  /** Set when the streamer has created the "input" data channel. */
  dc: RTCDataChannel | null;
  /** Set when an InputChannel has been wired up to the dc. */
  input: InputChannel | null;
  detachInput: (() => void) | null;
}

let active: Session | null = null;

function teardown(reason: string): void {
  if (!active) return;
  log("info", `tearing down: ${reason}`);
  try { active.detachInput?.(); } catch { /* ignore */ }
  try { active.dc?.close(); } catch { /* ignore */ }
  try { active.pc.close(); } catch { /* ignore */ }
  if (active.ws.readyState === WebSocket.OPEN) {
    try {
      active.ws.send(JSON.stringify({ type: "bye", from: "client" } satisfies Envelope));
    } catch { /* ignore */ }
  }
  try { active.ws.close(); } catch { /* ignore */ }
  active = null;
  setStatus("closed");
  els.connect.disabled = false;
  els.connect.textContent = "Connect";
}

// Map page coords into the source video's intrinsic pixel space,
// undoing object-fit:contain. The remote expects coords in the source
// coordinate system. Reused by the input data channel handler when it
// arrives.
function videoContentMapper(cx: number, cy: number, rect: DOMRect): { x: number; y: number } {
  const v = els.video;
  const vw = v.videoWidth || rect.width;
  const vh = v.videoHeight || rect.height;
  const scale = Math.min(rect.width / vw, rect.height / vh);
  const dispW = vw * scale;
  const dispH = vh * scale;
  const padX = (rect.width  - dispW) / 2;
  const padY = (rect.height - dispH) / 2;
  return {
    x: Math.max(0, Math.min(vw, ((cx - rect.left) - padX) / scale)),
    y: Math.max(0, Math.min(vh, ((cy - rect.top)  - padY) / scale)),
  };
}

function wireInputChannel(dc: RTCDataChannel): void {
  if (!active) return;
  els.dc.textContent = dc.readyState;
  log("ok", `← data channel "${dc.label}" (state=${dc.readyState})`);
  if (dc.label !== "input") {
    log("warn", `ignoring unknown data channel label: ${dc.label}`);
    return;
  }
  const input = new InputChannel(dc, {
    onCoalesce: (n) => log("info", `coalesced ${n} mouse_move`),
    onError: (err) => log("err", "input send failed", String(err)),
  });
  active.dc = dc;
  active.input = input;

  const attachListeners = () => {
    if (!active) return;
    const detach = input.attach(els.video, { toContentCoords: videoContentMapper });
    active.detachInput = detach;
  };
  if (dc.readyState === "open") attachListeners();
  else dc.addEventListener("open", attachListeners, { once: true });

  dc.addEventListener("close", () => {
    els.dc.textContent = "closed";
    log("info", "input data-channel closed");
    if (active) {
      active.detachInput?.();
      active.detachInput = null;
    }
  });
  dc.addEventListener("error", (e) => {
    els.dc.textContent = "error";
    log("err", "input data-channel error", String((e as RTCErrorEvent).error?.message ?? e));
  });
  dc.addEventListener("message", (e) => log("info", "← input.message", e.data));
}

async function connect(sessionId: string): Promise<void> {
  setStatus("connecting", "ws://");
  els.connect.disabled = true;

  const wsUrl = `${DEFAULT_SIGNALING}/${encodeURIComponent(sessionId)}`;
  log("info", `dialing signaling`, wsUrl);

  // Fetch ICE config from the signaling server (T25) before constructing
  // the peer connection. fetchTurnConfig falls back to public STUN on
  // any error so the client still has a chance of working.
  const iceConfig = await fetchTurnConfig(DEFAULT_SIGNALING);
  log("info", "ice config", iceConfig);

  const ws = new WebSocket(wsUrl);
  const pc = new RTCPeerConnection(iceConfig);

  pc.onsignalingstatechange = () => { els.sig.textContent = pc.signalingState; log("info", `signalingState=${pc.signalingState}`); };
  pc.oniceconnectionstatechange = () => {
    els.ice.textContent = pc.iceConnectionState;
    const lvl: LogLevel = pc.iceConnectionState === "failed" ? "err" : "info";
    log(lvl, `iceConnectionState=${pc.iceConnectionState}`);
  };
  pc.onicegatheringstatechange = () => { els.iceg.textContent = pc.iceGatheringState; };
  pc.onconnectionstatechange = () => {
    els.conn.textContent = pc.connectionState;
    if (pc.connectionState === "connected") setStatus("connected");
    else if (pc.connectionState === "failed") setStatus("failed");
    else if (pc.connectionState === "disconnected" || pc.connectionState === "closed") setStatus("closed");
    log(pc.connectionState === "failed" ? "err" : "info", `connectionState=${pc.connectionState}`);
  };

  pc.onicecandidate = (ev) => {
    if (ws.readyState !== WebSocket.OPEN) return;
    const env: Envelope = { type: "ice", from: "client", data: ev.candidate ? ev.candidate.toJSON() : null };
    ws.send(JSON.stringify(env));
    if (ev.candidate) log("info", `→ ice`, ev.candidate.candidate);
    else log("info", `→ ice (end of candidates)`);
  };

  pc.ontrack = (ev) => {
    log("ok", `← track`, { kind: ev.track.kind, id: ev.track.id });
    const stream = ev.streams[0] ?? new MediaStream([ev.track]);
    if (els.video.srcObject !== stream) {
      els.video.srcObject = stream;
    }
  };

  // T34: we no longer createDataChannel("input") on the answerer side.
  // The streamer (T23 follow-up / T41) will offer the data channel as
  // part of its SDP; we receive it here.
  pc.ondatachannel = (ev) => wireInputChannel(ev.channel);

  active = { ws, pc, dc: null, input: null, detachInput: null };
  els.dc.textContent = "—";

  ws.addEventListener("open", () => {
    log("ok", "ws open");
    setStatus("connecting", "waiting for offer");
    // Send a hello so the signaling server learns our role. The
    // server's protocol requires a valid envelope as the first frame
    // (offer|answer|ice|bye); a null-data ice frame is the right
    // no-op — the streamer treats it as "end of candidates" and
    // ignores it. See signaling/server.go::wsHandler.
    ws.send(JSON.stringify({ type: "ice", from: "client", data: null } satisfies Envelope));
  });

  ws.addEventListener("message", async (ev) => {
    let env: Envelope;
    try {
      env = JSON.parse(typeof ev.data === "string" ? ev.data : await (ev.data as Blob).text());
    } catch (err) {
      log("warn", "non-JSON ws frame", String(err));
      return;
    }
    if (env.from === "client") {
      log("warn", "echo from self?", env.type);
      return;
    }
    switch (env.type) {
      case "offer":
        log("ok", `← offer`, { sdpBytes: env.data.sdp?.length ?? 0 });
        try {
          await pc.setRemoteDescription(env.data);
          const answer = await pc.createAnswer();
          // T30 munging applies on the answer now that we are the
          // answerer (T34). Same pure transform as before.
          const mungedSdp = prioritizeCodec(answer.sdp ?? "", "VP9");
          await pc.setLocalDescription({ type: answer.type, sdp: mungedSdp });
          const reply: Envelope = { type: "answer", from: "client", data: { type: answer.type, sdp: mungedSdp } };
          ws.send(JSON.stringify(reply));
          log("ok", `→ answer`, { sdpBytes: mungedSdp.length });
        } catch (err) {
          log("err", "answer pipeline failed", String(err));
          teardown("answer failed");
        }
        break;
      case "answer":
        // We are the answerer — receiving an answer is unexpected. Log
        // and ignore. Renegotiation would be a fresh offer instead.
        log("warn", `← unexpected answer from ${env.from}`);
        break;
      case "ice":
        if (env.data === null) {
          log("info", "← ice (end of candidates)");
          return;
        }
        try {
          await pc.addIceCandidate(env.data);
          log("info", "← ice", env.data.candidate ?? "");
        } catch (err) {
          log("warn", "addIceCandidate failed", String(err));
        }
        break;
      case "bye":
        log("info", `← bye from ${env.from}`);
        teardown("peer said bye");
        break;
    }
  });

  ws.addEventListener("close", (ev) => {
    log(ev.wasClean ? "info" : "warn", `ws close`, { code: ev.code, reason: ev.reason || "(none)" });
    teardown("ws closed");
  });

  ws.addEventListener("error", () => {
    log("err", "ws error");
  });
}

// ---------- wire up ----------

els.connect.addEventListener("click", () => {
  if (active) {
    teardown("user clicked");
    return;
  }
  const sessionId = els.sessionId.value.trim() || "dev";
  els.connect.textContent = "Disconnect";
  void connect(sessionId);
});

window.addEventListener("beforeunload", () => teardown("page unload"));

log("info", "client loaded — click Connect to start");

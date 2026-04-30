// v0 browser client for cloud-browser-webrtc.
//
// Scope (T14):
//   - Connect to the signaling server from T13 at ws://localhost:8080/ws/{session}.
//   - Create an RTCPeerConnection with default STUN config.
//   - Open a data channel "input" for future input forwarding.
//   - Send a stub SDP offer and wait for an answer.
//   - Attach any incoming media track to the <video> element.
//   - Surface signaling/ICE/connection state to the debug panel.
//
// Out of scope: there is no real remote source yet (capture is Phase 1).
// You will see the offer go out and the connection sit in
// have-local-offer / new-ICE forever unless something on the other end
// actually answers. That is the expected stub behavior — the goal is to
// validate signaling round-trip and RTCPeerConnection lifecycle wiring.

import { InputChannel } from "./src/input.js";
import { fetchTurnConfig } from "./src/turn.js";

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
  dc: RTCDataChannel;
  input: InputChannel;
  detachInput: (() => void) | null;
}

let active: Session | null = null;

function teardown(reason: string): void {
  if (!active) return;
  log("info", `tearing down: ${reason}`);
  try { active.detachInput?.(); } catch { /* ignore */ }
  try { active.dc.close(); } catch { /* ignore */ }
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

  // T14/T20: data channel "input" carries the v1 input protocol
  // documented in docs/protocols/input-channel.md.
  const dc = pc.createDataChannel("input", { ordered: true });
  const input = new InputChannel(dc, {
    onCoalesce: (n) => log("info", `coalesced ${n} mouse_move`),
    onError: (err) => log("err", "input send failed", String(err)),
  });
  let detach: (() => void) | null = null;

  dc.onopen = () => {
    els.dc.textContent = "open";
    log("ok", "input data-channel open");
    // Attach DOM listeners only once the channel is actually open;
    // before that we'd just be queueing and dropping.
    detach = input.attach(els.video, {
      toContentCoords: (cx, cy, rect) => {
        // Map page coords into the source video's intrinsic pixel
        // space, accounting for object-fit: contain. The remote
        // expects coords in the source coordinate system.
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
      },
    });
    if (active) active.detachInput = detach;
  };
  dc.onclose   = () => {
    els.dc.textContent = "closed";
    log("info", "input data-channel closed");
    detach?.();
    detach = null;
  };
  dc.onerror   = (e) => { els.dc.textContent = "error";  log("err",  "input data-channel error", String((e as RTCErrorEvent).error?.message ?? e)); };
  dc.onmessage = (e) => log("info", "← input.message", e.data);
  els.dc.textContent = dc.readyState;

  active = { ws, pc, dc, input, detachInput: null };

  ws.addEventListener("open", async () => {
    log("ok", "ws open");
    setStatus("connecting", "negotiating");

    // We must add a recv-only transceiver for video so the SDP offer
    // contains an m= section the server can answer with a real track.
    pc.addTransceiver("video", { direction: "recvonly" });
    pc.addTransceiver("audio", { direction: "recvonly" });

    try {
      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);
      const env: Envelope = { type: "offer", from: "client", data: { type: offer.type, sdp: offer.sdp ?? "" } };
      ws.send(JSON.stringify(env));
      log("ok", "→ offer", { sdpBytes: offer.sdp?.length ?? 0 });
    } catch (err) {
      log("err", "createOffer failed", String(err));
      teardown("createOffer failed");
    }
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
      case "answer":
        log("ok", `← answer`, { sdpBytes: env.data.sdp?.length ?? 0 });
        try {
          await pc.setRemoteDescription(env.data);
        } catch (err) {
          log("err", "setRemoteDescription failed", String(err));
        }
        break;
      case "offer":
        // Browser-initiated renegotiation — not used in v0, log and ignore.
        log("warn", `← unexpected offer from ${env.from}`);
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

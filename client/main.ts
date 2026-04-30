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
//   - Surface signaling/ICE/connection/reconnect state to the debug panel.
//   - Auto-reconnect signaling websocket via T37's ReconnectingWebSocket;
//     on reconnect, rebuild the peer connection and wait for a fresh
//     offer from the streamer. On `iceConnectionState=failed`, send a
//     `request_renegotiate` envelope to ask the streamer for a fresh
//     offer with iceRestart=true (see docs/protocols/reconnect.md).

import { InputChannel } from "./src/input.js";
import { fetchTurnConfig } from "./src/turn.js";
import { prioritizeCodec } from "./src/sdp.js";
import { ReconnectingWebSocket, ReconnectState, requestIceRecovery } from "./src/reconnect.js";
import { StatsSampler, StatsSample, STATS_PROTOCOL_VERSION, formatSummary } from "./src/stats.js";

const DEFAULT_SIGNALING = "ws://localhost:8080/ws";

type Envelope =
  | { type: "offer";  from: "client" | "browser"; data: RTCSessionDescriptionInit }
  | { type: "answer"; from: "client" | "browser"; data: RTCSessionDescriptionInit }
  | { type: "ice";    from: "client" | "browser"; data: RTCIceCandidateInit | null }
  | { type: "bye";    from: "client" | "browser"; data?: undefined }
  | { type: "request_renegotiate"; from: "client" | "browser"; data?: null };

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
  rws: ReconnectingWebSocket;
  pc: RTCPeerConnection;
  iceConfig: RTCConfiguration;
  /** Set when the streamer has opened the "input" data channel. */
  dc: RTCDataChannel | null;
  input: InputChannel | null;
  detachInput: (() => void) | null;
  /** Set when the streamer has opened the "stats" data channel. */
  statsDc: RTCDataChannel | null;
  /** Stats sampler — created with the pc, lives until pc rebuild. */
  stats: StatsSampler | null;
  /** Subscriber detach function for the stats sampler. */
  detachStats: (() => void) | null;
  /** Has at least one rws "open" fired? Used to distinguish first vs reconnect. */
  hasOpenedOnce: boolean;
}

let active: Session | null = null;

function teardown(reason: string): void {
  if (!active) return;
  log("info", `tearing down: ${reason}`);
  try { active.detachInput?.(); } catch { /* ignore */ }
  try { active.detachStats?.(); } catch { /* ignore */ }
  try { active.stats?.stop(); } catch { /* ignore */ }
  try { active.dc?.close(); } catch { /* ignore */ }
  try { active.statsDc?.close(); } catch { /* ignore */ }
  try { active.pc.close(); } catch { /* ignore */ }
  if (active.rws.isConnected()) {
    try {
      active.rws.send(JSON.stringify({ type: "bye", from: "client" } satisfies Envelope));
    } catch { /* ignore */ }
  }
  try { active.rws.close(); } catch { /* ignore */ }
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

function wireDataChannel(dc: RTCDataChannel): void {
  if (!active) return;
  log("ok", `← data channel "${dc.label}" (state=${dc.readyState})`);
  if (dc.label === "input") return wireInputChannel(dc);
  if (dc.label === "stats") return wireStatsChannel(dc);
  log("warn", `ignoring unknown data channel label: ${dc.label}`);
}

function wireStatsChannel(dc: RTCDataChannel): void {
  if (!active) return;
  active.statsDc = dc;
  // Only start emitting frames over the channel once it's open.
  // Subscribers (debug panel) are wired in buildPeerConnection.
}

function wireInputChannel(dc: RTCDataChannel): void {
  if (!active) return;
  els.dc.textContent = dc.readyState;
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

/**
 * Build a fresh RTCPeerConnection wired up to the active session. Used
 * on the first connect AND on every signaling reconnect, so each fresh
 * signaling channel gets a fresh peer connection and the streamer can
 * cleanly re-offer.
 */
function buildPeerConnection(): RTCPeerConnection {
  if (!active) throw new Error("buildPeerConnection: no active session");
  const { iceConfig, rws } = active;
  const pc = new RTCPeerConnection(iceConfig);

  pc.onsignalingstatechange = () => { els.sig.textContent = pc.signalingState; log("info", `signalingState=${pc.signalingState}`); };
  pc.oniceconnectionstatechange = () => {
    els.ice.textContent = pc.iceConnectionState;
    const lvl: LogLevel = pc.iceConnectionState === "failed" ? "err" : "info";
    log(lvl, `iceConnectionState=${pc.iceConnectionState}`);
    if (pc.iceConnectionState === "failed") {
      // T37 ICE recovery — ask the streamer to redo the offer with iceRestart=true.
      const sent = requestIceRecovery((f) => rws.send(f), "client");
      log(sent ? "info" : "warn", sent ? "→ request_renegotiate" : "request_renegotiate dropped (ws not open)");
    }
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
    const env: Envelope = { type: "ice", from: "client", data: ev.candidate ? ev.candidate.toJSON() : null };
    rws.send(JSON.stringify(env));
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

  pc.ondatachannel = (ev) => wireDataChannel(ev.channel);

  // T42: per-second stats sampler. Subscribers update the debug panel
  // and (when the streamer offers a "stats" channel) emit over the
  // data channel for server-side scraping.
  const stats = new StatsSampler(pc, { intervalMs: 1000 });
  let prev: StatsSample | undefined;
  const detachStats = stats.on((s) => {
    log("info", `stats ${formatSummary(s, prev)}`);
    prev = s;
    if (active?.statsDc?.readyState === "open") {
      try {
        active.statsDc.send(JSON.stringify({ v: STATS_PROTOCOL_VERSION, t: s.t, sample: s }));
      } catch { /* ignore */ }
    }
  });
  if (active) {
    active.stats = stats;
    active.detachStats = detachStats;
  }
  // Start sampling once connection is up; pc.onconnectionstatechange handles it.
  const prevConnState = pc.onconnectionstatechange;
  pc.onconnectionstatechange = (ev) => {
    if (typeof prevConnState === "function") prevConnState.call(pc, ev);
    if (pc.connectionState === "connected") stats.start();
    else if (pc.connectionState === "closed" || pc.connectionState === "failed") stats.stop();
  };

  return pc;
}

function rebuildPeerConnection(reason: string): void {
  if (!active) return;
  log("info", `rebuilding peer connection: ${reason}`);
  try { active.detachInput?.(); } catch { /* ignore */ }
  try { active.detachStats?.(); } catch { /* ignore */ }
  try { active.stats?.stop(); } catch { /* ignore */ }
  active.detachInput = null;
  active.detachStats = null;
  active.stats = null;
  try { active.dc?.close(); } catch { /* ignore */ }
  try { active.statsDc?.close(); } catch { /* ignore */ }
  active.dc = null;
  active.statsDc = null;
  active.input = null;
  try { active.pc.close(); } catch { /* ignore */ }
  active.pc = buildPeerConnection();
  els.dc.textContent = "—";
}

async function connect(sessionId: string): Promise<void> {
  setStatus("connecting", "ws://");
  els.connect.disabled = true;

  const wsUrl = `${DEFAULT_SIGNALING}/${encodeURIComponent(sessionId)}`;
  log("info", `dialing signaling`, wsUrl);

  // Fetch ICE config once. We re-use it across reconnects; if it
  // rotates (TURN-REST in Phase 3), this is the call site that grows a
  // refresh.
  const iceConfig = await fetchTurnConfig(DEFAULT_SIGNALING);
  log("info", "ice config", iceConfig);

  const rws = new ReconnectingWebSocket(wsUrl);
  // Stash a placeholder pc so the active record is well-typed; replaced
  // synchronously by buildPeerConnection() once session is set.
  active = {
    rws, pc: null as unknown as RTCPeerConnection, iceConfig,
    dc: null, input: null, detachInput: null,
    statsDc: null, stats: null, detachStats: null,
    hasOpenedOnce: false,
  };
  active.pc = buildPeerConnection();
  els.dc.textContent = "—";

  rws.on("stateChange", (next, prev, info) => {
    log("info", `signaling ${prev}→${next}`, info.attempt > 0 ? { attempt: info.attempt, retryInMs: info.nextDelayMs } : undefined);
    if (next === "reconnecting") setStatus("connecting", `reconnecting (attempt ${info.attempt})`);
    else if (next === "failed")  setStatus("failed", `signaling failed`);
  });

  rws.on("open", () => {
    if (!active) return;
    if (active.hasOpenedOnce) {
      log("ok", "ws reopened — rebuilding peer connection");
      setStatus("connecting", "renegotiating");
      rebuildPeerConnection("ws reconnected");
    } else {
      active.hasOpenedOnce = true;
      log("ok", "ws open");
      setStatus("connecting", "waiting for offer");
    }
    // Hello frame so signaling learns our role.
    rws.send(JSON.stringify({ type: "ice", from: "client", data: null } satisfies Envelope));
  });

  rws.on("underlyingClose", (ev) => {
    log(ev.wasClean ? "info" : "warn", `ws closed`, { code: ev.code, reason: ev.reason || "(none)" });
    // We do NOT teardown the PC here. If signaling reconnects within
    // backoff window, ICE/DTLS may still be flowing and we just need a
    // fresh hello + offer cycle. rebuildPeerConnection runs on the
    // next "open".
  });

  rws.on("message", async (ev: MessageEvent) => {
    if (!active) return;
    const { pc } = active;
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
          // T30 munging applies on the answer (T34 role flip).
          const mungedSdp = prioritizeCodec(answer.sdp ?? "", "VP9");
          await pc.setLocalDescription({ type: answer.type, sdp: mungedSdp });
          const reply: Envelope = { type: "answer", from: "client", data: { type: answer.type, sdp: mungedSdp } };
          rws.send(JSON.stringify(reply));
          log("ok", `→ answer`, { sdpBytes: mungedSdp.length });
        } catch (err) {
          log("err", "answer pipeline failed", String(err));
          teardown("answer failed");
        }
        break;
      case "answer":
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
      case "request_renegotiate":
        // The streamer is asking US to renegotiate. We cannot
        // initiate (we are the answerer). Best we can do is ack via
        // log and rely on the streamer's own ICE restart machinery;
        // if it re-offers, our offer handler picks it up.
        log("info", `← request_renegotiate from ${env.from} (no-op for answerer; awaiting fresh offer)`);
        break;
    }
  });

  rws.connect();
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

// Hint to the bundler/eslint that ReconnectState is part of the public
// surface even though main.ts only uses it via the rws callbacks.
export type { ReconnectState };

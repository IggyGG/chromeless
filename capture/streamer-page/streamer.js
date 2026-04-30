// Streamer page — runs inside the headless Chromium container and pipes
// the captured display + audio into an RTCPeerConnection whose remote
// peer is the user's browser (client/main.ts).
//
// Wire protocol matches the signaling server (signaling/server.go):
//   { type: "offer"|"answer"|"ice"|"bye", from: "browser", data: ... }
// The streamer is role=browser; the user's browser is role=client.
//
// Cross-references:
//   - docs/capture/path-of-least-resistance.md  (T15 design)
//   - signaling/server.go                       (T13 protocol)
//   - capture/streamer-page/launch.md           (Chromium command line)
//   - capture/streamer-page/README.md           (how this page is launched)

(() => {
  "use strict";

  // -------- config ---------------------------------------------------

  // The signaling server URL and session ID arrive as query params so
  // the launch command (see launch.md) can wire them in without
  // rebuilding the page. Sensible defaults match the dev compose.
  const params = new URLSearchParams(location.search);
  const SIGNALING_URL = params.get("signal") || "ws://signaling:8080/ws";
  const SESSION_ID    = params.get("session") || "dev";
  const FRAMERATE     = Number(params.get("fps") || "30");

  const ICE_SERVERS = [{ urls: ["stun:stun.l.google.com:19302"] }];
  // Phase 3 swaps in our TURN-REST issued credentials (see PROJECT_BRIEF
  // Phase 3). Phase 1 stays on public STUN.

  const HEARTBEAT_MS = 10_000;

  // -------- log -------------------------------------------------------

  const logEl = document.getElementById("log");
  function log(level, msg, extra) {
    const ts = new Date().toISOString().slice(11, 23);
    const tail = extra === undefined
      ? ""
      : "  " + (typeof extra === "string" ? extra : safeStringify(extra));
    const line = document.createElement("div");
    line.className = level;
    line.textContent = `${ts} ${level.padEnd(4).toUpperCase()} ${msg}${tail}`;
    logEl.appendChild(line);
    logEl.scrollTop = logEl.scrollHeight;
    // Mirror to stderr/stdout so supervisord's log capture sees it.
    const c = level === "err" ? console.error
            : level === "warn" ? console.warn
            : console.log;
    c(`[streamer ${level}]`, msg, extra ?? "");
  }
  function safeStringify(v) {
    try { return JSON.stringify(v); } catch { return String(v); }
  }

  // -------- session ---------------------------------------------------

  /** @typedef {{ ws: WebSocket, pc: RTCPeerConnection, stream: MediaStream | null }} Session */
  /** @type {Session | null} */
  let active = null;
  let heartbeatTimer = null;

  function teardown(reason) {
    if (!active) return;
    log("info", `tearing down: ${reason}`);
    if (heartbeatTimer) { clearInterval(heartbeatTimer); heartbeatTimer = null; }
    try {
      if (active.stream) {
        active.stream.getTracks().forEach((t) => t.stop());
      }
    } catch (e) { log("warn", "track stop failed", String(e)); }
    try { active.pc.close(); } catch { /* ignore */ }
    if (active.ws.readyState === WebSocket.OPEN) {
      try {
        active.ws.send(JSON.stringify({ type: "bye", from: "browser" }));
      } catch { /* ignore */ }
    }
    try { active.ws.close(); } catch { /* ignore */ }
    active = null;
    // Phase 1 lifecycle: when the WS is gone, we are done. Supervisord
    // will restart Chromium for the next session (per T15 design).
    setTimeout(() => location.replace("about:blank"), 200);
  }

  async function start() {
    log("info", "streamer boot", { signaling: SIGNALING_URL, session: SESSION_ID, fps: FRAMERATE });

    // 1. Capture the display + audio.
    /** @type {MediaStream} */
    let stream;
    try {
      // Auto-grant flags ensure no picker; see launch.md.
      stream = await navigator.mediaDevices.getDisplayMedia({
        video: { frameRate: FRAMERATE },
        audio: true,
      });
      log("ok", "getDisplayMedia ok", {
        videoTracks: stream.getVideoTracks().length,
        audioTracks: stream.getAudioTracks().length,
      });
    } catch (err) {
      log("err", "getDisplayMedia failed — check Chromium auto-grant flags", String(err));
      throw err;
    }

    // 2. Build the peer connection and attach tracks.
    const pc = new RTCPeerConnection({ iceServers: ICE_SERVERS });
    stream.getTracks().forEach((t) => pc.addTrack(t, stream));

    pc.onsignalingstatechange   = () => log("info", `signalingState=${pc.signalingState}`);
    pc.oniceconnectionstatechange = () => {
      const lvl = pc.iceConnectionState === "failed" ? "err" : "info";
      log(lvl, `iceConnectionState=${pc.iceConnectionState}`);
    };
    pc.onicegatheringstatechange = () => log("info", `iceGatheringState=${pc.iceGatheringState}`);
    pc.onconnectionstatechange = () => {
      const lvl = pc.connectionState === "failed" ? "err" : "info";
      log(lvl, `connectionState=${pc.connectionState}`);
      if (pc.connectionState === "failed") teardown("pc failed");
    };

    // 3. Open the signaling WS.
    const wsUrl = `${SIGNALING_URL}/${encodeURIComponent(SESSION_ID)}`;
    log("info", "dialing signaling", wsUrl);
    const ws = new WebSocket(wsUrl);

    active = { ws, pc, stream };

    pc.onicecandidate = (ev) => {
      if (ws.readyState !== WebSocket.OPEN) return;
      const env = { type: "ice", from: "browser",
                    data: ev.candidate ? ev.candidate.toJSON() : null };
      ws.send(JSON.stringify(env));
      log("info", ev.candidate ? "→ ice" : "→ ice (end of candidates)",
          ev.candidate?.candidate ?? "");
    };

    ws.addEventListener("open", async () => {
      log("ok", "ws open");
      try {
        const offer = await pc.createOffer();
        await pc.setLocalDescription(offer);
        ws.send(JSON.stringify({
          type: "offer", from: "browser",
          data: { type: offer.type, sdp: offer.sdp ?? "" },
        }));
        log("ok", "→ offer", { sdpBytes: offer.sdp?.length ?? 0 });

        // Heartbeat: confirms the page is still alive in supervisord logs.
        heartbeatTimer = setInterval(() => {
          if (!active) return;
          const sender = active.pc.getSenders().find((s) => s.track?.kind === "video");
          log("info", "heartbeat", {
            pc: active.pc.connectionState,
            ice: active.pc.iceConnectionState,
            videoTrackEnabled: sender?.track?.enabled ?? null,
          });
        }, HEARTBEAT_MS);
      } catch (err) {
        log("err", "createOffer/setLocalDescription failed", String(err));
        teardown("offer failed");
      }
    });

    ws.addEventListener("message", async (ev) => {
      let env;
      try {
        env = JSON.parse(typeof ev.data === "string" ? ev.data : await ev.data.text());
      } catch (err) {
        log("warn", "non-JSON ws frame", String(err));
        return;
      }
      if (env.from === "browser") {
        log("warn", "echo from self?", env.type);
        return;
      }
      switch (env.type) {
        case "answer":
          log("ok", "← answer", { sdpBytes: env.data?.sdp?.length ?? 0 });
          try { await pc.setRemoteDescription(env.data); }
          catch (err) { log("err", "setRemoteDescription failed", String(err)); }
          break;
        case "ice":
          if (!env.data) { log("info", "← ice (end of candidates)"); return; }
          try { await pc.addIceCandidate(env.data); }
          catch (err) { log("warn", "addIceCandidate failed", String(err)); }
          break;
        case "offer":
          // Renegotiation from client — not used in v1.
          log("warn", "← unexpected offer from client");
          break;
        case "bye":
          log("info", "← bye from client");
          teardown("client said bye");
          break;
        default:
          log("warn", "← unknown type", env.type);
      }
    });

    ws.addEventListener("close", (ev) => {
      log(ev.wasClean ? "info" : "warn", "ws close",
          { code: ev.code, reason: ev.reason || "(none)" });
      teardown("ws closed");
    });

    ws.addEventListener("error", () => log("err", "ws error"));
  }

  window.addEventListener("beforeunload", () => teardown("page unload"));

  start().catch((err) => {
    log("err", "start failed", String(err));
    // Don't immediately exit on first failure — supervisord can decide.
  });
})();

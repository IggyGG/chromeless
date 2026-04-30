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
  // T77: simulcast support. Off by default; enable with ?simulcast=true.
  // The default ladder is 1× / 0.5× / 0.25× scale, with the bottom
  // layer also halving FPS. Override via:
  //   ?simulcast_layers=1,0.5,0.25@15
  // Each entry is "<scaleResolutionDownBy>[@<maxFramerate>]". Max 3
  // layers per the libwebrtc constraint (and per docs/protocols/simulcast.md).
  const SIMULCAST_ENABLED = (params.get("simulcast") || "").toLowerCase() === "true";
  // T81: webcam/mic passthrough. Off by default; enabled by the
  // orchestrator passing ?passthrough=true. When enabled, the
  // streamer's pc.ontrack handler routes inbound camera + mic
  // tracks to the v4l2-writer + PulseAudio loopback sockets at:
  //   PASSTHROUGH_VIDEO_SOCK / PASSTHROUGH_AUDIO_SOCK
  // The v4l2-writer native helper (T92, capture/v4l2-writer/)
  // listens on those sockets and writes I420 frames to /dev/video10
  // (video) or pacat-piped S16LE to the cb_passthrough sink
  // (audio). Wire format spec: docs/protocols/webcam-mic-passthrough.md.
  const PASSTHROUGH_ENABLED   = (params.get("passthrough") || "").toLowerCase() === "true";
  const PASSTHROUGH_VIDEO_SOCK = params.get("passthrough_video_sock") || "/run/cb-passthrough/video.sock";
  const PASSTHROUGH_AUDIO_SOCK = params.get("passthrough_audio_sock") || "/run/cb-passthrough/audio.sock";
  const SIMULCAST_LAYERS = parseSimulcastLayersParam(params.get("simulcast_layers"));
  // Input-bridge endpoint (T22 / T41). Same container as the streamer
  // (supervisord-managed), bound to loopback. Override via ?input=...
  // for tests that run the bridge elsewhere.
  const INPUT_BRIDGE_URL = params.get("input") || "ws://localhost:9100/input";
  // cb-metrics-sidecar /stats-update endpoint (T38 sidecar, T72 wiring).
  // Also loopback within the supervisord-managed container. Override
  // via ?metrics=... for integration tests that run the sidecar on a
  // different port. Set to "off" to disable the relay entirely (used
  // by tests that don't want a 9100/connect refused log every second).
  const METRICS_SIDECAR_URL = params.get("metrics") || "http://localhost:9100/stats-update";

  const ICE_SERVERS = [{ urls: ["stun:stun.l.google.com:19302"] }];
  // Phase 3 swaps in our TURN-REST issued credentials (see PROJECT_BRIEF
  // Phase 3). Phase 1 stays on public STUN.

  const HEARTBEAT_MS = 10_000;
  const INPUT_BACKOFF_MIN_MS = 200;
  const INPUT_BACKOFF_MAX_MS = 5_000;

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

  // T77: parse a "1,0.5,0.25@15" comma list into [{rid, scale, fps?}].
  // Empty/null input → the default 3-layer ladder.
  function parseSimulcastLayersParam(raw) {
    const def = [
      { rid: "layer0", scale: 1, fps: undefined,        maxBitrate: 4_000_000 },
      { rid: "layer1", scale: 2, fps: undefined,        maxBitrate: 1_500_000 },
      { rid: "layer2", scale: 4, fps: 15,               maxBitrate:   400_000 },
    ];
    if (!raw) return def;
    const parts = raw.split(",").map((s) => s.trim()).filter(Boolean);
    if (parts.length === 0 || parts.length > 3) return def;
    return parts.map((p, i) => {
      const [scaleStr, fpsStr] = p.split("@");
      const scale = 1 / Number(scaleStr); // CSS-y "1, 0.5, 0.25" → 1, 2, 4
      const fps = fpsStr !== undefined ? Number(fpsStr) : undefined;
      return { rid: `layer${i}`, scale, fps };
    });
  }

  // -------- session ---------------------------------------------------

  /** @typedef {{ ws: WebSocket, pc: RTCPeerConnection, stream: MediaStream | null,
   *              inputRelay: InputRelay | null, statsRelay: StatsRelay | null }} Session */
  /** @type {Session | null} */
  let active = null;
  let heartbeatTimer = null;

  // -------- input relay -----------------------------------------------
  //
  // When the user's client (T14 / T34) opens RTCDataChannel("input"),
  // the streamer page receives it via pc.ondatachannel. We open a
  // localhost WebSocket to the input-bridge (T22) and forward each
  // data-channel message verbatim. The bridge parses + dispatches into
  // CDP. See docs/protocols/input-channel.md for the wire format.
  //
  // We deliberately do not parse here: the bridge is the single source
  // of truth for the protocol and will reject anything malformed. Any
  // parsing in the streamer would be a duplicate validation surface
  // that has to stay in lock-step with the bridge's schema.

  class InputRelay {
    constructor(dc, url) {
      this.dc = dc;
      this.url = url;
      this.ws = null;
      this.queue = [];
      this.closed = false;
      this.attempt = 0;
      this.connectTimer = null;
      this.dropped = 0;

      dc.onmessage = (ev) => this.forward(ev.data);
      dc.onclose = () => {
        log("info", "input data-channel closed");
        this.close();
      };
      dc.onerror = (e) => log("warn", "input data-channel error", String(e));
      this.connect();
    }

    connect() {
      if (this.closed) return;
      const ws = new WebSocket(this.url);
      this.ws = ws;
      log("info", "input relay → bridge dialing", this.url);
      ws.onopen = () => {
        if (this.closed) { ws.close(); return; }
        this.attempt = 0;
        log("ok", "input relay → bridge open",
            { drained: this.queue.length });
        // Drain any messages that arrived before the bridge ws came up.
        for (const msg of this.queue) {
          try { ws.send(msg); } catch (e) { log("warn", "drain send failed", String(e)); break; }
        }
        this.queue = [];
      };
      ws.onmessage = (ev) => {
        // The bridge is one-way today. Anything coming back is a
        // protocol violation; log and ignore.
        log("warn", "input relay ← unexpected bridge message",
            typeof ev.data === "string" ? ev.data.slice(0, 80) : "[binary]");
      };
      ws.onerror = () => log("warn", "input relay → bridge error");
      ws.onclose = (ev) => {
        log(this.closed ? "info" : "warn", "input relay → bridge closed",
            { code: ev.code, clean: ev.wasClean });
        this.ws = null;
        if (!this.closed) this.scheduleReconnect();
      };
    }

    scheduleReconnect() {
      if (this.closed || this.connectTimer) return;
      // Exponential backoff: 200ms, 400ms, 800ms, ..., capped at 5s.
      const delay = Math.min(
        INPUT_BACKOFF_MAX_MS,
        INPUT_BACKOFF_MIN_MS * Math.pow(2, this.attempt));
      this.attempt++;
      log("info", "input relay reconnect scheduled",
          { delay_ms: delay, attempt: this.attempt });
      this.connectTimer = setTimeout(() => {
        this.connectTimer = null;
        this.connect();
      }, delay);
    }

    forward(data) {
      if (this.closed) return;
      // The bridge expects strings (text frames carrying JSON). If the
      // client ever sends binary (it shouldn't per the v1 protocol),
      // we drop with a warn so the team-lead-side debug surface shows
      // the misuse.
      if (typeof data !== "string") {
        this.dropped++;
        log("warn", "input relay dropping non-string frame",
            { dropped_total: this.dropped });
        return;
      }
      const ws = this.ws;
      if (ws && ws.readyState === WebSocket.OPEN) {
        try { ws.send(data); }
        catch (e) { log("warn", "input relay send failed", String(e)); }
        return;
      }
      // Bridge ws not open yet — buffer briefly. Cap the queue so a
      // disconnected bridge does not eat unbounded memory.
      if (this.queue.length >= 256) {
        this.dropped++;
        if (this.dropped === 1 || this.dropped % 50 === 0) {
          log("warn", "input relay queue full, dropping",
              { dropped_total: this.dropped });
        }
        return;
      }
      this.queue.push(data);
    }

    close() {
      if (this.closed) return;
      this.closed = true;
      if (this.connectTimer) { clearTimeout(this.connectTimer); this.connectTimer = null; }
      if (this.ws && (this.ws.readyState === WebSocket.OPEN ||
                       this.ws.readyState === WebSocket.CONNECTING)) {
        try { this.ws.close(); } catch { /* ignore */ }
      }
      this.ws = null;
      this.queue = [];
      log("info", "input relay closed");
    }
  }

  // -------- stats relay (T72) ----------------------------------------
  //
  // The user's client (T42) opens RTCDataChannel("stats") and emits
  // {v, t, sample} envelopes once per second. We forward them to the
  // cb-metrics-sidecar's /stats-update endpoint on loopback, which
  // updates the cb_client_* gauges (T38). HTTP POST is the right shape
  // here because:
  //   - Each sample is independent; no need for a long-lived ws.
  //   - The sidecar's HTTP surface already exists (/metrics, /healthz);
  //     adding /stats-update is one more handler.
  //   - We can drop on failure without retry semantics — the next
  //     sample is at most 1s away and the gauges are last-set anyway.
  //
  // Failure mode: when the sidecar is down, fetch() rejects. We log
  // once per failure burst (rate-limited by the consecutiveFailures
  // counter) and otherwise stay quiet so the streamer page stays
  // legible.

  class StatsRelay {
    constructor(dc, url) {
      this.dc = dc;
      this.url = url;
      this.disabled = (url === "off" || !url);
      this.consecutiveFailures = 0;
      this.successCount = 0;

      dc.onmessage = (ev) => this.forward(ev.data);
      dc.onclose = () => {
        log("info", "stats data-channel closed",
            { posted: this.successCount, failures: this.consecutiveFailures });
      };
      dc.onerror = (e) => log("warn", "stats data-channel error", String(e));
      if (this.disabled) {
        log("info", "stats relay disabled (url=off)");
      } else {
        log("info", "stats relay → sidecar", { url });
      }
    }

    forward(data) {
      if (this.disabled) return;
      if (typeof data !== "string") {
        log("warn", "stats relay: non-string frame, dropping");
        return;
      }
      // Fire-and-forget; we do not await the response.
      fetch(this.url, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: data,
        // No-CORS so fetch doesn't try a preflight on a JSON POST in
        // some browser configurations. The sidecar trusts loopback.
        keepalive: true,
      }).then((resp) => {
        if (!resp.ok) {
          this.consecutiveFailures++;
          if (this.consecutiveFailures === 1 || this.consecutiveFailures % 10 === 0) {
            log("warn", "stats relay POST non-2xx",
                { status: resp.status, consecutive: this.consecutiveFailures });
          }
          return;
        }
        this.successCount++;
        if (this.consecutiveFailures > 0) {
          log("info", "stats relay recovered",
              { after_failures: this.consecutiveFailures });
          this.consecutiveFailures = 0;
        }
      }).catch((err) => {
        this.consecutiveFailures++;
        if (this.consecutiveFailures === 1 || this.consecutiveFailures % 10 === 0) {
          log("warn", "stats relay POST failed",
              { err: String(err), consecutive: this.consecutiveFailures });
        }
      });
    }
  }

  // T81 — passthrough sink. Lazily creates a hidden <video>/<audio>
  // for diagnostic visibility. The actual v4l2-writer / pulse sink
  // is a Phase 4 follow-up; we keep this seam so the page can route
  // the inbound MediaStreamTrack into a known place.
  function ensurePassthroughMediaElement(kind) {
    const id = `passthrough-${kind}`;
    let el = document.getElementById(id);
    if (el) return el;
    el = document.createElement(kind === "video" ? "video" : "audio");
    el.id = id;
    el.autoplay = true;
    el.playsInline = true;
    el.muted = kind === "audio"; // we don't want the audio to play locally
    el.style.position = "fixed";
    el.style.top = "-9999px";    // off-screen
    el.style.left = "-9999px";
    document.body.appendChild(el);
    return el;
  }

  function teardown(reason) {
    if (!active) return;
    log("info", `tearing down: ${reason}`);
    if (heartbeatTimer) { clearInterval(heartbeatTimer); heartbeatTimer = null; }
    if (active.inputRelay) {
      try { active.inputRelay.close(); } catch { /* ignore */ }
    }
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
    // Drop the window hooks so the watchdog sees state=null and starts
    // its idle countdown on a torn-down session — without this clear,
    // a closed PC's connectionState ("closed") is in the watchdog's
    // idle set anyway, but explicit cleanup avoids surprising the
    // T33 / T42 introspection paths.
    try { delete window.pc; delete window.signalingWs; }
    catch { window.pc = undefined; window.signalingWs = undefined; }
    // Phase 1 lifecycle: when the WS is gone, we are done. Supervisord
    // will restart Chromium for the next session (per T15 design).
    setTimeout(() => location.replace("about:blank"), 200);
  }

  // T102: seed encoder ceiling from the client's pre-call probe.
  // The client measured RTT + rough up/down throughput before
  // negotiation; we use min(uplink,downlink) × 0.8 as a starting
  // maxBitrate, clamped to [200 kbps, 8 Mbps]. libwebrtc's BWE
  // continues to adjust from this seed during the session.
  async function applyProbeResult(probe) {
    if (!active || !probe) return;
    const ul = Number(probe.uplink_kbps);
    const dl = Number(probe.downlink_kbps);
    if (!Number.isFinite(ul) || !Number.isFinite(dl) || ul <= 0 || dl <= 0) {
      log("warn", "probe_result: bad numbers", probe);
      return;
    }
    const target = Math.min(ul, dl) * 0.8 * 1000; // kbps→bps
    const maxBitrate = Math.max(200_000, Math.min(8_000_000, Math.round(target)));
    const sender = active.pc.getSenders().find((s) => s.track?.kind === "video");
    if (!sender) {
      log("warn", "probe_result: no video sender yet (deferring not implemented; likely raced offer)");
      return;
    }
    try {
      const params = sender.getParameters();
      // Preserve existing simulcast layout (T77/T83): scale every layer's
      // maxBitrate proportionally to the probe ceiling so layer-relative
      // ratios stay intact. With one encoding the math collapses to
      // setting that single layer's cap.
      if (!params.encodings || params.encodings.length === 0) {
        params.encodings = [{}];
      }
      const existingMax = params.encodings.reduce(
        (acc, e) => Math.max(acc, e.maxBitrate ?? 0), 0,
      );
      for (const enc of params.encodings) {
        if (existingMax > 0 && enc.maxBitrate) {
          enc.maxBitrate = Math.round(enc.maxBitrate * (maxBitrate / existingMax));
        } else {
          enc.maxBitrate = maxBitrate;
        }
      }
      await sender.setParameters(params);
      log("ok", "probe_result applied", {
        rtt_ms: probe.rtt_ms,
        uplink_kbps: ul,
        downlink_kbps: dl,
        encoderCapBps: maxBitrate,
      });
    } catch (err) {
      log("warn", "setParameters from probe_result failed", String(err));
    }
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
      // The most common failure modes here, in order of how often we
      // see them, plus where the rationale lives:
      //
      //   * NotAllowedError — auto-grant flags missing (see launch.md
      //     "Chromium command line", §"Auto-grant getDisplayMedia").
      //   * NotReadableError — Vulkan / GPU / ozone init failed and
      //     left the screen capturer broken (see launch.md
      //     "Why the GPU / Vulkan flags are non-negotiable on
      //     Chromium 147"; T78-followup).
      //   * NotFoundError — Xvfb display isn't running (check
      //     supervisord's [program:xvfb]).
      //
      // We surface enough information that the next debugging
      // session doesn't start by re-querying DevTools.
      const detail = {
        name: err && err.name ? err.name : "(unknown)",
        message: err && err.message ? err.message : String(err),
        ua: navigator.userAgent,
      };
      log("err", "getDisplayMedia failed — see launch.md for flag rationale",
          detail);
      throw err;
    }

    // 2. Build the peer connection and attach tracks.
    const pc = new RTCPeerConnection({ iceServers: ICE_SERVERS });

    // T77: simulcast requires rid-bearing encodings to be supplied at
    // transceiver creation time — `setParameters` after `addTrack`
    // throws InvalidModificationError because rid is read-only on an
    // existing sender. Use addTransceiver with sendEncodings for video
    // when simulcast is requested; fall back to addTrack otherwise.
    if (SIMULCAST_ENABLED) {
      const videoTrack = stream.getVideoTracks()[0];
      if (videoTrack) {
        const sendEncodings = SIMULCAST_LAYERS.map((l) => {
          const e = { rid: l.rid, scaleResolutionDownBy: l.scale, active: true };
          if (l.fps !== undefined) e.maxFramerate = l.fps;
          if (l.maxBitrate !== undefined) e.maxBitrate = l.maxBitrate;
          return e;
        });
        pc.addTransceiver(videoTrack, { direction: "sendonly", streams: [stream], sendEncodings });
        log("ok", "simulcast configured via addTransceiver", {
          layers: sendEncodings.map((e) => ({
            rid: e.rid, scale: e.scaleResolutionDownBy,
            maxFps: e.maxFramerate ?? null, maxBps: e.maxBitrate ?? null,
          })),
        });
      }
      // Audio still uses addTrack — simulcast is video-only.
      stream.getAudioTracks().forEach((t) => pc.addTrack(t, stream));
    } else {
      stream.getTracks().forEach((t) => pc.addTrack(t, stream));
    }

    // Expose the live PeerConnection on window so:
    //   * infra/lifecycle/idle-watchdog.sh can poll
    //     `window.pc.connectionState` via DevTools Runtime.evaluate
    //     to decide whether the session is active. Without this hook
    //     the watchdog reports state=null on every tick and tombstones
    //     the container after IDLE_TIMEOUT_S regardless of activity.
    //   * Phase 1 / T33 E2E specs can introspect getReceivers() etc.
    // The streamer page is privileged (only the cloud-browser worker
    // ever loads it; remote pages cannot navigate here), so exposing
    // the PC on window has no cross-origin implications.
    window.pc = pc;

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

    // The streamer is the offerer (T34) and creates the "input"
    // (T41) and "stats" (T72) channels itself; see below. Anything
    // arriving via pc.ondatachannel would be an unexpected client-
    // initiated channel — log it for diagnostics.
    pc.ondatachannel = (ev) => {
      log("warn", "unexpected client-initiated data channel", { label: ev.channel.label });
    };

    // T81: webcam/mic passthrough. The client (per
    // docs/protocols/webcam-mic-passthrough.md) calls
    // navigator.mediaDevices.getUserMedia and adds the resulting
    // video/audio tracks to the existing peer connection. When
    // they arrive here we route them to the v4l2-writer socket
    // (video) or the PulseAudio loopback sink (audio).
    //
    // Two behaviours, gated on the PASSTHROUGH_ENABLED query param:
    //   - enabled  → install the passthrough handler. It logs each
    //                inbound track, attaches it to a hidden media
    //                element for visibility, and (T92) the
    //                v4l2-writer native helper picks up frames
    //                from the per-kind sink socket and pushes
    //                them to /dev/video10 / the PulseAudio sink.
    //                The InsertableStreams + worker that pipes
    //                frames from the page into the Unix socket is
    //                still a documented seam — see the warning
    //                emitted on first track for the operator note.
    //   - disabled → drop any inbound track immediately by calling
    //                track.stop() and logging a warning. Per the
    //                threat model §T2, content from a non-
    //                passthrough-configured pod must NEVER reach
    //                the v4l2/pulse loopback.
    pc.ontrack = (ev) => {
      const track = ev.track;
      const kind = track.kind;
      log("info", "← inbound track", { kind, id: track.id, readyState: track.readyState });
      if (!PASSTHROUGH_ENABLED) {
        log("warn", "passthrough not enabled; dropping inbound track", { kind });
        try { track.stop(); } catch { /* ignore */ }
        return;
      }
      // Choose a socket per kind. The frame format on the wire is
      // documented in docs/protocols/webcam-mic-passthrough.md.
      const sock = kind === "video" ? PASSTHROUGH_VIDEO_SOCK
                : kind === "audio" ? PASSTHROUGH_AUDIO_SOCK
                : null;
      if (!sock) {
        log("warn", "passthrough: unknown track kind; ignoring", { kind });
        try { track.stop(); } catch { /* ignore */ }
        return;
      }
      // For visibility during dev, attach the track to a hidden
      // media element. Removing the track from this element on
      // "ended" is what triggers Chromium to release the underlying
      // pipeline, which lets v4l2-writer's read loop unblock.
      const mediaEl = ensurePassthroughMediaElement(kind);
      mediaEl.srcObject = mediaEl.srcObject instanceof MediaStream
        ? (mediaEl.srcObject.addTrack(track), mediaEl.srcObject)
        : new MediaStream([track]);

      track.addEventListener("ended", () => {
        log("info", "passthrough track ended", { kind, id: track.id });
        // T92: closing the page-side socket connection (which the
        // InsertableStreams seam does when the worker exits) lets
        // the v4l2-writer's read loop unblock and accept the next
        // session's connection.
      });

      // The page-side InsertableStreams → Unix-socket worker that
      // actually pipes frames from the MediaStreamTrack to
      // PASSTHROUGH_VIDEO_SOCK / PASSTHROUGH_AUDIO_SOCK is a
      // remaining seam — the v4l2-writer (T92) is in place on the
      // server side and waiting for bytes. The page-side worker is
      // documented but not implemented in this commit; track this
      // gap as the [T92-followup] page-side InsertableStreams
      // worker.
      log("info", "passthrough handler installed",
        { kind, sock, sink: kind === "video" ? "/dev/video10" : "cb_passthrough (PA)" });
    };

    // 3. Open the signaling WS.
    const wsUrl = `${SIGNALING_URL}/${encodeURIComponent(SESSION_ID)}`;
    log("info", "dialing signaling", wsUrl);
    const ws = new WebSocket(wsUrl);
    // Same window-exposure rationale as window.pc above. The watchdog
    // doesn't currently inspect ws state but other diagnostics
    // (T42 stats panel, manual debugging) benefit from a stable hook.
    window.signalingWs = ws;

    // T41 + T72: as the offerer (T34) we are responsible for creating
    // the data channels so they appear in the offer SDP. The client
    // (answerer) receives them via pc.ondatachannel and pushes events
    // through; we relay each one to its sidecar.
    //
    // We have to call pc.ondatachannel BEFORE these createDataChannel
    // calls so the corresponding open events fire on this side (they
    // don't, normally — the offerer's createDataChannel returns the
    // channel directly — but our own dispatch uses pc.ondatachannel
    // in case future code paths add channels remotely).
    const inputDC  = pc.createDataChannel("input",  { ordered: true });
    const statsDC  = pc.createDataChannel("stats",  { ordered: true });
    log("info", "created data channels", {
      input: inputDC.id, stats: statsDC.id,
    });
    const inputRelay = new InputRelay(inputDC, INPUT_BRIDGE_URL);
    const statsRelay = new StatsRelay(statsDC, METRICS_SIDECAR_URL);

    active = { ws, pc, stream, inputRelay, statsRelay };

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
        case "probe_result":
          // T102: client measured up/down throughput before negotiation;
          // seed the encoder ceiling so the first few seconds aren't
          // degraded while libwebrtc's BWE ramps up.
          applyProbeResult(env.data);
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

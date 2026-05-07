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
  const PASSTHROUGH_VIDEO_SOCK = params.get("passthrough_video_sock") || "/run/chromeless-passthrough/video.sock";
  const PASSTHROUGH_AUDIO_SOCK = params.get("passthrough_audio_sock") || "/run/chromeless-passthrough/audio.sock";
  const SIMULCAST_LAYERS = parseSimulcastLayersParam(params.get("simulcast_layers"));
  // Input-bridge endpoint (T22 / T41). Same container as the streamer
  // (supervisord-managed), bound to loopback. Override via ?input=...
  // for tests that run the bridge elsewhere.
  const INPUT_BRIDGE_URL = params.get("input") || "ws://localhost:9100/input";
  // chromeless-metrics-sidecar /stats-update endpoint (T38 sidecar, T72 wiring).
  // Also loopback within the supervisord-managed container. Override
  // via ?metrics=... for integration tests that run the sidecar on a
  // different port. Set to "off" to disable the relay entirely (used
  // by tests that don't want a 9100/connect refused log every second).
  const METRICS_SIDECAR_URL = params.get("metrics") || "http://localhost:9100/stats-update";
  // Wave 2 A4 — chromeless-metrics-sidecar /webrtc-event endpoint
  // (sibling of /stats-update). Receives lifecycle beacons:
  // session_created, session_closed, ice.connected, ice.failed,
  // dc.opened, plus the per-channel counters
  // (cursor.coalesced, clipboard.unsupported_mime / stale_seq,
  // file_upload.orphan_timeout / completed). Schema:
  // /workspace/chemistry/elements/tools/chromeless/.triform/observability.yaml.
  // Override via ?webrtc_metrics=... ; "off" disables emit (tests use
  // this so the page doesn't spam connect-refused warnings when no
  // sidecar is bound on :9100).
  const WEBRTC_METRICS_URL = params.get("webrtc_metrics") || "http://localhost:9100/webrtc-event";

  // Wave 1 (chromeless-datachannel-contract.md §§4-6): three new
  // DataChannels — `cursor`, `clipboard`, `file-upload` — wired
  // against CDP probes injected into every user-content document.
  //
  // The streamer page itself runs inside the same Chromium that hosts
  // the user content, so we can dial DevTools at ws://localhost:9222
  // (the `--unsafely-treat-insecure-origin-as-secure=...:9000` flag
  // grants this page enough power to open the DevTools websocket).
  // Override via ?cdp=ws://... for tests that mock CDP elsewhere.
  // Set to "off" to disable Wave 1 channel wiring entirely (so old
  // tests that don't expect the new channels keep passing).
  const CDP_BASE_URL = params.get("cdp") || "http://localhost:9222";
  const WAVE1_DISABLED = (params.get("wave1") || "").toLowerCase() === "off";

  const ICE_SERVERS = [{ urls: ["stun:stun.l.google.com:19302"] }];
  // Phase 3 swaps in our TURN-REST issued credentials (see PROJECT_BRIEF
  // Phase 3). Phase 1 stays on public STUN.

  const HEARTBEAT_MS = 10_000;
  const INPUT_BACKOFF_MIN_MS = 200;
  const INPUT_BACKOFF_MAX_MS = 5_000;
  // Wave 1 §6 file-upload orphan timeout — receiver discards the
  // partial buffer for an upload_id if no new chunk arrives within
  // 60 s and `eof` hasn't been reached.
  const FILE_UPLOAD_ORPHAN_MS = 60_000;
  // Wave 1 §6 file-upload integrity rule (1): allow up to 4 chunks
  // ahead of the expected `seq` to absorb in-flight reordering. Beyond
  // that we drop the upload and warn.
  const FILE_UPLOAD_REORDER_AHEAD = 4;
  // Wave 1 §6 sanitisation: truncate the file name to 255 bytes after
  // stripping path separators / control chars.
  const FILE_UPLOAD_NAME_MAX_BYTES = 255;

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
   *              inputRelay: InputRelay | null, statsRelay: StatsRelay | null,
   *              cdp: CDPClient | null,
   *              cursorEmitter: CursorEmitter | null,
   *              clipboardChannel: ClipboardChannel | null,
   *              fileUploadReceiver: FileUploadReceiver | null,
   *              emitSessionClosed: (reason: string) => void }} Session */
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
  // chromeless-metrics-sidecar's /stats-update endpoint on loopback, which
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

  // -------- Wave 2 A4 — WebRTC observability emitter ---------------
  //
  // Fire-and-forget POSTs to the chromeless-metrics-sidecar
  // /webrtc-event endpoint, one per lifecycle beacon. Mirrors the
  // StatsRelay shape (rate-limited error log on burst, single line on
  // recovery) but doesn't relay from a DataChannel — every emit is
  // direct from the streamer-page code path that observed the event.
  //
  // The sidecar translates each event into a Prometheus counter or
  // histogram increment. Names match
  // chemistry/elements/tools/chromeless/.triform/observability.yaml.
  //
  // Disabling: pass ?webrtc_metrics=off to skip emit entirely. We never
  // throw or block on a sidecar outage — observability is best-effort.

  class MetricsEmitter {
    constructor(url) {
      this.url = url;
      this.disabled = (url === "off" || !url);
      this.consecutiveFailures = 0;
      this.successCount = 0;
      if (this.disabled) {
        log("info", "webrtc metrics emit disabled (url=off)");
      } else {
        log("info", "webrtc metrics emit → sidecar", { url });
      }
    }

    // emit posts a single {event, attrs} envelope. Fire-and-forget;
    // we never await, never block streamer logic on the network.
    emit(event, attrs) {
      if (this.disabled) return;
      const body = JSON.stringify({ event, attrs: attrs ?? {} });
      fetch(this.url, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body,
        keepalive: true,
      }).then((resp) => {
        if (!resp.ok) {
          this.consecutiveFailures++;
          if (this.consecutiveFailures === 1 || this.consecutiveFailures % 10 === 0) {
            log("warn", "webrtc metrics POST non-2xx",
                { event, status: resp.status, consecutive: this.consecutiveFailures });
          }
          return;
        }
        this.successCount++;
        if (this.consecutiveFailures > 0) {
          log("info", "webrtc metrics emit recovered",
              { after_failures: this.consecutiveFailures });
          this.consecutiveFailures = 0;
        }
      }).catch((err) => {
        this.consecutiveFailures++;
        if (this.consecutiveFailures === 1 || this.consecutiveFailures % 10 === 0) {
          // console.warn (not log()) — we don't want to flood the
          // page-side log overlay every second when :9100 is down.
          // The first failure and every tenth thereafter still surface.
          console.warn("[streamer warn] webrtc metrics POST failed",
              { event, err: String(err), consecutive: this.consecutiveFailures });
        }
      });
    }
  }

  // Module-scope singleton so the emitter classes (CursorEmitter,
  // ClipboardChannel, FileUploadReceiver) and the start() session
  // bootstrap can all reach it without threading it through every
  // constructor. Created on first start() call below.
  /** @type {MetricsEmitter | null} */
  let metrics = null;

  // -------- Wave 1 — CDP client + new channel emitters/receivers ----
  //
  // Background: the streamer.js page lives inside the headless Chromium
  // that also hosts the user content. CDP at ws://localhost:9222
  // gives us:
  //   * `Runtime.addBinding` + `Page.addScriptToEvaluateOnNewDocument`
  //     to install in-page probes that callback when the cursor shape
  //     or clipboard contents change (matches the existing
  //     capture/cursor-watcher and capture/clipboard-bridge sidecars,
  //     which we don't reuse in-process because driving CDP directly
  //     from the streamer is one fewer hop and avoids the wire-format
  //     translation tax).
  //   * `Browser.setClipboard` / `Runtime.evaluate` for inbound
  //     clipboard application.
  //   * `DOM.setFileInputFiles` for file-upload landing on the focused
  //     `<input type=file>`.
  //
  // CDP failure mode: if the WebSocket dial fails (e.g. flag set
  // doesn't expose DevTools to the page, or :9222 is unreachable), we
  // still create the DataChannels — the labels need to land in the
  // offer SDP so the portal-side demux doesn't drop into the
  // `other =>` arm — but we log a single warning and leave the
  // emitters dormant. The portal sees an open channel that never
  // produces traffic; that's a strictly less-broken state than a
  // closed channel that flips state on the portal side.
  //
  // Contract reference:
  // /workspace/.triform/guides/chromeless-datachannel-contract.md
  // (ships in the triform monorepo; the chromeless side reads it but
  // does not vendor it).

  // CDPClient — a tiny Chrome DevTools Protocol client speaking the
  // /devtools/browser flat-mode shape, mirroring capture/input-bridge's
  // dialCDP/pageSessionSender plumbing. We only need a handful of
  // methods (Runtime.{enable,addBinding,evaluate}, Page.{enable,
  // addScriptToEvaluateOnNewDocument}, Browser.{grantPermissions,
  // setClipboard}, DOM.setFileInputFiles), so we don't bother with
  // session-attach machinery — Runtime.evaluate at the browser-level
  // target executes against the active page when no session is
  // attached, which is sufficient for v1.
  //
  // Disposal: on pc.close (via teardown) we close the underlying
  // WebSocket. `closed` is set first so any in-flight send rejects
  // with a deterministic Error rather than racing on readyState.
  class CDPClient {
    constructor() {
      this.ws = null;
      this.closed = false;
      this.nextId = 1;
      /** @type {Map<number, {resolve: (v:any)=>void, reject:(e:Error)=>void}>} */
      this.pending = new Map();
      /** @type {((ev:{method:string, params:any})=>void)[]} */
      this.eventListeners = [];
    }

    async connect(baseUrl) {
      // Discover the browser-level WS URL via /json/version.
      const versionUrl = baseUrl.replace(/\/$/, "") + "/json/version";
      let resp;
      try {
        resp = await fetch(versionUrl);
      } catch (err) {
        throw new Error(`CDP /json/version fetch failed: ${err}`);
      }
      if (!resp.ok) throw new Error(`CDP /json/version status ${resp.status}`);
      const meta = await resp.json();
      const wsUrl = meta.webSocketDebuggerUrl;
      if (!wsUrl) throw new Error("CDP /json/version missing webSocketDebuggerUrl");
      await new Promise((resolve, reject) => {
        const ws = new WebSocket(wsUrl);
        this.ws = ws;
        ws.onopen = () => resolve();
        ws.onerror = (e) => reject(new Error("CDP WS open error " + (e?.message ?? "")));
        ws.onclose = () => {
          this.closed = true;
          // Reject every pending request; the caller's onclose path
          // will tear down the relevant emitters.
          for (const p of this.pending.values()) {
            try { p.reject(new Error("CDP disconnected")); } catch { /* ignore */ }
          }
          this.pending.clear();
        };
        ws.onmessage = (ev) => this._onMessage(ev);
      });
    }

    _onMessage(ev) {
      let frame;
      try {
        frame = JSON.parse(typeof ev.data === "string" ? ev.data : "");
      } catch {
        return; // malformed, drop
      }
      if (typeof frame.id === "number") {
        const slot = this.pending.get(frame.id);
        if (!slot) return;
        this.pending.delete(frame.id);
        if (frame.error) {
          slot.reject(new Error(`CDP error: ${frame.error.message ?? JSON.stringify(frame.error)}`));
        } else {
          slot.resolve(frame.result ?? null);
        }
        return;
      }
      // Event frame
      if (typeof frame.method === "string") {
        for (const fn of this.eventListeners) {
          try { fn(frame); } catch (e) { log("warn", "CDP event listener threw", String(e)); }
        }
      }
    }

    on(fn) {
      this.eventListeners.push(fn);
      return () => {
        const i = this.eventListeners.indexOf(fn);
        if (i >= 0) this.eventListeners.splice(i, 1);
      };
    }

    send(method, params) {
      if (this.closed || !this.ws || this.ws.readyState !== WebSocket.OPEN) {
        return Promise.reject(new Error("CDP not open"));
      }
      const id = this.nextId++;
      const frame = JSON.stringify({ id, method, params: params ?? {} });
      const p = new Promise((resolve, reject) => {
        this.pending.set(id, { resolve, reject });
      });
      try {
        this.ws.send(frame);
      } catch (err) {
        this.pending.delete(id);
        return Promise.reject(err instanceof Error ? err : new Error(String(err)));
      }
      return p;
    }

    close() {
      if (this.closed) return;
      this.closed = true;
      try {
        if (this.ws && (this.ws.readyState === WebSocket.OPEN ||
                         this.ws.readyState === WebSocket.CONNECTING)) {
          this.ws.close();
        }
      } catch { /* ignore */ }
      this.ws = null;
      this.eventListeners = [];
    }
  }

  // Helper for emitters: defensive write to a DataChannel that drops
  // the message rather than throwing if the channel is closed mid-send
  // (race window during teardown).
  function dcSafeSend(dc, payload) {
    if (!dc || dc.readyState !== "open") return false;
    try {
      dc.send(payload);
      return true;
    } catch (err) {
      log("warn", "data channel send failed", String(err));
      return false;
    }
  }

  // Wave 1 §4 — CursorEmitter
  //
  // CDP doesn't expose a first-class "cursor changed" event (the
  // original A1 brief mentioned `Page.setCursorChanged`, which is not
  // part of the public DevTools Protocol). The proven path — already
  // used by the capture/cursor-watcher Go service — is to inject a
  // small JS probe into every document via
  // `Page.addScriptToEvaluateOnNewDocument`, observe pointer
  // movement + `getComputedStyle(...).cursor`, and call back through
  // a `Runtime.addBinding` exposed function on each change.
  //
  // The probe keeps page-CSS-pixel coordinates (clientX/clientY are
  // already in CSS pixels per the CSSOM spec, regardless of
  // devicePixelRatio), matching the contract's coordinate space.
  //
  // Drop-stale-on-coalesce backpressure: we hold at most one
  // outstanding cursor message per channel. If `dc.bufferedAmount`
  // exceeds zero when the next change arrives, we replace the queued
  // message (overwrite `pendingPayload`) rather than enqueue a
  // second.
  class CursorEmitter {
    constructor(dc, cdp) {
      this.dc = dc;
      this.cdp = cdp;
      this.detach = null;
      this.pendingPayload = null; // {x,y,kind} — most-recent unflushed sample
      this.flushPending = false;
      this.failed = false;
      dc.addEventListener("close", () => this.dispose("dc closed"));
      dc.addEventListener("error", (e) => log("warn", "cursor data-channel error",
        String((e && e.error?.message) || e)));
    }

    static probeJS() {
      // Self-contained, idempotent. Runs in every document.
      // Coordinates are page CSS pixels (clientX/Y). Kind is the
      // computed-style cursor keyword for the topmost element under
      // the pointer; we map a few common synonyms to the contract's
      // names and otherwise pass through (the portal-side B1 handler
      // is required to fall back to "default" for unknown kinds —
      // contract §4).
      return `(() => {
        if (window.__cb_cursor_v1_installed__) return;
        window.__cb_cursor_v1_installed__ = true;
        let lastX = 0, lastY = 0, lastKind = "default", lastVisible = true;
        let scheduled = false;
        function describe(cssCursor) {
          if (!cssCursor || cssCursor === "auto") return "default";
          if (cssCursor === "none") return "none";
          // First non-url() keyword (CSS allows a fallback list).
          const head = cssCursor.split(",").map(s => s.trim()).find(s => s && !s.startsWith("url("));
          return head ? head.split(/\\s+/)[0] : "default";
        }
        function emit() {
          scheduled = false;
          const el = document.elementFromPoint(lastX, lastY);
          const css = el ? getComputedStyle(el).cursor : "default";
          const kind = describe(css);
          const visible = kind !== "none";
          if (lastKind === kind && lastVisible === visible) return;
          lastKind = kind; lastVisible = visible;
          if (typeof window.__cb_cursor_v1__ === "function") {
            try {
              window.__cb_cursor_v1__(JSON.stringify({
                x: lastX, y: lastY, kind: visible ? kind : "none",
              }));
            } catch (e) { /* ignore */ }
          }
        }
        function schedule() {
          if (scheduled) return;
          scheduled = true;
          requestAnimationFrame(emit);
        }
        document.addEventListener("pointermove", (e) => {
          lastX = e.clientX; lastY = e.clientY;
          if (typeof window.__cb_cursor_v1_pos__ === "function") {
            try {
              window.__cb_cursor_v1_pos__(JSON.stringify({
                x: lastX, y: lastY,
              }));
            } catch (err) { /* ignore */ }
          }
          schedule();
        }, { passive: true, capture: true });
        document.addEventListener("pointerleave", () => {
          if (lastVisible) {
            lastVisible = false; lastKind = "none";
            if (typeof window.__cb_cursor_v1__ === "function") {
              try {
                window.__cb_cursor_v1__(JSON.stringify({
                  x: lastX, y: lastY, kind: "none",
                }));
              } catch (e) { /* ignore */ }
            }
          }
        }, { passive: true });
      })();`;
    }

    async install() {
      // Best-effort. If any step fails, we log + flag the emitter
      // failed and skip future activity but keep the DC open — the
      // portal sees an open-but-quiet channel, which is the documented
      // dormant-channel state.
      try {
        await this.cdp.send("Page.enable");
        await this.cdp.send("Runtime.enable");
        await this.cdp.send("Runtime.addBinding", { name: "__cb_cursor_v1__" });
        await this.cdp.send("Runtime.addBinding", { name: "__cb_cursor_v1_pos__" });
        await this.cdp.send("Page.addScriptToEvaluateOnNewDocument", {
          source: CursorEmitter.probeJS(),
        });
        // Also install in the current document (no navigation needed).
        await this.cdp.send("Runtime.evaluate", {
          expression: CursorEmitter.probeJS(),
          includeCommandLineAPI: false,
          awaitPromise: false,
          returnByValue: true,
        });
        this.detach = this.cdp.on((frame) => {
          if (frame.method !== "Runtime.bindingCalled") return;
          const p = frame.params || {};
          const name = p.name;
          if (name !== "__cb_cursor_v1__" && name !== "__cb_cursor_v1_pos__") return;
          let payload;
          try { payload = JSON.parse(p.payload || "{}"); }
          catch { return; }
          if (typeof payload.x !== "number" || typeof payload.y !== "number") return;
          const kind = (typeof payload.kind === "string" && payload.kind) || (this.pendingPayload?.kind ?? "default");
          this.queue({ x: payload.x, y: payload.y, kind });
        });
        log("ok", "cursor emitter installed");
      } catch (err) {
        this.failed = true;
        log("warn", "cursor emitter install failed; channel will stay dormant",
            String(err));
      }
    }

    queue(payload) {
      if (this.failed) return;
      // Drop-stale-on-coalesce: replace whatever is queued.
      // Wave 2 A4: count each coalesce so dashboards can quantify how
      // often backpressure replaces an unflushed payload (high-volume
      // signal that the cursor probe outpaces its DataChannel drain).
      if (this.pendingPayload !== null && metrics) {
        metrics.emit("chromeless.webrtc.dc.cursor.coalesced", {});
      }
      this.pendingPayload = payload;
      if (this.flushPending) return;
      this.flushPending = true;
      // Use queueMicrotask to give the bufferedAmount drain a chance
      // before re-checking. This is not a hard cadence — the next
      // `pointermove` will trigger another drain attempt anyway.
      queueMicrotask(() => this.flush());
    }

    flush() {
      this.flushPending = false;
      const p = this.pendingPayload;
      if (!p) return;
      // Backpressure: if the channel has bytes queued, we keep our
      // single pendingPayload and let the next change overwrite it.
      // The buffered byte count is the spec-defined backpressure
      // signal; we tolerate up to 16 KiB before pausing emit.
      if (!this.dc || this.dc.readyState !== "open") return;
      if (this.dc.bufferedAmount > 16_384) return;
      const env = JSON.stringify({
        v: 1, t: Date.now(),
        cursor: { x: p.x, y: p.y, kind: p.kind },
      });
      this.pendingPayload = null;
      dcSafeSend(this.dc, env);
    }

    dispose(reason) {
      if (this.detach) {
        try { this.detach(); } catch { /* ignore */ }
        this.detach = null;
      }
      this.pendingPayload = null;
      log("info", `cursor emitter disposed: ${reason}`);
    }
  }

  // Wave 1 §5 — ClipboardChannel (bidirectional + per-session sequence
  // numbers).
  //
  // Outbound (chromeless→portal, dir="c2p"): when any document fires a
  // `copy` event, the page-side probe captures the selection text /
  // HTML and calls a binding. We translate to a contract envelope and
  // emit on the DC, incrementing `nextOutboundSeq`.
  //
  // Inbound (portal→chromeless, dir="p2c"): we receive an envelope on
  // the DC, drop if `seq <= lastSeenInboundSeq`, otherwise base64-
  // decode `data` and write into the in-container clipboard via CDP
  // Runtime.evaluate (`navigator.clipboard.writeText`-equivalent
  // through Browser.grantPermissions; falls back to Input.insertText
  // is out of scope for v1). Then advance `lastSeenInboundSeq`.
  //
  // MIME types: only `text/plain` and `text/html` are accepted; others
  // are dropped with a DEBUG log line (per contract §5). Unknown
  // direction strings are dropped.
  class ClipboardChannel {
    constructor(dc, cdp) {
      this.dc = dc;
      this.cdp = cdp;
      this.detach = null;
      this.nextOutboundSeq = 0;
      this.lastSeenInboundSeq = -1;
      this.failed = false;
      // Echo suppression: we remember the last text we *applied
      // inbound* so the page-side `copy` listener doesn't loop the
      // same text right back out. Mirrors clipboard-bridge §"Echo
      // suppression".
      this.lastInboundText = null;
      dc.addEventListener("close", () => this.dispose("dc closed"));
      dc.addEventListener("error", (e) => log("warn", "clipboard data-channel error",
        String((e && e.error?.message) || e)));
      dc.addEventListener("message", (ev) => this.onInbound(ev.data));
    }

    static probeJS() {
      return `(() => {
        if (window.__cb_clipboard_v1_installed__) return;
        window.__cb_clipboard_v1_installed__ = true;
        document.addEventListener("copy", (e) => {
          // Best-effort capture: the spec says the active selection
          // is what gets copied; we read it and forward.
          const sel = document.getSelection ? document.getSelection() : null;
          const text = sel ? sel.toString() : "";
          if (!text) return;
          // No HTML capture in v1 — text/plain only on the c2p path.
          // Adding text/html is forward-compatible (just bumps the
          // emitter; the contract already permits it).
          if (typeof window.__cb_clipboard_v1__ === "function") {
            try {
              window.__cb_clipboard_v1__(JSON.stringify({
                mime: "text/plain", text,
              }));
            } catch (err) { /* ignore */ }
          }
        }, { capture: true });
      })();`;
    }

    static utf8ToBase64(str) {
      // TextEncoder + btoa pipeline; handles non-Latin-1 cleanly.
      const bytes = new TextEncoder().encode(str);
      let bin = "";
      for (let i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i]);
      return btoa(bin);
    }

    static base64ToUtf8(b64) {
      const bin = atob(b64);
      const bytes = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
      return new TextDecoder().decode(bytes);
    }

    async install() {
      try {
        await this.cdp.send("Page.enable");
        await this.cdp.send("Runtime.enable");
        // Browser.grantPermissions makes the in-page Clipboard API
        // calls in `applyInbound` work without a permission prompt.
        // Some Chromium builds reject `clipboardSanitizedWrite`; we
        // retry with the smaller set on failure.
        try {
          await this.cdp.send("Browser.grantPermissions", {
            permissions: ["clipboardReadWrite", "clipboardSanitizedWrite"],
          });
        } catch (e) {
          await this.cdp.send("Browser.grantPermissions", {
            permissions: ["clipboardReadWrite"],
          }).catch(() => {});
        }
        await this.cdp.send("Runtime.addBinding", { name: "__cb_clipboard_v1__" });
        await this.cdp.send("Page.addScriptToEvaluateOnNewDocument", {
          source: ClipboardChannel.probeJS(),
        });
        await this.cdp.send("Runtime.evaluate", {
          expression: ClipboardChannel.probeJS(),
          includeCommandLineAPI: false,
          awaitPromise: false,
          returnByValue: true,
        });
        this.detach = this.cdp.on((frame) => {
          if (frame.method !== "Runtime.bindingCalled") return;
          const p = frame.params || {};
          if (p.name !== "__cb_clipboard_v1__") return;
          let payload;
          try { payload = JSON.parse(p.payload || "{}"); }
          catch { return; }
          this.emitOutbound(payload);
        });
        log("ok", "clipboard channel installed");
      } catch (err) {
        this.failed = true;
        log("warn", "clipboard install failed; channel stays dormant",
            String(err));
      }
    }

    emitOutbound(payload) {
      if (this.failed) return;
      const mime = payload.mime === "text/html" ? "text/html" : "text/plain";
      const text = String(payload.text ?? "");
      if (!text) return;
      // Echo suppression — drop a `copy` that mirrors the last
      // inbound write we just applied.
      if (this.lastInboundText !== null && this.lastInboundText === text) {
        // One-shot suppression; future `copy` events of the same
        // text are real user activity.
        this.lastInboundText = null;
        return;
      }
      const data = ClipboardChannel.utf8ToBase64(text);
      const env = JSON.stringify({
        v: 1, t: Date.now(),
        clipboard: {
          dir: "c2p",
          seq: this.nextOutboundSeq++,
          mime, data,
        },
      });
      dcSafeSend(this.dc, env);
    }

    onInbound(data) {
      if (typeof data !== "string") {
        log("warn", "clipboard inbound: non-string frame, dropping");
        return;
      }
      let env;
      try { env = JSON.parse(data); }
      catch (err) { log("warn", "clipboard inbound: malformed JSON", String(err)); return; }
      const v = env?.v;
      if (v !== 1) {
        log("warn", "webrtc_dc_clipboard_unsupported_version", { received: v });
        return;
      }
      const cb = env?.clipboard;
      if (!cb || typeof cb !== "object") return;
      // We only act on p2c. Loopback c2p frames (shouldn't happen,
      // but defense in depth) are ignored.
      if (cb.dir !== "p2c") return;
      const seq = Number(cb.seq);
      if (!Number.isFinite(seq) || seq <= this.lastSeenInboundSeq) {
        // Stale. Per contract §5: drop silently with DEBUG log.
        log("info", "clipboard inbound stale; dropping",
            { seq, last_seen: this.lastSeenInboundSeq });
        // Wave 2 A4 — counter signals seq protocol drift (typically a
        // late-arriving retry or a portal-side bug emitting out of
        // order; should be near-zero in steady state).
        if (metrics) metrics.emit("chromeless.webrtc.dc.clipboard.stale_seq", {});
        return;
      }
      const mime = cb.mime;
      if (mime !== "text/plain" && mime !== "text/html") {
        log("info", "clipboard inbound: unsupported mime, dropping",
            { mime });
        // Still advance lastSeen so we don't replay it on a re-emit.
        this.lastSeenInboundSeq = seq;
        // Wave 2 A4 — flag MIME the contract doesn't support; non-zero
        // indicates a portal-side B2 producer trying to send something
        // outside the agreed (text/plain | text/html) set.
        if (metrics) metrics.emit("chromeless.webrtc.dc.clipboard.unsupported_mime", { mime: String(mime ?? "") });
        return;
      }
      let text;
      try { text = ClipboardChannel.base64ToUtf8(String(cb.data ?? "")); }
      catch (err) { log("warn", "clipboard inbound: base64 decode failed", String(err)); return; }
      this.lastSeenInboundSeq = seq;
      this.applyInbound(mime, text);
    }

    applyInbound(mime, text) {
      if (this.failed || !this.cdp || this.cdp.closed) return;
      this.lastInboundText = mime === "text/plain" ? text : null;
      // Use Runtime.evaluate to call navigator.clipboard.writeText.
      // For text/html we'd want navigator.clipboard.write with a
      // ClipboardItem; v1 keeps both paths on writeText for the
      // text content so paste-to-text-input always works.
      const expr = `(async () => {
        try {
          await navigator.clipboard.writeText(${JSON.stringify(text)});
          return "ok";
        } catch (e) { return "err:" + (e && e.message || e); }
      })()`;
      this.cdp.send("Runtime.evaluate", {
        expression: expr,
        awaitPromise: true,
        returnByValue: true,
      }).then((res) => {
        const v = res?.result?.value;
        if (typeof v === "string" && v.startsWith("err:")) {
          log("warn", "clipboard apply via navigator.clipboard failed", v);
        }
      }).catch((err) => {
        log("warn", "clipboard apply CDP send failed", String(err));
      });
    }

    dispose(reason) {
      if (this.detach) {
        try { this.detach(); } catch { /* ignore */ }
        this.detach = null;
      }
      log("info", `clipboard channel disposed: ${reason}`);
    }
  }

  // Wave 1 §6 — FileUploadReceiver (portal→chromeless, one-way).
  //
  // The portal-side B3 producer chunks each upload (≤64 KiB raw bytes
  // per chunk, base64-encoded into the JSON envelope) and pumps over
  // the `file-upload` DC; we reassemble per `upload_id` and call CDP
  // `DOM.setFileInputFiles` against the focused `<input type=file>`
  // when `eof: true` arrives.
  //
  // Validation per contract §6:
  //   1. seq monotonic non-decreasing per upload_id (allow 4-ahead).
  //   2. total constant across an upload_id.
  //   3. eof: true ⇔ seq == total - 1.
  //   4. name sanitised (path separators / NUL / control chars
  //      stripped, truncated to 255 bytes).
  //
  // Orphan timeout: 60 s of no new chunk → discard buffer + warn.
  class FileUploadReceiver {
    constructor(dc, cdp) {
      this.dc = dc;
      this.cdp = cdp;
      /** @type {Map<string, {chunks: Map<number, Uint8Array>, total: number, name: string, mime: string, expected: number, totalBytes: number, lastSeen: number, timer: number | null}>} */
      this.uploads = new Map();
      dc.addEventListener("close", () => this.dispose("dc closed"));
      dc.addEventListener("error", (e) => log("warn", "file-upload data-channel error",
        String((e && e.error?.message) || e)));
      dc.addEventListener("message", (ev) => this.onChunk(ev.data));
    }

    static sanitiseName(raw) {
      if (typeof raw !== "string") return "upload.bin";
      // Strip path separators, NUL, and other control characters.
      // eslint-disable-next-line no-control-regex
      let s = raw.replace(/[\\\/\x00-\x1f\x7f]/g, "_");
      // Disallow ".." segments and leading dots that produce hidden files.
      s = s.replace(/\.\.+/g, "_");
      s = s.replace(/^\.+/, "");
      if (!s) s = "upload.bin";
      // Truncate to 255 BYTES (post-UTF-8 encode), not 255 chars.
      const enc = new TextEncoder();
      let bytes = enc.encode(s);
      if (bytes.length <= FILE_UPLOAD_NAME_MAX_BYTES) return s;
      // Walk back from the truncation point until we land on a valid
      // UTF-8 boundary so we don't emit a half-codepoint.
      let cutoff = FILE_UPLOAD_NAME_MAX_BYTES;
      while (cutoff > 0 && (bytes[cutoff] & 0xc0) === 0x80) cutoff--;
      return new TextDecoder().decode(bytes.slice(0, cutoff));
    }

    static base64ToBytes(b64) {
      const bin = atob(b64);
      const bytes = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
      return bytes;
    }

    onChunk(raw) {
      if (typeof raw !== "string") {
        log("warn", "file-upload: non-string frame, dropping");
        return;
      }
      let env;
      try { env = JSON.parse(raw); }
      catch (err) { log("warn", "file-upload: malformed JSON", String(err)); return; }
      const v = env?.v;
      if (v !== 1) {
        log("warn", "webrtc_dc_file_upload_unsupported_version", { received: v });
        return;
      }
      const c = env?.chunk;
      if (!c || typeof c !== "object") return;
      const upload_id = String(c.upload_id ?? "");
      const seq = Number(c.seq);
      const total = Number(c.total);
      const eof = !!c.eof;
      const name = FileUploadReceiver.sanitiseName(c.name);
      const mime = typeof c.mime === "string" && c.mime ? c.mime : "application/octet-stream";
      if (!upload_id) {
        log("warn", "file-upload: missing upload_id, dropping chunk");
        return;
      }
      if (!Number.isInteger(seq) || seq < 0 ||
          !Number.isInteger(total) || total <= 0) {
        log("warn", "file-upload: bad seq/total, dropping",
            { upload_id, seq, total });
        return;
      }
      // Validation rule §6.3: eof ⇔ seq == total - 1.
      if (eof !== (seq === total - 1)) {
        log("warn", "file-upload: eof/seq mismatch, dropping upload",
            { upload_id, seq, total, eof });
        this.discard(upload_id);
        return;
      }
      let entry = this.uploads.get(upload_id);
      if (!entry) {
        entry = {
          chunks: new Map(),
          total, name, mime,
          expected: 0,
          totalBytes: 0,
          lastSeen: Date.now(),
          timer: null,
        };
        this.uploads.set(upload_id, entry);
      } else if (entry.total !== total) {
        log("warn", "file-upload: total mismatch across chunks, dropping",
            { upload_id, prev_total: entry.total, new_total: total });
        this.discard(upload_id);
        return;
      }
      // Validation rule §6.1: monotonic non-decreasing seq, with up
      // to 4-ahead reorder window. We accept both seq < expected
      // (duplicate retry) and seq within [expected, expected+4]
      // (reorder); seq >= expected+5 means a hole we cannot close.
      if (seq < entry.expected) {
        // Duplicate/late chunk we already buffered & flushed past.
        // Drop silently — the DC is reliable and ordered, so this
        // is a redelivery the lower layer didn't suppress.
        return;
      }
      if (seq > entry.expected + FILE_UPLOAD_REORDER_AHEAD) {
        log("warn", "file-upload: seq jump beyond reorder window, dropping",
            { upload_id, seq, expected: entry.expected });
        this.discard(upload_id);
        return;
      }
      let bytes;
      try { bytes = FileUploadReceiver.base64ToBytes(String(c.data ?? "")); }
      catch (err) { log("warn", "file-upload: base64 decode failed",
                        { upload_id, seq, err: String(err) });
                    this.discard(upload_id); return; }
      // Don't double-store a chunk we already received.
      if (!entry.chunks.has(seq)) {
        entry.chunks.set(seq, bytes);
        entry.totalBytes += bytes.length;
      }
      // Slide the expected pointer past contiguous chunks we now have.
      while (entry.chunks.has(entry.expected)) entry.expected++;
      entry.lastSeen = Date.now();
      this.armOrphanTimer(upload_id);
      // §6.3 — when we see eof and `expected` has advanced past it,
      // we have everything in seq order. (If reorder buffered chunks
      // higher than `expected` are still missing, expected won't have
      // reached total yet and we wait.)
      if (eof && entry.expected >= total) {
        this.complete(upload_id);
      }
    }

    armOrphanTimer(upload_id) {
      const entry = this.uploads.get(upload_id);
      if (!entry) return;
      if (entry.timer !== null) clearTimeout(entry.timer);
      entry.timer = setTimeout(() => {
        const e2 = this.uploads.get(upload_id);
        if (!e2) return;
        log("warn", "file-upload: orphan timeout, discarding",
            { upload_id, name: e2.name, received_chunks: e2.chunks.size,
              expected_total: e2.total });
        // Wave 2 A4 — orphan timeout fires when the portal abandons an
        // upload mid-stream; non-zero indicates flaky DataChannel or a
        // portal-side bug (B3 producer not driving to eof).
        if (metrics) metrics.emit("chromeless.webrtc.dc.file_upload.orphan_timeout", {
          received_chunks: e2.chunks.size,
          expected_total: e2.total,
        });
        this.discard(upload_id);
      }, FILE_UPLOAD_ORPHAN_MS);
    }

    discard(upload_id) {
      const entry = this.uploads.get(upload_id);
      if (entry) {
        if (entry.timer !== null) clearTimeout(entry.timer);
        // Drop chunk byte references so the GC can reclaim memory.
        entry.chunks.clear();
      }
      this.uploads.delete(upload_id);
    }

    async complete(upload_id) {
      const entry = this.uploads.get(upload_id);
      if (!entry) return;
      if (entry.timer !== null) { clearTimeout(entry.timer); entry.timer = null; }
      // Concatenate buffered chunks in seq order.
      const merged = new Uint8Array(entry.totalBytes);
      let offset = 0;
      for (let i = 0; i < entry.total; i++) {
        const chunk = entry.chunks.get(i);
        if (!chunk) {
          log("warn", "file-upload: missing chunk at completion, discarding",
              { upload_id, missing_seq: i });
          this.discard(upload_id);
          return;
        }
        merged.set(chunk, offset);
        offset += chunk.length;
      }
      // Re-encode to base64 for CDP DOM.setFileInputFiles? No —
      // setFileInputFiles wants on-disk paths, NOT bytes. v1 takes
      // the practical fallback per the architect's brief: write the
      // file via Runtime.evaluate using a Blob → DataTransferItem
      // pipeline against the focused <input type=file>. This avoids
      // touching the disk and matches B3's expectation that the
      // bytes drive the input directly.
      //
      // The Runtime.evaluate path uses the page's File constructor
      // and a synthetic DataTransfer to assemble FileList. Many
      // pages read input.files in a `change` handler, so we
      // dispatch a synthetic 'change' event after assignment.
      try {
        const b64 = (() => {
          let bin = "";
          for (let i = 0; i < merged.length; i++) bin += String.fromCharCode(merged[i]);
          return btoa(bin);
        })();
        // Build a JS expression that:
        //   1. Locates the focused <input type=file>; falls back to
        //      the first such input on the page if no file input has
        //      focus. Returns "no-input" when neither exists.
        //   2. Decodes the base64 payload into a Uint8Array.
        //   3. Constructs a File and a DataTransfer; assigns
        //      input.files; dispatches `input` and `change`.
        const expr = `(() => {
          try {
            let inp = document.activeElement;
            if (!(inp instanceof HTMLInputElement) || inp.type !== "file") {
              inp = document.querySelector('input[type="file"]');
            }
            if (!inp) return "no-input";
            const bin = atob(${JSON.stringify(b64)});
            const buf = new Uint8Array(bin.length);
            for (let i = 0; i < bin.length; i++) buf[i] = bin.charCodeAt(i);
            const file = new File([buf], ${JSON.stringify(entry.name)}, { type: ${JSON.stringify(entry.mime)} });
            const dt = new DataTransfer();
            dt.items.add(file);
            inp.files = dt.files;
            inp.dispatchEvent(new Event("input", { bubbles: true }));
            inp.dispatchEvent(new Event("change", { bubbles: true }));
            return "ok";
          } catch (e) { return "err:" + (e && e.message || e); }
        })()`;
        const res = await this.cdp.send("Runtime.evaluate", {
          expression: expr,
          awaitPromise: false,
          returnByValue: true,
        });
        const out = res?.result?.value;
        // Wave 2 A4 — record outcome distribution for dashboards.
        // Result values: "ok" (file applied to focused input),
        // "no-input" (page had no <input type=file>), or "err" (any
        // page-side throw). The CDP-call-failed path below also emits
        // "err" via the catch block.
        let result;
        if (out === "ok") {
          log("info", "file-upload completed",
              { upload_id, name: entry.name, mime: entry.mime,
                total_bytes: entry.totalBytes });
          result = "ok";
        } else if (out === "no-input") {
          log("warn", "file-upload: no <input type=file> focused/available, dropping",
              { upload_id, name: entry.name });
          result = "no-input";
        } else {
          log("warn", "file-upload: page-side apply failed",
              { upload_id, result: String(out) });
          result = "err";
        }
        if (metrics) metrics.emit("chromeless.webrtc.dc.file_upload.completed", {
          result, total_bytes: entry.totalBytes,
        });
      } catch (err) {
        log("warn", "file-upload: CDP apply failed",
            { upload_id, err: String(err) });
        if (metrics) metrics.emit("chromeless.webrtc.dc.file_upload.completed", {
          result: "err", total_bytes: entry.totalBytes,
        });
      } finally {
        this.discard(upload_id);
      }
    }

    dispose(reason) {
      for (const upload_id of Array.from(this.uploads.keys())) {
        this.discard(upload_id);
      }
      log("info", `file-upload receiver disposed: ${reason}`);
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
    // Wave 2 A4 — emit session_closed BEFORE we dispose anything
    // session-bound. The emit is fire-and-forget against the local
    // sidecar; it doesn't depend on the PC, so calling it first keeps
    // the duration measurement honest (between session_created and
    // start of teardown, not the end of a multi-step disposal).
    if (typeof active.emitSessionClosed === "function") {
      try { active.emitSessionClosed(reason); } catch { /* never block teardown */ }
    }
    if (heartbeatTimer) { clearInterval(heartbeatTimer); heartbeatTimer = null; }
    if (active.inputRelay) {
      try { active.inputRelay.close(); } catch { /* ignore */ }
    }
    // Wave 1 — dispose new emitters/receivers + close CDP. Order:
    // emitters first (they may try one final send), then CDP (the
    // emitters' disposal listeners fire on dc close, which is OK to
    // invoke after their own dispose() too — both paths are
    // idempotent).
    if (active.cursorEmitter) {
      try { active.cursorEmitter.dispose("teardown"); } catch { /* ignore */ }
    }
    if (active.clipboardChannel) {
      try { active.clipboardChannel.dispose("teardown"); } catch { /* ignore */ }
    }
    if (active.fileUploadReceiver) {
      try { active.fileUploadReceiver.dispose("teardown"); } catch { /* ignore */ }
    }
    if (active.cdp) {
      try { active.cdp.close(); } catch { /* ignore */ }
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

    // Wave 2 A4: lazily create the per-process metrics emitter on the
    // first start() call. Nothing emits before this point; a
    // pre-PeerConnection emit would have nowhere to flush to anyway.
    if (!metrics) {
      metrics = new MetricsEmitter(WEBRTC_METRICS_URL);
    }

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

    // Wave 2 A4 — per-PC session lifecycle bookkeeping.
    //
    // sessionId is a UUID minted on this side so dashboards can pivot
    // on a label that's stable across the session_created/session_closed
    // pair. SESSION_ID (the URL param) is a logical session identifier
    // for the broker; we keep them separate because the streamer can
    // re-attach to a broker session multiple times during a single
    // page life and each PC instance is observability-distinct.
    const sessionId = (() => {
      // crypto.randomUUID is available in Chromium 92+; the streamer
      // runs on 147 so this is safe. Fallback path is for tests that
      // mock crypto.
      if (typeof crypto !== "undefined" && typeof crypto.randomUUID === "function") {
        return crypto.randomUUID();
      }
      return `s${Date.now().toString(36)}${Math.random().toString(36).slice(2, 8)}`;
    })();
    let sessionStartedAt = null;       // ms timestamp of session_created emit
    let sessionCreated = false;        // emit-once latch
    let firstDCOpenedSeen = false;     // handshake_ms attached to first dc.opened only
    let iceFailedDebounce = null;      // setTimeout handle for the 3 s ice.failed debounce

    function emitSessionCreated(reason) {
      if (sessionCreated) return;
      sessionCreated = true;
      sessionStartedAt = Date.now();
      log("info", "webrtc session_created", { sessionId, reason });
      if (metrics) {
        metrics.emit("chromeless.webrtc.session_created", {
          session_id: sessionId, signaling_session: SESSION_ID, reason,
        });
      }
    }

    function emitSessionClosed(reason) {
      if (!sessionCreated) return; // never opened → never closes
      const durationMs = sessionStartedAt !== null
        ? Math.max(0, Date.now() - sessionStartedAt) : null;
      log("info", "webrtc session_closed", { sessionId, reason, duration_ms: durationMs });
      if (metrics) {
        metrics.emit("chromeless.webrtc.session_closed", {
          session_id: sessionId,
          duration_ms: durationMs,
          reason,
        });
      }
      // Latch: a second close attempt is a no-op.
      sessionStartedAt = null;
      sessionCreated = false;
    }

    pc.onsignalingstatechange   = () => log("info", `signalingState=${pc.signalingState}`);
    pc.oniceconnectionstatechange = () => {
      const state = pc.iceConnectionState;
      const lvl = state === "failed" ? "err" : "info";
      log(lvl, `iceConnectionState=${state}`);
      // Wave 2 A4 — dimension-less counters per D4-YAML.
      if (state === "connected" || state === "completed") {
        // Cancel any pending failed-debounce; ICE recovered.
        if (iceFailedDebounce !== null) {
          clearTimeout(iceFailedDebounce);
          iceFailedDebounce = null;
        }
        if (metrics) {
          metrics.emit("chromeless.webrtc.ice.connected", {
            session_id: sessionId, ice_state: state,
          });
        }
      } else if (state === "failed" || state === "disconnected") {
        // Debounce ~3 s — disconnected often recovers without
        // intervention, and the YAML's failure semantic is "stayed
        // past the recovery window". Failed is permanent on Chromium
        // but we route both through the same debounce for consistency.
        if (iceFailedDebounce === null) {
          iceFailedDebounce = setTimeout(() => {
            iceFailedDebounce = null;
            const finalState = pc.iceConnectionState;
            if (finalState !== "connected" && finalState !== "completed") {
              if (metrics) {
                metrics.emit("chromeless.webrtc.ice.failed", {
                  session_id: sessionId,
                  ice_state: finalState,
                  reason: state, // the state that triggered the debounce
                });
              }
            }
          }, 3000);
        }
      }
    };
    pc.onicegatheringstatechange = () => log("info", `iceGatheringState=${pc.iceGatheringState}`);
    pc.onconnectionstatechange = () => {
      const state = pc.connectionState;
      const lvl = state === "failed" ? "err" : "info";
      log(lvl, `connectionState=${state}`);
      // Wave 2 A4 — first transition to "connected" is the session
      // lifecycle marker. RTCPeerConnection's connected state can flap
      // back to "connecting"/"disconnected"/"failed"; we only emit
      // session_created once per PC.
      if (state === "connected") {
        emitSessionCreated("connectionState=connected");
      }
      if (state === "failed") teardown("pc failed");
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
    // Wave 1 §2: three new DataChannel labels. Labels are exact and
    // case-sensitive — any drift silently routes to the portal's
    // `other =>` log-and-drop arm. `cursor` is one-way emit, `stats`-
    // shaped JSON envelopes; `clipboard` is bidirectional with per-
    // session monotonic seq counters; `file-upload` is one-way
    // receive with chunk reassembly per upload_id.
    const cursorDC     = pc.createDataChannel("cursor",      { ordered: true });
    const clipboardDC  = pc.createDataChannel("clipboard",   { ordered: true });
    const fileUploadDC = pc.createDataChannel("file-upload", { ordered: true });
    log("info", "created data channels", {
      input: inputDC.id, stats: statsDC.id,
      cursor: cursorDC.id, clipboard: clipboardDC.id,
      file_upload: fileUploadDC.id,
    });

    // Wave 2 A4 — per-DC `open` listener emits dc.opened for every
    // label so the YAML-declared chromeless_webrtc_dc_opened_count
    // counter and the chromeless_webrtc_signaling_handshake_ms
    // histogram (on the FIRST dc.opened only) populate.
    //
    // The handshake duration is measured from session_created (PC's
    // first connectionState=connected) to the first DC's `open`
    // event. With trickle-ICE that gap is typically 200-800 ms; a
    // long tail signals slow ICE selection or signaling stalls.
    const dcStartedAt = Date.now();
    function wireDCOpenEmit(dc, label) {
      dc.addEventListener("open", () => {
        const attrs = { label, session_id: sessionId };
        if (!firstDCOpenedSeen) {
          firstDCOpenedSeen = true;
          // Prefer measuring from session_created time when available
          // (a DC can open before the PC's connectionState transitions
          // to `connected` because data-channel readiness rides the
          // SCTP handshake, which beats the bundle ICE check). When
          // session_created hasn't fired yet, fall back to a
          // page-relative measure.
          const baseT = sessionStartedAt ?? dcStartedAt;
          attrs.handshake_ms = Math.max(0, Date.now() - baseT);
        }
        if (metrics) metrics.emit("chromeless.webrtc.dc.opened", attrs);
      });
    }
    wireDCOpenEmit(inputDC,      "input");
    wireDCOpenEmit(statsDC,      "stats");
    wireDCOpenEmit(cursorDC,     "cursor");
    wireDCOpenEmit(clipboardDC,  "clipboard");
    wireDCOpenEmit(fileUploadDC, "file-upload");

    const inputRelay = new InputRelay(inputDC, INPUT_BRIDGE_URL);
    const statsRelay = new StatsRelay(statsDC, METRICS_SIDECAR_URL);

    // Wave 1 — best-effort CDP attach. If this fails (e.g. the
    // Chromium flag set blocks WebSocket connections from the
    // streamer page to its own DevTools port, or :9222 is unreachable
    // because of supervisord ordering), we still ship the open-but-
    // dormant DataChannels so the portal demux sees the labels and
    // doesn't drop them. The emitters log the install failure.
    let cdp = null;
    let cursorEmitter = null;
    let clipboardChannel = null;
    let fileUploadReceiver = null;
    if (!WAVE1_DISABLED) {
      cdp = new CDPClient();
      cdp.connect(CDP_BASE_URL).then(() => {
        log("ok", "CDP connected for Wave 1 channels", { base: CDP_BASE_URL });
        cursorEmitter      = new CursorEmitter(cursorDC, cdp);
        clipboardChannel   = new ClipboardChannel(clipboardDC, cdp);
        fileUploadReceiver = new FileUploadReceiver(fileUploadDC, cdp);
        if (active) {
          active.cdp = cdp;
          active.cursorEmitter = cursorEmitter;
          active.clipboardChannel = clipboardChannel;
          active.fileUploadReceiver = fileUploadReceiver;
        }
        // Install probes + bindings. Each install is independent —
        // a clipboard install failure shouldn't prevent cursor.
        cursorEmitter.install();
        clipboardChannel.install();
        // FileUploadReceiver doesn't inject any page-side probe; it
        // just listens for inbound DC messages and uses CDP for the
        // landing call. No install step needed.
      }).catch((err) => {
        log("warn", "CDP connect failed; Wave 1 channels will stay dormant",
            String(err));
        try { cdp.close(); } catch { /* ignore */ }
        cdp = null;
      });
    } else {
      log("info", "Wave 1 channels disabled via ?wave1=off");
    }

    active = {
      ws, pc, stream, inputRelay, statsRelay,
      cdp, cursorEmitter, clipboardChannel, fileUploadReceiver,
      // Wave 2 A4 — the closeover-bound emitSessionClosed lets the
      // module-scope teardown() pair every session_created with
      // exactly one session_closed. Idempotent (the latch inside the
      // closure no-ops a second call).
      emitSessionClosed,
    };

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

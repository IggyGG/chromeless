// ChromelessSession — the reusable answerer state machine (Track 5).
//
// Everything a client needs to bring up a chromeless session WITHOUT
// any DOM coupling: token fetch → ICE config → signaling dial →
// offer/answer/ICE envelope pump → per-connection stats → reconnect
// with peer-connection rebuild → teardown. Extracted verbatim from
// client/main.ts, which had grown a 750-line file where ~350 lines
// were this machine and the rest was demo page; a third party
// integrating chromeless previously had to re-derive the machine by
// reading main.ts (the original Track 5 finding).
//
// Division of labour:
//   ChromelessSession (this file, DOM-free)
//     - signaling lifecycle incl. reconnect + hello frames
//     - RTCPeerConnection build/rebuild per signaling generation
//     - offer → (createAnswer + codec munge) → answer, trickle ICE
//     - codec-negotiation classification (tears down on no_codec)
//     - "stats" data channel + StatsSampler emission
//     - pre-call probe → probe_result emission
//     - bye/teardown semantics
//   caller (demo page main.ts, or your app)
//     - everything that touches a document: video element, input
//       attach + coordinate mapping, cursor overlay, drag-drop file
//       upload, buttons. Delivered the raw material via events
//       ("track", "dataChannel", "pcCreated").
//
// Event, not callback-bag: the session multicasts to any number of
// subscribers (demo page wires a debug panel AND the video element),
// and unsubscribing on teardown is the caller's job via the returned
// detach fn — same contract as StatsSampler.on / ReconnectingWebSocket.on.
//
// Injection seams (options.*) exist for tests and for embedders with
// exotic environments (custom WebSocket impl behind a proxy, wrapped
// RTCPeerConnection): production callers pass nothing and get the
// browser globals. This mirrors reconnect.ts's RWSocketCtor seam.

import { fetchTurnConfig } from "./turn.js";
import { prioritizeCodec } from "./sdp.js";
import {
  ReconnectingWebSocket,
  requestIceRecovery,
  type ReconnectState,
  type RWSocketCtor,
} from "./reconnect.js";

/** Retry metadata attached to signaling stateChange (reconnect.ts). */
export interface SignalingRetryInfo {
  attempt: number;
  nextDelayMs: number;
}
import { StatsSampler, type StatsSample, STATS_PROTOCOL_VERSION } from "./stats.js";
import { fetchSessionToken, withToken, type IssuedToken } from "./auth.js";
import { classifyNegotiation, describeOutcome, type NegotiationResult } from "./codec-negotiate.js";
import { estimateConnectionQuality, type ProbeResult } from "./probe.js";

// The six-tag signaling envelope (docs/protocols/signaling.md). The
// wire contract is owned by capture/signaling/cb_wire_envelope.h; this
// is the client-side mirror of the subset an answerer touches.
export type Envelope =
  | { type: "offer";  from: "client" | "browser"; data: RTCSessionDescriptionInit }
  | { type: "answer"; from: "client" | "browser"; data: RTCSessionDescriptionInit }
  | { type: "ice";    from: "client" | "browser"; data: RTCIceCandidateInit | null }
  | { type: "bye";    from: "client" | "browser"; data?: undefined }
  | { type: "request_renegotiate"; from: "client" | "browser"; data?: null }
  | { type: "probe_result"; from: "client" | "browser"; data: ProbeResult };

export type SessionStatus = "idle" | "connecting" | "connected" | "failed" | "closed";

export type SessionLogLevel = "info" | "ok" | "warn" | "err";

// One subscription surface instead of a dozen onX options. `pcCreated`
// fires for the initial pc AND every rebuild — callers that hold a pc
// reference (passthrough, e2e hooks) MUST re-arm on every emission.
export interface SessionEvents {
  /** Human-oriented trace, same stream main.ts used to render its log pane. */
  log: (level: SessionLogLevel, msg: string, extra?: unknown) => void;
  /** Coarse UI state. `text` is a short human hint ("waiting for offer"). */
  status: (status: SessionStatus, text?: string) => void;
  /** A media track arrived. `stream` is ev.streams[0] or a wrapper. */
  track: (track: MediaStreamTrack, stream: MediaStream) => void;
  /**
   * A data channel arrived that the session does NOT own. "stats" is
   * handled internally; everything else ("input", "cursor", "files",
   * future labels) is forwarded here for the caller to wire.
   */
  dataChannel: (dc: RTCDataChannel) => void;
  /** Fresh RTCPeerConnection (initial connect or post-reconnect rebuild). */
  pcCreated: (pc: RTCPeerConnection) => void;
  /** Per-second stats sample (mirrors what is sent on the stats channel). */
  stats: (sample: StatsSample, prev: StatsSample | undefined) => void;
  /** Result of codec-preference classification of the negotiated answer. */
  codecOutcome: (result: NegotiationResult) => void;
  /** Signaling-transport state (from ReconnectingWebSocket). */
  signaling: (next: ReconnectState, prev: ReconnectState, info: SignalingRetryInfo) => void;
  /** Peer connection state mirrors, for debug panels. */
  connectionState: (state: RTCPeerConnectionState) => void;
  signalingState: (state: RTCSignalingState) => void;
  iceConnectionState: (state: RTCIceConnectionState) => void;
  iceGatheringState: (state: RTCIceGatheringState) => void;
  /** Session ended (bye, fatal negotiation failure, or disconnect()). */
  closed: (reason: string) => void;
}

export interface ChromelessSessionOptions {
  /** ws(s):// base WITHOUT the trailing /{sessionId}. Required. */
  signalingBase: string;
  /**
   * Codec preference, top-first. The first entry is what the answer
   * SDP is munged to lead with, so classifyNegotiation's "ok" outcome
   * aligns with the actual ask. Default matches the demo page.
   */
  codecPreference?: readonly string[];
  /** Stats sampling interval. Default 1000. */
  statsIntervalMs?: number;

  // ---- injection seams (tests / exotic embedders) ----
  /** WebSocket constructor handed to ReconnectingWebSocket. */
  socketCtor?: RWSocketCtor;
  /** RTCPeerConnection factory. Default: `new RTCPeerConnection(cfg)`. */
  pcFactory?: (config: RTCConfiguration) => RTCPeerConnection;
  /** Token fetcher. Default: auth.ts fetchSessionToken. */
  fetchToken?: typeof fetchSessionToken;
  /** ICE/TURN config fetcher. Default: turn.ts fetchTurnConfig. */
  fetchIce?: typeof fetchTurnConfig;
  /** Pre-call probe. Default: probe.ts estimateConnectionQuality. */
  probe?: typeof estimateConnectionQuality;
}

// Internal listener storage. Erased to the loosest function shape;
// on()/emit() are the only writers/readers and both are keyed by the
// SessionEvents map, so the erasure never escapes.
type AnyListener = (...args: unknown[]) => void;
interface Listeners {
  [k: string]: Set<AnyListener>;
}

const DEFAULT_CODEC_PREFERENCE = ["VP9", "AV1", "H264", "VP8"] as const;

export class ChromelessSession {
  private readonly opts: Required<Pick<ChromelessSessionOptions, "signalingBase">> &
    ChromelessSessionOptions;
  private readonly codecPreference: readonly string[];

  private listeners: Listeners = {};

  private rws: ReconnectingWebSocket | null = null;
  private pc: RTCPeerConnection | null = null;
  private iceConfig: RTCConfiguration = {};
  private sessionId = "";
  private tenantId = "";
  private statsDc: RTCDataChannel | null = null;
  private stats: StatsSampler | null = null;
  private detachStats: (() => void) | null = null;
  private hasOpenedOnce = false;
  private probePromise: Promise<ProbeResult | null> | null = null;
  private closedFired = false;

  constructor(options: ChromelessSessionOptions) {
    this.opts = options;
    this.codecPreference = options.codecPreference ?? DEFAULT_CODEC_PREFERENCE;
  }

  // ---- events ----

  on<K extends keyof SessionEvents>(event: K, cb: SessionEvents[K]): () => void {
    (this.listeners[event] ??= new Set()).add(cb as unknown as AnyListener);
    return () => this.listeners[event]?.delete(cb as unknown as AnyListener);
  }

  private emit<K extends keyof SessionEvents>(
    event: K,
    ...args: Parameters<SessionEvents[K]>
  ): void {
    for (const cb of this.listeners[event] ?? []) {
      try {
        cb(...args);
      } catch {
        /* subscriber errors never break the machine */
      }
    }
  }

  private log(level: SessionLogLevel, msg: string, extra?: unknown): void {
    this.emit("log", level, msg, extra);
  }

  // ---- public surface ----

  /** The live peer connection, or null before connect / after close. */
  getPeerConnection(): RTCPeerConnection | null {
    return this.pc;
  }

  /** Session id passed to connect(); "" before connect. */
  getSessionId(): string {
    return this.sessionId;
  }

  /** Tenant id from the verified token's `sub`; "" if anonymous. */
  getTenantId(): string {
    return this.tenantId;
  }

  isConnected(): boolean {
    return this.pc?.connectionState === "connected";
  }

  /**
   * Ask the offerer for a fresh offer (e.g. after the caller added
   * tracks that need new m= sections). Returns false if signaling is
   * not currently open.
   */
  requestRenegotiate(reason: string): boolean {
    if (!this.rws?.isConnected()) return false;
    try {
      this.rws.send(JSON.stringify({
        type: "request_renegotiate", from: "client",
      } satisfies Envelope));
      this.log("info", `→ request_renegotiate (${reason})`);
      return true;
    } catch (err) {
      this.log("warn", "request_renegotiate send failed", String(err));
      return false;
    }
  }

  /**
   * Dial signaling and run the answerer machine until disconnect() or
   * a fatal condition. Resolves once the dial is initiated (NOT once
   * media flows — subscribe to "status"/"track" for that).
   */
  async connect(sessionId: string): Promise<void> {
    if (this.rws) throw new Error("ChromelessSession: already connected");
    this.closedFired = false;
    this.sessionId = sessionId;
    this.emit("status", "connecting", "ws://");

    const base = this.opts.signalingBase;
    let wsUrl = `${base}/${encodeURIComponent(sessionId)}`;

    // T48 auth: null issuer ⇒ connect unauthenticated; the signaling
    // server decides whether that is acceptable.
    const fetchToken = this.opts.fetchToken ?? fetchSessionToken;
    let issued: IssuedToken | null = null;
    try {
      issued = await fetchToken(sessionId, "client", { signalingBase: base });
    } catch {
      issued = null;
    }
    if (issued) {
      this.log("info", "auth token", { exp: issued.exp, sub: issued.sub });
      wsUrl = withToken(wsUrl, issued.token);
      this.tenantId = issued.sub ?? "";
    } else {
      this.log("info", "no auth token (issuer unavailable; connecting unauthenticated)");
      this.tenantId = "";
    }
    this.log("info", "dialing signaling", wsUrl.replace(/token=[^&]+/, "token=…"));

    // ICE config once per session; reused across reconnects. If TURN
    // credentials rotate mid-session this is the call site that grows
    // a refresh (same note as the pre-split code).
    const fetchIce = this.opts.fetchIce ?? fetchTurnConfig;
    this.iceConfig = await fetchIce(base);
    this.log("info", "ice config", this.iceConfig);

    // T102 pre-call probe, raced against the WS dial; result shipped
    // after the hello so it lands on a registered session.
    const probe = this.opts.probe ?? estimateConnectionQuality;
    this.probePromise = probe({
      signalingBase: base,
      ...(issued?.token ? { authToken: issued.token } : {}),
    }).catch(() => null);

    const rws = this.opts.socketCtor
      ? new ReconnectingWebSocket(wsUrl, { webSocket: this.opts.socketCtor })
      : new ReconnectingWebSocket(wsUrl);
    this.rws = rws;
    this.hasOpenedOnce = false;

    this.pc = this.buildPeerConnection();

    rws.on("stateChange", (next, prev, info) => {
      this.emit("signaling", next, prev, info);
      this.log("info", `signaling ${prev}→${next}`,
        info.attempt > 0 ? { attempt: info.attempt, retryInMs: info.nextDelayMs } : undefined);
      if (next === "reconnecting") this.emit("status", "connecting", `reconnecting (attempt ${info.attempt})`);
      else if (next === "failed") this.emit("status", "failed", "signaling failed");
    });

    rws.on("open", () => {
      if (!this.rws) return;
      if (this.hasOpenedOnce) {
        this.log("ok", "ws reopened — rebuilding peer connection");
        this.emit("status", "connecting", "renegotiating");
        this.rebuildPeerConnection("ws reconnected");
      } else {
        this.hasOpenedOnce = true;
        this.log("ok", "ws open");
        this.emit("status", "connecting", "waiting for offer");
      }
      // Hello frame so signaling learns our role.
      rws.send(JSON.stringify({ type: "ice", from: "client", data: null } satisfies Envelope));

      void this.probePromise?.then((p) => {
        if (!p) {
          this.log("info", "probe: no result (skipping probe_result envelope)");
          return;
        }
        this.log("ok", "probe", p);
        rws.send(JSON.stringify({ type: "probe_result", from: "client", data: p } satisfies Envelope));
      });
    });

    rws.on("underlyingClose", (ev) => {
      this.log(ev.wasClean ? "info" : "warn", "ws closed",
        { code: ev.code, reason: ev.reason || "(none)" });
      // No pc teardown here: if signaling reconnects inside the backoff
      // window, ICE/DTLS may still be flowing; rebuild happens on the
      // next "open".
    });

    rws.on("message", (ev: MessageEvent) => {
      void this.handleMessage(ev);
    });

    rws.connect();
  }

  /** Tear the session down. Safe to call repeatedly. */
  disconnect(reason: string): void {
    this.teardown(reason);
  }

  // ---- internals ----

  private async handleMessage(ev: MessageEvent): Promise<void> {
    const pc = this.pc;
    const rws = this.rws;
    if (!pc || !rws) return;
    let env: Envelope;
    try {
      env = JSON.parse(
        typeof ev.data === "string" ? ev.data : await (ev.data as Blob).text());
    } catch (err) {
      this.log("warn", "non-JSON ws frame", String(err));
      return;
    }
    if (env.from === "client") {
      this.log("warn", "echo from self?", env.type);
      return;
    }
    switch (env.type) {
      case "offer": {
        this.log("ok", "← offer", { sdpBytes: env.data.sdp?.length ?? 0 });
        try {
          await pc.setRemoteDescription(env.data);
          const answer = await pc.createAnswer();
          const lead = this.codecPreference[0] ?? "VP9";
          const mungedSdp = prioritizeCodec(answer.sdp ?? "", lead);
          await pc.setLocalDescription({ type: answer.type, sdp: mungedSdp });
          const reply: Envelope = {
            type: "answer", from: "client",
            data: { type: answer.type, sdp: mungedSdp },
          };
          rws.send(JSON.stringify(reply));
          this.log("ok", "→ answer", { sdpBytes: mungedSdp.length });

          // Classify what actually got negotiated; the local
          // description IS the negotiated answer.
          const result = classifyNegotiation(mungedSdp, [...this.codecPreference]);
          const lvl: SessionLogLevel =
            result.outcome === "ok" ? "ok" :
            result.outcome === "fallback" ? "warn" : "err";
          this.log(lvl, describeOutcome(result), { videoCodecs: result.videoCodecs });
          this.emit("codecOutcome", result);
          if (result.outcome === "no_codec") {
            this.teardown("no video codec negotiated");
            return;
          }
          if (result.outcome === "fallback" && this.statsDc?.readyState === "open") {
            try {
              this.statsDc.send(JSON.stringify({
                v: STATS_PROTOCOL_VERSION,
                t: Date.now(),
                session_id: this.sessionId,
                tenant_id: this.tenantId || "",
                event: "codec_fallback",
                data: {
                  preferred: result.preferred,
                  negotiated: result.negotiated,
                  video_codecs: result.videoCodecs,
                },
              }));
            } catch { /* ignore */ }
          }
        } catch (err) {
          this.log("err", "answer pipeline failed", String(err));
          this.teardown("answer failed");
        }
        break;
      }
      case "answer":
        this.log("warn", `← unexpected answer from ${env.from}`);
        break;
      case "ice":
        if (env.data === null) {
          this.log("info", "← ice (end of candidates)");
          return;
        }
        try {
          await pc.addIceCandidate(env.data);
          this.log("info", "← ice", env.data.candidate ?? "");
        } catch (err) {
          this.log("warn", "addIceCandidate failed", String(err));
        }
        break;
      case "bye":
        this.log("info", `← bye from ${env.from}`);
        this.teardown("peer said bye");
        break;
      case "request_renegotiate":
        // We are the answerer; we cannot initiate. The offerer's own
        // ICE-restart machinery re-offers and the offer handler above
        // picks it up.
        this.log("info", `← request_renegotiate from ${env.from} (no-op for answerer; awaiting fresh offer)`);
        break;
      case "probe_result":
        this.log("info", `← probe_result from ${env.from} (ignored on client)`);
        break;
    }
  }

  private buildPeerConnection(): RTCPeerConnection {
    const rws = this.rws;
    if (!rws) throw new Error("buildPeerConnection: no signaling channel");
    const factory = this.opts.pcFactory ?? ((cfg: RTCConfiguration) => new RTCPeerConnection(cfg));
    const pc = factory(this.iceConfig);

    // T108 — seed every state mirror from the fresh PC. The onX
    // handlers fire only on TRANSITIONS; without seeding, a PC stuck
    // at "new" is indistinguishable from one that never built.
    this.emit("connectionState", pc.connectionState);
    this.emit("signalingState", pc.signalingState);
    this.emit("iceConnectionState", pc.iceConnectionState);
    this.emit("iceGatheringState", pc.iceGatheringState);
    this.log("info", "pc constructed", {
      connectionState: pc.connectionState,
      signalingState: pc.signalingState,
      iceServers: this.iceConfig.iceServers?.length ?? 0,
    });

    pc.onsignalingstatechange = () => {
      this.emit("signalingState", pc.signalingState);
      this.log("info", `signalingState=${pc.signalingState}`);
    };
    pc.oniceconnectionstatechange = () => {
      this.emit("iceConnectionState", pc.iceConnectionState);
      const lvl: SessionLogLevel = pc.iceConnectionState === "failed" ? "err" : "info";
      this.log(lvl, `iceConnectionState=${pc.iceConnectionState}`);
      if (pc.iceConnectionState === "failed") {
        // T37 ICE recovery — ask the offerer to redo with iceRestart.
        const sent = requestIceRecovery((f) => rws.send(f), "client");
        this.log(sent ? "info" : "warn",
          sent ? "→ request_renegotiate" : "request_renegotiate dropped (ws not open)");
      }
    };
    pc.onicegatheringstatechange = () => {
      this.emit("iceGatheringState", pc.iceGatheringState);
    };

    // T42 stats sampler, bound to this pc generation. Created before
    // the connectionstate handler so start/stop can reference it.
    const stats = new StatsSampler(pc, {
      intervalMs: this.opts.statsIntervalMs ?? 1000,
      sessionId: this.sessionId,
      tenantId: this.tenantId,
    });
    let prev: StatsSample | undefined;
    const detachStats = stats.on((s) => {
      this.emit("stats", s, prev);
      prev = s;
      if (this.statsDc?.readyState === "open") {
        try {
          this.statsDc.send(JSON.stringify(stats.buildEnvelope(s)));
        } catch { /* ignore */ }
      }
    });
    this.stats = stats;
    this.detachStats = detachStats;

    pc.onconnectionstatechange = () => {
      this.emit("connectionState", pc.connectionState);
      if (pc.connectionState === "connected") this.emit("status", "connected");
      else if (pc.connectionState === "failed") this.emit("status", "failed");
      else if (pc.connectionState === "disconnected" || pc.connectionState === "closed") {
        this.emit("status", "closed");
      }
      this.log(pc.connectionState === "failed" ? "err" : "info",
        `connectionState=${pc.connectionState}`);
      if (pc.connectionState === "connected") stats.start();
      else if (pc.connectionState === "closed" || pc.connectionState === "failed") stats.stop();
    };

    pc.onicecandidate = (ev) => {
      const env: Envelope = {
        type: "ice", from: "client",
        data: ev.candidate ? ev.candidate.toJSON() : null,
      };
      rws.send(JSON.stringify(env));
      if (ev.candidate) this.log("info", "→ ice", ev.candidate.candidate);
      else this.log("info", "→ ice (end of candidates)");
    };

    pc.ontrack = (ev) => {
      this.log("ok", "← track", { kind: ev.track.kind, id: ev.track.id });
      const stream = ev.streams[0] ?? new MediaStream([ev.track]);
      this.emit("track", ev.track, stream);
    };

    pc.ondatachannel = (ev) => {
      const dc = ev.channel;
      this.log("ok", `← data channel "${dc.label}" (state=${dc.readyState})`);
      if (dc.label === "stats") {
        // Ours: outbound per-second samples once open.
        this.statsDc = dc;
        return;
      }
      this.emit("dataChannel", dc);
    };

    this.emit("pcCreated", pc);
    return pc;
  }

  private rebuildPeerConnection(reason: string): void {
    if (!this.pc) return;
    this.log("info", `rebuilding peer connection: ${reason}`);
    try { this.detachStats?.(); } catch { /* ignore */ }
    try { this.stats?.stop(); } catch { /* ignore */ }
    this.detachStats = null;
    this.stats = null;
    try { this.statsDc?.close(); } catch { /* ignore */ }
    this.statsDc = null;
    try { this.pc.close(); } catch { /* ignore */ }
    this.pc = this.buildPeerConnection();
  }

  private teardown(reason: string): void {
    if (!this.rws && !this.pc) return;
    this.log("info", `tearing down: ${reason}`);
    try { this.detachStats?.(); } catch { /* ignore */ }
    try { this.stats?.stop(); } catch { /* ignore */ }
    try { this.statsDc?.close(); } catch { /* ignore */ }
    try { this.pc?.close(); } catch { /* ignore */ }
    if (this.rws?.isConnected()) {
      try {
        this.rws.send(JSON.stringify({ type: "bye", from: "client" } satisfies Envelope));
      } catch { /* ignore */ }
    }
    try { this.rws?.close(); } catch { /* ignore */ }
    this.rws = null;
    this.pc = null;
    this.stats = null;
    this.detachStats = null;
    this.statsDc = null;
    this.emit("status", "closed");
    if (!this.closedFired) {
      this.closedFired = true;
      this.emit("closed", reason);
    }
  }
}

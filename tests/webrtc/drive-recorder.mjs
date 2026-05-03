#!/usr/bin/env node
/**
 * tests/webrtc/drive-recorder.mjs — WebRTC harness driver.
 *
 * Connects to cb-chromium via Chrome DevTools Protocol, opens a fresh
 * BrowserContext + target, navigates that target to a self-hosted
 * streamer-page.html, then plays the role of WebRTC counter-peer in
 * Node.js using @roamhq/wrtc. Records the inbound video stream to
 * a .webm via ffmpeg, then queries getStats() from both sides and
 * (when encoder-assertions.mjs is present) asserts that the encoder
 * implementation_name matches our embedded encoders, NOT chromium's
 * stock libvpx/OpenH264.
 *
 * See ../../.. (chromeless repo root)/.. (or this team's plan
 * fizzy-beaming-shamir.md) for the full architecture rationale.
 *
 * CLI:
 *   --cb-url=<url>     Base URL of the cb-chromium DevTools endpoint.
 *                      Default: http://cb-browserless.triform-wtf.svc.cluster.local:9222
 *   --duration=<sec>   How long to record after first frame. Default 5.
 *   --out=<path>       Output .webm path. Default ./out.webm.
 *   --codec=<name>     Codec to pin via setCodecPreferences (vp9|vp8|h264|av1).
 *                      Default: don't pin (browser default order).
 *   --listen-host=<h>  Host the harness's tiny HTTP server binds.
 *                      Default 127.0.0.1.
 *   --listen-port=<p>  Port. Default 0 (ephemeral).
 *   --keep-page-open   Don't close the chromium page on exit. Useful
 *                      when iterating manually via DevTools.
 *
 * Exit codes:
 *   0  success — recording written, encoder assertions (if available) passed.
 *   1  any handshake or recording failure.
 */

import http from "node:http";
import path from "node:path";
import fs from "node:fs";
import { fileURLToPath } from "node:url";
import { spawn } from "node:child_process";
import process from "node:process";

import CDP from "chrome-remote-interface";
import wrtc from "@roamhq/wrtc";

const { RTCPeerConnection, nonstandard } = wrtc;
const { RTCVideoSink } = nonstandard;

// ---------- arg parsing -----------------------------------------------------

function parseArgs(argv) {
  const out = {
    cbUrl: "http://cb-browserless.triform-wtf.svc.cluster.local:9222",
    duration: 5,
    outFile: path.resolve(process.cwd(), "out.webm"),
    codec: "",
    listenHost: "127.0.0.1",
    listenPort: 0,
    keepPageOpen: false,
  };
  for (const a of argv) {
    if (a.startsWith("--cb-url=")) out.cbUrl = a.slice("--cb-url=".length);
    else if (a.startsWith("--duration=")) out.duration = Number(a.slice("--duration=".length));
    else if (a.startsWith("--out=")) out.outFile = path.resolve(a.slice("--out=".length));
    else if (a.startsWith("--codec=")) out.codec = a.slice("--codec=".length);
    else if (a.startsWith("--listen-host=")) out.listenHost = a.slice("--listen-host=".length);
    else if (a.startsWith("--listen-port=")) out.listenPort = Number(a.slice("--listen-port=".length));
    else if (a === "--keep-page-open") out.keepPageOpen = true;
    else if (a === "-h" || a === "--help") { printHelp(); process.exit(0); }
    else { console.error(`unknown arg: ${a}`); printHelp(); process.exit(2); }
  }
  if (!Number.isFinite(out.duration) || out.duration <= 0) {
    console.error("--duration must be a positive number of seconds");
    process.exit(2);
  }
  return out;
}

function printHelp() {
  process.stderr.write(`Usage: node drive-recorder.mjs [options]

Options:
  --cb-url=<url>      cb-chromium /json/version base. Default cluster DNS.
  --duration=<sec>    Recording length in seconds. Default 5.
  --out=<path>        Output .webm path. Default ./out.webm.
  --codec=<name>      Pin codec (vp9|vp8|h264|av1). Default: browser order.
  --listen-host=<h>   Local HTTP server bind host. Default 127.0.0.1.
  --listen-port=<p>   Local HTTP server bind port. Default 0 (ephemeral).
  --keep-page-open    Leave the chromium page open on exit (debugging).
  -h, --help          Show this help.
`);
}

const args = parseArgs(process.argv.slice(2));
const HERE = path.dirname(fileURLToPath(import.meta.url));

// ---------- log helpers ----------------------------------------------------

const startedAt = Date.now();
function ts() {
  const ms = Date.now() - startedAt;
  return `+${(ms / 1000).toFixed(3)}s`;
}
function log(level, msg, extra) {
  const tail = extra === undefined
    ? ""
    : "  " + (typeof extra === "string" ? extra : JSON.stringify(extra));
  process.stderr.write(`[harness ${ts()} ${level}] ${msg}${tail}\n`);
}

// ---------- optional encoder assertions ------------------------------------
//
// encoder-assertion-author owns ./encoder-assertions.mjs in parallel.
// We import it dynamically so this file is runnable even if the parallel
// branch hasn't merged yet. When absent we log a soft warning and skip
// the assertions (recording-only mode).

async function loadEncoderAssertions() {
  try {
    const mod = await import("./encoder-assertions.mjs");
    log("info", "encoder-assertions.mjs loaded",
        { exports: Object.keys(mod) });
    return mod;
  } catch (err) {
    if (err && err.code === "ERR_MODULE_NOT_FOUND") {
      log("warn", "encoder-assertions.mjs not present — recording-only mode");
      return null;
    }
    throw err;
  }
}

// ---------- tiny static HTTP server ----------------------------------------
//
// Serves streamer-page.html and fixtures/ from the same origin so
// getDisplayMedia/getUserMedia in the page inherits one permission grant.
// The harness doesn't need to handle anything beyond GET on those two paths
// — keeping it minimal also keeps the test offline-deterministic.

function startStaticServer(host, port) {
  return new Promise((resolve, reject) => {
    const server = http.createServer((req, res) => {
      try {
        const reqUrl = new URL(req.url, `http://${host}:${port || 0}/`);
        let rel = decodeURIComponent(reqUrl.pathname);
        if (rel === "/" || rel === "/index.html") rel = "/streamer-page.html";
        // path.join + a startsWith check prevents traversal.
        const target = path.normalize(path.join(HERE, rel.replace(/^\//, "")));
        if (!target.startsWith(HERE)) {
          res.writeHead(403); res.end("forbidden"); return;
        }
        if (!fs.existsSync(target) || fs.statSync(target).isDirectory()) {
          res.writeHead(404); res.end(`not found: ${rel}`); return;
        }
        const ext = path.extname(target).toLowerCase();
        const ct = ext === ".html" ? "text/html; charset=utf-8"
                 : ext === ".js"   ? "application/javascript; charset=utf-8"
                 : ext === ".mjs"  ? "application/javascript; charset=utf-8"
                 : "application/octet-stream";
        res.writeHead(200, {
          "content-type": ct,
          // Disable caching so iterating on the streamer page during dev
          // doesn't get a stale body when the harness re-runs.
          "cache-control": "no-store",
        });
        fs.createReadStream(target).pipe(res);
      } catch (err) {
        res.writeHead(500); res.end(String(err));
      }
    });
    server.on("error", reject);
    server.listen(port, host, () => {
      const addr = server.address();
      log("info", "static server listening",
          { host: addr.address, port: addr.port, root: HERE });
      resolve({ server, host: addr.address, port: addr.port });
    });
  });
}

// ---------- CDP helpers ----------------------------------------------------
//
// Mirrors the chain we proved manually tonight (see
// /tmp/test-create-browser-context.py): /json/version → browser-level
// websocket → Target.createBrowserContext → Target.createTarget →
// Target.attachToTarget. The flatten:true bit is essential — it gives
// us per-session message routing on a single browser-level connection,
// which chrome-remote-interface natively supports via the `flatten`
// option on .send().

async function attachToFreshTarget(cbUrl, navUrl) {
  // The cb-chromium binary refuses /json/version requests whose Host
  // header isn't loopback (DNS-rebinding mitigation in DevTools). The
  // Python test forces Host: localhost; we do the same by using the
  // chrome-remote-interface low-level API (CDP.Version) which lets us
  // override headers. CRI's host/port discovery uses fetch under the
  // hood; override its target Host with the embedded option below.
  //
  // Easier: do the /json/version dance ourselves and feed the
  // resolved webSocketDebuggerUrl directly to CDP({target}).
  const u = new URL(cbUrl);
  const versionUrl = new URL("/json/version", u).toString();
  log("info", "fetching /json/version", { url: versionUrl });
  const ver = await fetchJsonWithHostOverride(versionUrl, "localhost");
  let wsUrl = ver.webSocketDebuggerUrl;
  if (!wsUrl) throw new Error("no webSocketDebuggerUrl in /json/version response");
  // chromium replaces the host with "localhost" — rewrite back to the
  // service we actually reached so the websocket dial routes correctly.
  // u.host already contains host[:port].
  wsUrl = wsUrl.replace(/ws:\/\/[^/]+/, `ws://${u.host}`);
  log("info", "browser ws", { wsUrl });

  const browser = await CDP({ target: wsUrl });
  log("ok", "browser CDP attached");

  // Fresh browser context — equivalent to an incognito profile slot;
  // gives us isolated cookies/storage per harness run.
  const { browserContextId } = await browser.Target.createBrowserContext({});
  log("ok", "createBrowserContext", { browserContextId });

  const { targetId } = await browser.Target.createTarget({
    url: "about:blank",
    browserContextId,
  });
  log("ok", "createTarget", { targetId });

  const { sessionId } = await browser.Target.attachToTarget({
    targetId,
    flatten: true,
  });
  log("ok", "attachToTarget", { sessionId });

  // Bind a thin per-session client. With flatten:true the raw browser
  // connection multiplexes; chrome-remote-interface gives us a child
  // namespace via .session(sessionId) (CRI 0.33+).
  const session = await browser.session(sessionId);
  await session.Page.enable();
  await session.Runtime.enable();

  // Navigate to the streamer page now that listeners are wired.
  log("info", "Page.navigate", { navUrl });
  await session.Page.navigate({ url: navUrl });

  return {
    browser,
    session,
    sessionId,
    targetId,
    browserContextId,
    async dispose() {
      try { await browser.Target.closeTarget({ targetId }); } catch { /* ignore */ }
      try { await browser.Target.disposeBrowserContext({ browserContextId }); } catch { /* ignore */ }
      try { await browser.close(); } catch { /* ignore */ }
    },
  };
}

function fetchJsonWithHostOverride(urlStr, hostHeader) {
  return new Promise((resolve, reject) => {
    const u = new URL(urlStr);
    const req = http.request({
      hostname: u.hostname,
      port: u.port || 80,
      path: u.pathname + u.search,
      method: "GET",
      headers: { Host: hostHeader, Accept: "application/json" },
    }, (res) => {
      const chunks = [];
      res.on("data", (c) => chunks.push(c));
      res.on("end", () => {
        try { resolve(JSON.parse(Buffer.concat(chunks).toString("utf8"))); }
        catch (err) { reject(err); }
      });
    });
    req.on("error", reject);
    req.setTimeout(10_000, () => req.destroy(new Error("timeout fetching /json/version")));
    req.end();
  });
}

// ---------- in-page bridge -------------------------------------------------
//
// Outbound (page → harness): the page emits `console.log("CBTEST:" + JSON)`
// which CDP relays via Runtime.consoleAPICalled. We parse the prefix.
//
// Inbound (harness → page): we Runtime.evaluate window.__cbtest.handle({...})
// with awaitPromise so failures surface as CDP errors.

function wireConsoleBridge(session, onMessage) {
  session.Runtime.consoleAPICalled((evt) => {
    const args = evt.args || [];
    if (args.length === 0) return;
    const first = args[0];
    if (first?.type !== "string" || typeof first.value !== "string") return;
    const v = first.value;
    if (!v.startsWith("CBTEST:")) {
      // Optional: pipe non-CBTEST log lines through for human debugging.
      // Comment out to silence:
      const tail = args.slice(1).map((a) => a.value ?? a.description ?? "").join(" ");
      log("page", `[${evt.type}] ${v} ${tail}`.trimEnd());
      return;
    }
    let msg;
    try { msg = JSON.parse(v.slice("CBTEST:".length)); }
    catch (err) { log("warn", "bad CBTEST payload", { v, err: String(err) }); return; }
    onMessage(msg);
  });
}

async function pushInbound(session, msg) {
  const expr = `window.__cbtest.handle(${JSON.stringify(msg)})`;
  const r = await session.Runtime.evaluate({
    expression: expr,
    awaitPromise: true,
    returnByValue: true,
  });
  if (r.exceptionDetails) {
    throw new Error("inbound failed: " + JSON.stringify(r.exceptionDetails));
  }
  return r.result?.value;
}

// ---------- ffmpeg recorder ------------------------------------------------
//
// We stash a fixed set of env-tweaks here so a missing ffmpeg surfaces
// loudly instead of silently producing a 0-byte file.

function spawnFfmpeg({ width, height, fps, outFile, codec }) {
  // codec coming from the page (vp9|vp8|h264|av1) controls our
  // ENCODER on the wire. The .webm container gates its allowed
  // codecs. We default to libvpx-vp9 because:
  //   - .webm + VP9 is a universally inspectable combination
  //     (mpv, QuickTime, ffprobe, every browser).
  //   - The SOURCE video stream we receive from cb-chromium gets
  //     decoded by libwebrtc and handed to us as raw I420 frames
  //     before we re-encode for storage; so the storage codec is
  //     independent of the wire codec.
  // If the user passes --codec=h264 we still write VP9 to the .webm
  // (the wire-codec assertion happens via getStats(), not via
  // ffprobe). A future option could offer storageCodec= for
  // diagnostic .mp4 output.
  const ffArgs = [
    "-loglevel", "warning",
    "-f", "rawvideo",
    "-pix_fmt", "yuv420p",
    "-s", `${width}x${height}`,
    "-r", String(fps),
    "-i", "pipe:0",
    "-c:v", "libvpx-vp9",
    "-deadline", "realtime",
    "-cpu-used", "8",
    "-y", outFile,
  ];
  log("info", "spawn ffmpeg", { argv: ffArgs.join(" ") });
  const proc = spawn("ffmpeg", ffArgs, { stdio: ["pipe", "ignore", "inherit"] });
  proc.on("error", (err) => {
    log("err", "ffmpeg spawn failed — is ffmpeg on PATH?", String(err));
  });
  return proc;
}

// ---------- main flow ------------------------------------------------------

async function main() {
  log("info", "harness start", { args });

  const encoderAssertions = await loadEncoderAssertions();

  // 1. Static server (page + fixture).
  const { server, host, port } = await startStaticServer(args.listenHost, args.listenPort);
  const navUrl = `http://${host}:${port}/streamer-page.html` +
    (args.codec ? `?codec=${encodeURIComponent(args.codec)}` : "");

  // 2. CDP attach + navigate.
  const cb = await attachToFreshTarget(args.cbUrl, navUrl);

  // 3. Node.js peer.
  const peer = new RTCPeerConnection({});
  let videoSink = null;
  let firstFrameAt = null;
  let frameCount = 0;
  let frameWidth = 0;
  let frameHeight = 0;
  let ffmpeg = null;

  // The peer fires `ontrack` when the inbound (chromium → us) media
  // track becomes available. From there we attach an RTCVideoSink and
  // pipe the raw I420 frames to ffmpeg's stdin.
  peer.ontrack = (ev) => {
    const track = ev.track;
    log("ok", "peer ontrack", { kind: track.kind, id: track.id });
    if (track.kind !== "video") return;
    videoSink = new RTCVideoSink(track);
    videoSink.onframe = ({ frame }) => {
      // node-webrtc's frame format: { width, height, data, rotation }
      // where data is a Uint8Array containing YUV planes concatenated
      // (Y, U, V, sizes per the I420 layout).
      frameCount += 1;
      if (firstFrameAt === null) {
        firstFrameAt = Date.now();
        frameWidth = frame.width;
        frameHeight = frame.height;
        log("ok", "first frame", { width: frameWidth, height: frameHeight });
        ffmpeg = spawnFfmpeg({
          width: frameWidth,
          height: frameHeight,
          fps: 30, // wire frame-rate target (best-effort; ffmpeg accepts variable)
          outFile: args.outFile,
          codec: args.codec,
        });
      }
      if (ffmpeg && !ffmpeg.killed && ffmpeg.stdin && !ffmpeg.stdin.destroyed) {
        try { ffmpeg.stdin.write(Buffer.from(frame.data)); }
        catch (err) { log("warn", "ffmpeg stdin write failed", String(err)); }
      }
    };
  };
  peer.onicecandidate = async (ev) => {
    if (!ev.candidate) {
      try { await pushInbound(cb.session, { type: "ice", candidate: null }); }
      catch (err) { log("warn", "push end-of-candidates failed", String(err)); }
      return;
    }
    try {
      await pushInbound(cb.session, {
        type: "ice",
        candidate: ev.candidate.toJSON(),
      });
    } catch (err) {
      log("warn", "push ice failed", String(err));
    }
  };
  peer.oniceconnectionstatechange = () => {
    log("info", `peer iceConnectionState=${peer.iceConnectionState}`);
  };
  peer.onconnectionstatechange = () => {
    log("info", `peer connectionState=${peer.connectionState}`);
  };

  // 4. Wire the page → harness console bridge BEFORE the page sends
  //    its offer. consoleAPICalled events arrive only for messages
  //    emitted while a subscription is active.
  let resolveOffer;
  const offerReady = new Promise((resolve) => { resolveOffer = resolve; });
  let pageReady = false;
  let fatalReason = null;

  wireConsoleBridge(cb.session, (msg) => {
    switch (msg.type) {
      case "sdp-offer":
        log("ok", "← sdp-offer", { sdpBytes: (msg.sdp || "").length });
        resolveOffer(msg.sdp);
        break;
      case "ice":
        // Apply remote candidate to our peer. addIceCandidate(null) is
        // the "no more candidates" sentinel; @roamhq/wrtc accepts the
        // standard {candidate:""} form too.
        if (msg.candidate === null) {
          log("info", "← page end-of-candidates");
          peer.addIceCandidate(null).catch((err) => {
            log("warn", "addIceCandidate(null) failed", String(err));
          });
        } else {
          peer.addIceCandidate(msg.candidate).catch((err) => {
            log("warn", "addIceCandidate failed", String(err));
          });
        }
        break;
      case "ice-state":
      case "pc-state":
        // Just informational; already logged page-side.
        break;
      case "ready":
        pageReady = true;
        break;
      case "fatal":
        fatalReason = msg.reason || "unknown";
        log("err", "page fatal", msg);
        break;
      default:
        log("warn", "unknown CBTEST msg type", msg);
    }
  });

  // Add a video transceiver in recvonly mode so the SDP answer accepts
  // the page's video track. @roamhq/wrtc supports addTransceiver; the
  // alternative is to wait for ontrack from setRemoteDescription which
  // also works but requires the m-section to match.
  peer.addTransceiver("video", { direction: "recvonly" });

  // 5. Wait for offer → answer → setLocalDescription on our side.
  log("info", "waiting for sdp-offer from page…");
  const offerSdp = await Promise.race([
    offerReady,
    timeoutErr(30_000, "sdp-offer never arrived"),
  ]);
  await peer.setRemoteDescription({ type: "offer", sdp: offerSdp });
  const answer = await peer.createAnswer();
  await peer.setLocalDescription(answer);
  await pushInbound(cb.session, { type: "sdp-answer", sdp: answer.sdp });
  log("ok", "→ sdp-answer pushed", { sdpBytes: answer.sdp?.length ?? 0 });

  // 6. Wait for the first frame (proves the wire actually carried
  //    encoded media, not just an SDP handshake on paper).
  await waitFor(() => firstFrameAt !== null, 20_000, "no frames received from page");

  // 6a. If encoder-assertions exposes pollStatsUntilEncoded, defer
  //     to it for the per-codec dwell logic (it polls until
  //     outboundRtp.encoderImplementation populates — chromium omits
  //     the field until the first frame actually encodes). Otherwise
  //     just hold the fort for --duration seconds.
  let pinnedEncoder = null;
  if (encoderAssertions?.pollStatsUntilEncoded) {
    try {
      pinnedEncoder = await encoderAssertions.pollStatsUntilEncoded(
        cb.session, "window.pc", 8_000);
      log("ok", "pollStatsUntilEncoded resolved", { encoder: pinnedEncoder });
    } catch (err) {
      log("warn", "pollStatsUntilEncoded failed (non-fatal)", String(err));
    }
  }

  // 7. Record for --duration seconds beyond first frame.
  log("info", `recording for ${args.duration}s…`);
  await new Promise((r) => setTimeout(r, args.duration * 1000));

  // 8. Pull stats from BOTH sides — the chromium sender's stats are
  //    where encoderImplementation lives (only meaningful after at
  //    least one frame has encoded, which we guaranteed above).
  const senderStats = await readSenderStatsViaCDP(cb.session);
  log("info", "sender getStats(): outboundRtp.video summary",
      summarizeOutboundRtp(senderStats));

  const recvStatsRaw = await peer.getStats();
  const recvStats = mapToObj(recvStatsRaw);
  log("info", "receiver getStats(): inboundRtp.video summary",
      summarizeInboundRtp(recvStats));

  // 9. Encoder-identity assertion (delegated, optional). The module
  //    expects (statsOrImplString, expectedCodec). We pass the
  //    encoderImplementation string we already pulled from the sender
  //    summary (or, if poll already resolved one, that — same value).
  //    The assertion is only meaningful when --codec was passed; we
  //    skip otherwise because we have no expected.
  if (!encoderAssertions?.assertEncoderIdentity) {
    log("warn", "skipping encoder identity assertion " +
                "(encoder-assertions.mjs missing assertEncoderIdentity export)");
  } else if (!args.codec) {
    log("warn", "skipping encoder identity assertion " +
                "(no --codec passed; nothing to assert against)");
  } else {
    const summary = summarizeOutboundRtp(senderStats);
    const impl = pinnedEncoder
      || summary.find((s) => s.encoderImplementation)?.encoderImplementation
      || null;
    try {
      encoderAssertions.assertEncoderIdentity(impl, args.codec);
      log("ok", "encoder identity OK", { codec: args.codec, impl });
    } catch (err) {
      log("err", "encoder identity FAILED", String(err));
      throw err;
    }
  }

  // 10. Tear down recording.
  if (videoSink) {
    try { videoSink.stop(); } catch { /* ignore */ }
  }
  if (ffmpeg && !ffmpeg.killed) {
    try { ffmpeg.stdin.end(); } catch { /* ignore */ }
    await new Promise((resolve) => {
      const t = setTimeout(() => { try { ffmpeg.kill("SIGKILL"); } catch { /* ignore */ } resolve(); }, 5_000);
      ffmpeg.on("exit", (code, sig) => {
        clearTimeout(t);
        log("info", "ffmpeg exit", { code, sig });
        resolve();
      });
    });
  }

  // 11. Confirm we wrote something useful.
  let outSize = 0;
  try { outSize = fs.statSync(args.outFile).size; }
  catch { /* file missing */ }
  log("info", "recording", {
    outFile: args.outFile, bytes: outSize,
    frames: frameCount,
    elapsedMs: firstFrameAt ? (Date.now() - firstFrameAt) : 0,
  });
  if (outSize < 1024) {
    throw new Error(`recording too small (${outSize} bytes); ffmpeg likely failed`);
  }
  if (fatalReason) {
    throw new Error(`page reported fatal: ${fatalReason}`);
  }

  // 12. Cleanup chromium.
  try { peer.close(); } catch { /* ignore */ }
  if (!args.keepPageOpen) await cb.dispose();
  server.close();

  log("ok", "harness PASS");
}

// ---------- small helpers --------------------------------------------------

function timeoutErr(ms, reason) {
  return new Promise((_, reject) =>
    setTimeout(() => reject(new Error(`timeout ${ms}ms: ${reason}`)), ms),
  );
}

async function waitFor(pred, ms, reason) {
  const deadline = Date.now() + ms;
  while (Date.now() < deadline) {
    if (pred()) return;
    await new Promise((r) => setTimeout(r, 50));
  }
  throw new Error(`timeout waiting for: ${reason}`);
}

async function readSenderStatsViaCDP(session) {
  // Run inside the page so we read CHROMIUM's getStats (sender side),
  // not the @roamhq/wrtc receiver's. Returning by value flattens the
  // RTCStatsReport (which is a Map) into a plain object.
  const expr = `
    (async () => {
      if (!window.pc) return { __no_pc: true };
      const report = await window.pc.getStats();
      const out = {};
      report.forEach((v, k) => { out[k] = v; });
      return out;
    })()
  `;
  const r = await session.Runtime.evaluate({
    expression: expr,
    awaitPromise: true,
    returnByValue: true,
  });
  if (r.exceptionDetails) {
    throw new Error("sender getStats failed: " + JSON.stringify(r.exceptionDetails));
  }
  return r.result?.value || {};
}

function mapToObj(report) {
  const out = {};
  report.forEach((v, k) => { out[k] = v; });
  return out;
}

function summarizeOutboundRtp(stats) {
  const out = [];
  for (const v of Object.values(stats)) {
    if (v?.type === "outbound-rtp" && v.kind === "video") {
      out.push({
        ssrc: v.ssrc,
        framesEncoded: v.framesEncoded,
        bytesSent: v.bytesSent,
        encoderImplementation: v.encoderImplementation,
        codecId: v.codecId,
      });
    }
  }
  return out;
}

function summarizeInboundRtp(stats) {
  const out = [];
  for (const v of Object.values(stats)) {
    if (v?.type === "inbound-rtp" && v.kind === "video") {
      out.push({
        ssrc: v.ssrc,
        framesDecoded: v.framesDecoded,
        bytesReceived: v.bytesReceived,
        decoderImplementation: v.decoderImplementation,
        codecId: v.codecId,
      });
    }
  }
  return out;
}

// ---------- entry ----------------------------------------------------------

main().then(() => {
  process.exit(0);
}).catch((err) => {
  log("err", "harness FAIL", String(err?.stack || err));
  process.exit(1);
});

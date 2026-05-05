// tests/webrtc/scenarios/_lib_portal.mjs
//
// Runner support for the **portal-encoder** scenarios numbered 30+.
//
// These are NOT WebRTC scenarios — they test the Triform portal's
// input-encoder wire format end-to-end via:
//
//   CDP Input.dispatchMouseEvent (test driver)
//     → DOM mousedown on test-client canvas
//     → encoder.web::from_mouse_event   ◀── PRODUCTION CODE PATH
//     → JSON over WebSocket
//     → stub WS server (this file)
//     → assertions
//
// Wholly separate from `_lib.mjs`'s `runScenario` because that runner
// builds a WebRTC peer + recording pipeline — overkill (and coupling
// risk) for a wire-format test. We reuse the lower-level primitives
// (`makeSessionClient`, `makeInputBag`, `MOD`, `sleep`) from there.
//
// See `portal-test-client/README.md` for the fixture-page contract.

import http from "node:http";
import path from "node:path";
import fs from "node:fs";
import process from "node:process";
import { fileURLToPath } from "node:url";

import CDP from "chrome-remote-interface";
import { WebSocketServer } from "ws";

import { makeSessionClient, makeInputBag, MOD, sleep } from "./_lib.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));

// ---------------------------------------------------------------------------
// Stub WS server — receives the bytes the test client encodes and forwards.
// Records each parsed message + timestamp so scenarios can assert on shape
// AND on ordering (e.g. mousedown precedes mouseup precedes click).
// ---------------------------------------------------------------------------

export function startPortalStubWs(opts = {}) {
  return new Promise((resolve) => {
    const httpServer = http.createServer();
    const wss = new WebSocketServer({ server: httpServer });
    const received = [];
    const errors = [];
    let connections = 0;

    wss.on("connection", (sock) => {
      connections++;
      sock.on("message", (data) => {
        const raw = String(data);
        try {
          received.push({ at: Date.now(), msg: JSON.parse(raw) });
        } catch (e) {
          errors.push({ at: Date.now(), error: String(e), raw });
        }
      });
      sock.on("error", (e) => {
        errors.push({ at: Date.now(), error: "socket: " + String(e) });
      });
    });

    httpServer.on("error", (e) => {
      errors.push({ at: Date.now(), error: "http: " + String(e) });
    });

    httpServer.listen(opts.port ?? 0, "127.0.0.1", () => {
      const { port } = httpServer.address();
      resolve({
        url: `ws://127.0.0.1:${port}/`,
        port,
        received: () => received.slice(),
        errors: () => errors.slice(),
        connections: () => connections,
        clear: () => {
          received.length = 0;
          errors.length = 0;
        },
        close: () =>
          new Promise((r) => {
            try {
              wss.close(() => httpServer.close(() => r()));
            } catch {
              r();
            }
          }),
      });
    });
  });
}

// ---------------------------------------------------------------------------
// Static server — same shape as `_lib.mjs`'s but scoped to the scenarios/
// directory so /portal-test-client/index.html resolves cleanly.
// ---------------------------------------------------------------------------

function startStaticServer(host, port, root) {
  return new Promise((resolve, reject) => {
    const server = http.createServer((req, res) => {
      try {
        const u = new URL(req.url, `http://${host}:${port || 0}/`);
        let rel = decodeURIComponent(u.pathname);
        if (rel === "/") rel = "/index.html";
        const target = path.normalize(path.join(root, rel.replace(/^\//, "")));
        if (!target.startsWith(root)) {
          res.writeHead(403);
          res.end("forbidden");
          return;
        }
        if (!fs.existsSync(target) || !fs.statSync(target).isFile()) {
          res.writeHead(404);
          res.end(`not found: ${rel}`);
          return;
        }
        const ext = path.extname(target).toLowerCase();
        const ct = ext === ".html" ? "text/html; charset=utf-8"
                 : ext === ".js" || ext === ".mjs"
                                  ? "application/javascript; charset=utf-8"
                 : ext === ".wasm" ? "application/wasm"
                 : ext === ".json" ? "application/json; charset=utf-8"
                 : "application/octet-stream";
        res.writeHead(200, { "content-type": ct, "cache-control": "no-store" });
        fs.createReadStream(target).pipe(res);
      } catch (err) {
        res.writeHead(500);
        res.end(String(err));
      }
    });
    server.on("error", reject);
    server.listen(port, host, () => {
      const addr = server.address();
      resolve({ server, host: addr.address, port: addr.port });
    });
  });
}

function fetchJsonWithHostOverride(urlStr, hostHeader) {
  return new Promise((resolve, reject) => {
    const u = new URL(urlStr);
    const req = http.request(
      {
        hostname: u.hostname,
        port: u.port || 80,
        path: u.pathname + u.search,
        method: "GET",
        headers: { Host: hostHeader, Accept: "application/json" },
      },
      (res) => {
        const chunks = [];
        res.on("data", (c) => chunks.push(c));
        res.on("end", () => {
          try {
            resolve(JSON.parse(Buffer.concat(chunks).toString("utf8")));
          } catch (err) {
            reject(err);
          }
        });
      },
    );
    req.on("error", reject);
    req.setTimeout(10_000, () =>
      req.destroy(new Error("timeout fetching /json/version")),
    );
    req.end();
  });
}

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------

function makeLogger(scenarioName) {
  const startedAt = Date.now();
  const ts = () => `+${((Date.now() - startedAt) / 1000).toFixed(3)}s`;
  return (level, msg, extra) => {
    const tail = extra === undefined
      ? ""
      : "  " + (typeof extra === "string" ? extra : JSON.stringify(extra));
    process.stderr.write(`[${scenarioName} ${ts()} ${level}] ${msg}${tail}\n`);
  };
}

// ---------------------------------------------------------------------------
// PortalScenario — minimal Scenario-like surface. No WebRTC, no recording.
// ---------------------------------------------------------------------------

class PortalScenario {
  constructor({ name, cbUrl, artifactsDir }) {
    this.name = name;
    this.cbUrl = cbUrl;
    this.artifactsDir = artifactsDir;
    this.log = makeLogger(name);
    this.assertions = [];
    this._browser = null;
    this._session = null;
    this._targetId = null;
    this._browserContextId = null;
    this._staticServer = null;
    this._stubWs = null;
  }

  async setup() {
    fs.mkdirSync(this.artifactsDir, { recursive: true });

    // Start the stub WS first — we need its URL for the page-load query string.
    this._stubWs = await startPortalStubWs();
    this.log("ok", "stub WS listening", { url: this._stubWs.url });

    // Static server scoped to scenarios/ so /portal-test-client/index.html resolves.
    const sv = await startStaticServer("127.0.0.1", 0, HERE);
    this._staticServer = sv.server;
    const wsParam = encodeURIComponent(this._stubWs.url);
    const navUrl =
      `http://${sv.host}:${sv.port}/portal-test-client/index.html?ws=${wsParam}`;
    this.log("info", "static server", { host: sv.host, port: sv.port, navUrl });

    // CDP attach (same /json/version handshake the WebRTC scenarios use).
    const u = new URL(this.cbUrl);
    const versionUrl = new URL("/json/version", u).toString();
    const ver = await fetchJsonWithHostOverride(versionUrl, "localhost");
    let wsUrl = ver.webSocketDebuggerUrl;
    if (!wsUrl) throw new Error("no webSocketDebuggerUrl in /json/version");
    wsUrl = wsUrl.replace(/ws:\/\/[^/]+/, `ws://${u.host}`);
    this._browser = await CDP({ target: wsUrl, local: true });

    // Per-scenario browser context + fresh target. No about:blank →
    // navigate dance — we boot-load directly to navUrl. Same approach
    // the wire scenarios use for the BUGS-529 workaround; happens to
    // also avoid an unnecessary CDP roundtrip here.
    const ctx = await this._browser.Target.createBrowserContext({});
    this._browserContextId = ctx.browserContextId;
    const tgt = await this._browser.Target.createTarget({
      url: navUrl,
      browserContextId: this._browserContextId,
    });
    this._targetId = tgt.targetId;

    const { sessionId } = await this._browser.Target.attachToTarget({
      targetId: this._targetId,
      flatten: true,
    });
    this._session = makeSessionClient(this._browser, sessionId);
    await this._session.Page.enable();
    await this._session.Runtime.enable();
    this.log("ok", "primary CDP session attached", { sessionId, targetId: this._targetId });

    // Bridge page-side console.log into the harness log — useful when
    // debugging encoder edge cases.
    this._session.Runtime.consoleAPICalled((evt) => {
      const args = evt.args || [];
      if (args.length === 0) return;
      const v = args[0];
      if (v?.type === "string") {
        this.log("page", `[${evt.type}] ${v.value}`);
      }
    });

    // Viewport: 600×400 to match the canvas drawn in index.html. Anything
    // else and CDP click coordinates won't land on the canvas.
    await this._session.Emulation.setDeviceMetricsOverride({
      width: 720,
      height: 480,
      deviceScaleFactor: 1,
      mobile: false,
    });

    // Wait for the page to load + the encoder's WS to reach `open` state.
    const deadline = Date.now() + 10_000;
    let lastState = null;
    while (Date.now() < deadline) {
      try {
        const r = await this._session.Runtime.evaluate({
          expression:
            "JSON.stringify({ ready: document.readyState, ws: window.__cbtest && window.__cbtest.ws_state })",
          returnByValue: true,
        });
        if (r?.result?.value) {
          const state = JSON.parse(r.result.value);
          lastState = state;
          if (state.ready === "complete" && state.ws === "open") break;
        }
      } catch {
        /* tolerate evaluation race during page boot */
      }
      await sleep(100);
    }
    if (!lastState || lastState.ws !== "open") {
      throw new Error(
        `test client never reached ws=open within 10s; last=${JSON.stringify(lastState)}`,
      );
    }
    this.log("ok", "test client encoder WS open", lastState);

    this.user = makeInputBag(this._session, { log: this.log, tag: "user" });
    return this;
  }

  /**
   * Read window.__cbtest as a JS object. Includes the `sent` array
   * (every encoded JSON string) and counters.
   */
  async readCbtest() {
    const r = await this._session.Runtime.evaluate({
      expression: "JSON.stringify(window.__cbtest || null)",
      returnByValue: true,
    });
    return r?.result?.value ? JSON.parse(r.result.value) : null;
  }

  /**
   * Synchronously focus the test client's canvas — required before
   * keydown / keyup events route to its DOM listeners.
   */
  async focusTarget() {
    await this._session.Runtime.evaluate({
      expression: "document.getElementById('target').focus()",
    });
  }

  /**
   * Read everything the stub WS has received so far. Stable snapshot;
   * later sends won't appear in the returned array.
   */
  receivedWire() {
    return this._stubWs.received().map((e) => e.msg);
  }

  /**
   * Drain any in-flight messages and clear the stub's buffer. Useful
   * between assertion phases.
   */
  resetWire() {
    this._stubWs.clear();
  }

  /**
   * Wait until at least `n` messages have arrived at the stub, or
   * `timeoutMs` elapses. Returns the snapshot (which may have more
   * than `n` if extras arrived).
   */
  async waitForWireCount(n, timeoutMs = 2_000) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      if (this._stubWs.received().length >= n) break;
      await sleep(20);
    }
    return this.receivedWire();
  }

  assert(label, fn, detail = {}) {
    let ok = false;
    let err = null;
    try {
      ok = !!fn();
    } catch (e) {
      err = String(e);
    }
    this.assertions.push({ label, ok, ...detail, error: err });
    this.log(ok ? "ok" : "FAIL", `assert: ${label}`, ok ? "" : detail);
    return ok;
  }

  async finish() {
    const summary = {
      name: this.name,
      passed: this.assertions.every((a) => a.ok),
      total: this.assertions.length,
      failed: this.assertions.filter((a) => !a.ok).length,
      assertions: this.assertions,
      stubErrors: this._stubWs ? this._stubWs.errors() : [],
      stubConnections: this._stubWs ? this._stubWs.connections() : 0,
    };
    fs.writeFileSync(
      path.join(this.artifactsDir, `assertions-${this.name}.json`),
      JSON.stringify(summary, null, 2),
    );
    process.stdout.write(
      `RUNNER_RESULT name=${this.name} total=${summary.total} ` +
        `passed=${summary.total - summary.failed} failed=${summary.failed}\n`,
    );

    // Teardown — best-effort.
    try {
      if (this._targetId) {
        await this._browser.Target.closeTarget({ targetId: this._targetId });
      }
    } catch (e) {
      this.log("warn", "closeTarget failed", String(e));
    }
    try {
      this._browser?.close && (await this._browser.close());
    } catch { /* swallow */ }
    try {
      this._staticServer?.close();
    } catch { /* swallow */ }
    if (this._stubWs) await this._stubWs.close();

    return summary;
  }
}

// ---------------------------------------------------------------------------
// Public runner — mirrors `runScenario`'s shape from _lib.mjs.
// ---------------------------------------------------------------------------

export async function runPortalScenario(scenario, opts = {}) {
  const cbUrl = opts.cbUrl ?? process.env.CHROMELESS_URL ?? "http://127.0.0.1:9222";
  const artifactsDir =
    opts.artifactsDir
    ?? process.env.TEST_ARTIFACTS_DIR
    ?? path.resolve(process.cwd(), "artifacts");
  const s = new PortalScenario({ name: scenario.name, cbUrl, artifactsDir });
  let runErr = null;
  try {
    await s.setup();
    await scenario.run(s);
  } catch (err) {
    runErr = err;
    s.assert(`scenario threw: ${err.message}`, () => false, {
      expected: "no exception",
      actual: String(err.stack || err),
    });
  } finally {
    const summary = await s.finish();
    if (runErr) process.exitCode = 1;
    else if (!summary.passed) process.exitCode = 2;
  }
}

export { sleep, MOD };

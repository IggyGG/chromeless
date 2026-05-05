// tests/webrtc/scenarios/_lib.mjs
//
// Scenario runner foundation. Provides a Scenario harness that:
//
//   1. Owns the CDP attach + page navigate + WebRTC peer setup that
//      drive-recorder.mjs already does, refactored so each scenario
//      gets a fresh BrowserContext / Target / recording.
//   2. Exposes input-dispatch helpers that wrap CDP `Input.dispatch*`
//      with semantic shapes (click, drag, type, hover) so scenario
//      authors don't reinvent the wheel.
//   3. Records each scenario to its own .webm with a sidecar
//      events-{name}.json (page-side event log) + assertions.json
//      (pass/fail per assertion).
//   4. Supports a SECOND concurrent CDP session on the same target so
//      "agent vs user" race scenarios can be expressed simply:
//        const agent = await scenario.openSecondaryCdpSession();
//        // dispatch on `agent` while the primary harness sends user
//        // inputs, and the page records who arrived first.
//
// Why CDP-direct from the harness instead of the WebRTC `input`
// DataChannel + input-bridge sidecar:
//
//   * The page-level question we care about ("does mousedown on a
//     draggable element fire dragstart?") is independent of the wire
//     path. CDP-direct pins the variable to "what the input-bridge
//     would have dispatched anyway" and removes a layer of plumbing
//     for local runs.
//
//   * The wire-format roundtrip (envelope → bridge → CDP) is already
//     covered by capture/input-bridge/main_test.go (27 unit tests on
//     the Go side) and tests/integration/input_loop_test.go.
//
//   * The cluster Job (chromeless-webrtc-input-validation.yaml) runs the
//     input-bridge sidecar and a separate set of e2e scenarios that
//     drive the DataChannel path. That covers the wire layer; this
//     module covers behaviour.
//
// Concurrent-CDP scenarios use this module's openSecondaryCdpSession
// — a fresh `Target.attachToTarget` flatten=true session on the SAME
// targetId, plus a dispatch lambda that emits CDP commands on it.
// Two attachers on one target is the production analogue of "agent
// drives via physics CdpSession while a human drives via the
// input-bridge"; the ordering / interleaving behaviour we observe is
// identical to what production sees.

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

const HERE = path.dirname(fileURLToPath(import.meta.url));
// Scenarios live in scenarios/, page fixtures live alongside, and the
// existing streamer-page.html is one level up at tests/webrtc/.
const WEBRTC_ROOT = path.dirname(HERE);

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

function makeLogger(scenarioName) {
  const startedAt = Date.now();
  const ts = () => `+${((Date.now() - startedAt) / 1000).toFixed(3)}s`;
  return (level, msg, extra) => {
    const tail = extra === undefined
      ? ""
      : "  " + (typeof extra === "string" ? extra : safeJson(extra));
    process.stderr.write(`[${scenarioName} ${ts()} ${level}] ${msg}${tail}\n`);
  };
}

function safeJson(v) { try { return JSON.stringify(v); } catch { return String(v); } }

// ---------------------------------------------------------------------------
// CDP session adapter
//
// chrome-remote-interface 0.33 exposes per-flat-session messaging via
// `browser.send(method, params, sessionId)` and `browser.on(event,
// (params, sId) => ...)` — there is no `.session()` helper. We wrap
// those primitives so callers can write `session.Page.navigate({...})`
// without the sessionId mechanics.
// ---------------------------------------------------------------------------

function makeSessionClient(browser, sessionId) {
  const send = (method, params = {}) => browser.send(method, params, sessionId);
  const subscribe = (eventName, handler) => {
    browser.on(eventName, (params, evtSessionId) => {
      if (evtSessionId === sessionId) handler(params);
    });
  };
  return {
    sessionId,
    send,
    subscribe,
    Page: {
      enable:      () => send("Page.enable"),
      navigate:    (p) => send("Page.navigate", p),
      reload:      (p = {}) => send("Page.reload", p),
      captureScreenshot: (p = {}) => send("Page.captureScreenshot", p),
    },
    Runtime: {
      enable:      () => send("Runtime.enable"),
      evaluate:    (p) => send("Runtime.evaluate", p),
      consoleAPICalled: (h) => subscribe("Runtime.consoleAPICalled", h),
    },
    Input: {
      dispatchMouseEvent: (p) => send("Input.dispatchMouseEvent", p),
      dispatchKeyEvent:   (p) => send("Input.dispatchKeyEvent", p),
      dispatchDragEvent:  (p) => send("Input.dispatchDragEvent", p),
      insertText:         (p) => send("Input.insertText", p),
    },
    DOM: {
      enable:      () => send("DOM.enable"),
      getDocument: (p = {}) => send("DOM.getDocument", p),
      querySelector: (p) => send("DOM.querySelector", p),
      getBoxModel: (p) => send("DOM.getBoxModel", p),
    },
    Emulation: {
      setDeviceMetricsOverride: (p) => send("Emulation.setDeviceMetricsOverride", p),
    },
  };
}

// ---------------------------------------------------------------------------
// Input helpers — translate semantic events into CDP wire format
// ---------------------------------------------------------------------------

// CDP modifier bits per chromium's docs:
//   Alt=1, Ctrl=2, Meta/Command=4, Shift=8
const MOD = { Alt: 1, Ctrl: 2, Meta: 4, Shift: 8 };

// Compute modifier bitfield from {alt, ctrl, meta, shift} bag.
function modBits(mods = {}) {
  return (mods.alt   ? MOD.Alt   : 0)
       | (mods.ctrl  ? MOD.Ctrl  : 0)
       | (mods.meta  ? MOD.Meta  : 0)
       | (mods.shift ? MOD.Shift : 0);
}

// Minimal key map covering ASCII + the special keys the scenarios use.
// CDP key events want { key, code, windowsVirtualKeyCode, text? }.
// For printable ASCII, text is the character itself; for special keys
// like "Enter", "ArrowDown", text is empty (the raw keystroke is what
// matters, not a printable character).
const KEY_MAP = {
  Enter:      { key: "Enter",      code: "Enter",      windowsVirtualKeyCode: 13 },
  Escape:     { key: "Escape",     code: "Escape",     windowsVirtualKeyCode: 27 },
  Tab:        { key: "Tab",        code: "Tab",        windowsVirtualKeyCode:  9 },
  Backspace:  { key: "Backspace",  code: "Backspace",  windowsVirtualKeyCode:  8 },
  Delete:     { key: "Delete",     code: "Delete",     windowsVirtualKeyCode: 46 },
  ArrowUp:    { key: "ArrowUp",    code: "ArrowUp",    windowsVirtualKeyCode: 38 },
  ArrowDown:  { key: "ArrowDown",  code: "ArrowDown",  windowsVirtualKeyCode: 40 },
  ArrowLeft:  { key: "ArrowLeft",  code: "ArrowLeft",  windowsVirtualKeyCode: 37 },
  ArrowRight: { key: "ArrowRight", code: "ArrowRight", windowsVirtualKeyCode: 39 },
  Home:       { key: "Home",       code: "Home",       windowsVirtualKeyCode: 36 },
  End:        { key: "End",        code: "End",        windowsVirtualKeyCode: 35 },
  PageUp:     { key: "PageUp",     code: "PageUp",     windowsVirtualKeyCode: 33 },
  PageDown:   { key: "PageDown",   code: "PageDown",   windowsVirtualKeyCode: 34 },
};

function keyDescriptor(keyOrChar) {
  if (KEY_MAP[keyOrChar]) return KEY_MAP[keyOrChar];
  // Single ASCII char fallback. Printable chars get their own keyCode
  // from charCodeAt — works for [a-z], [A-Z], digits, punctuation.
  if (keyOrChar.length === 1) {
    const c = keyOrChar;
    const upper = c.toUpperCase();
    const codeName =
      /[A-Z]/.test(upper) ? `Key${upper}` :
      /[0-9]/.test(c)     ? `Digit${c}`  :
      /\s/.test(c)        ? "Space"      : null;
    return {
      key: c,
      code: codeName || "",
      windowsVirtualKeyCode: upper.charCodeAt(0),
      text: c,
    };
  }
  throw new Error(`unknown key: ${keyOrChar}`);
}

// Build an InputBag keyed off (session, opts) so individual scenarios
// can mix-and-match dispatchers (primary user, secondary agent, etc.).
function makeInputBag(session, opts = {}) {
  const log = opts.log || (() => {});
  const tag = opts.tag || "user"; // recorded into events.json

  const stamp = (extra) => ({ ...extra, __dispatchTag: tag });

  return {
    tag,

    async mouseMove(x, y, mods = {}) {
      log("dbg", `mouseMove(${x},${y})`, stamp({ tag, mods }));
      return session.Input.dispatchMouseEvent({
        type: "mouseMoved", x, y,
        button: "none",
        modifiers: modBits(mods),
      });
    },

    async mouseDown(x, y, opts2 = {}) {
      const button = opts2.button || "left";
      const clickCount = opts2.clickCount ?? 1;
      log("dbg", `mouseDown(${x},${y},${button})`, stamp({ tag }));
      return session.Input.dispatchMouseEvent({
        type: "mousePressed", x, y, button,
        buttons: button === "left" ? 1 : button === "right" ? 2 : 4,
        clickCount,
        modifiers: modBits(opts2),
      });
    },

    async mouseUp(x, y, opts2 = {}) {
      const button = opts2.button || "left";
      const clickCount = opts2.clickCount ?? 1;
      log("dbg", `mouseUp(${x},${y},${button})`, stamp({ tag }));
      return session.Input.dispatchMouseEvent({
        type: "mouseReleased", x, y, button,
        buttons: 0,
        clickCount,
        modifiers: modBits(opts2),
      });
    },

    async click(x, y, opts2 = {}) {
      // Dispatch as press + release at the same coords. clickCount=1
      // for a single click, =2 for double-click. Modifiers travel on
      // BOTH events (chromium's click-detection logic checks both).
      await this.mouseDown(x, y, opts2);
      await this.mouseUp(x, y, opts2);
    },

    async doubleClick(x, y, opts2 = {}) {
      // First click clickCount=1, second click clickCount=2 — chromium
      // requires the ramping count for the dblclick event to fire.
      await this.click(x, y, { ...opts2, clickCount: 1 });
      await this.click(x, y, { ...opts2, clickCount: 2 });
    },

    async drag(x1, y1, x2, y2, opts2 = {}) {
      // Native HTML5 drag-and-drop firing depends on movement-during-
      // mousedown, which chromium's hit detector measures over a
      // 5-pixel slop. Stepping the path with several mouseMove events
      // is the difference between a registered drag and a misclassified
      // click, especially for short distances.
      const steps = opts2.steps ?? 12;
      const settleMs = opts2.settleMs ?? 16; // ~one frame between moves
      await this.mouseDown(x1, y1, opts2);
      for (let i = 1; i <= steps; i++) {
        const t = i / steps;
        const x = Math.round(x1 + (x2 - x1) * t);
        const y = Math.round(y1 + (y2 - y1) * t);
        // mouseMove during a drag must report buttons=1 so the
        // browser knows the button is still held (some pages care).
        await session.Input.dispatchMouseEvent({
          type: "mouseMoved", x, y,
          button: "left", buttons: 1,
          modifiers: modBits(opts2),
        });
        if (settleMs > 0) await sleep(settleMs);
      }
      await this.mouseUp(x2, y2, opts2);
    },

    async wheel(x, y, deltaX, deltaY, mods = {}) {
      log("dbg", `wheel(${x},${y},${deltaX},${deltaY})`, stamp({ tag, mods }));
      return session.Input.dispatchMouseEvent({
        type: "mouseWheel",
        x, y,
        deltaX, deltaY,
        modifiers: modBits(mods),
      });
    },

    async keyDown(key, mods = {}) {
      const d = keyDescriptor(key);
      log("dbg", `keyDown(${key})`, stamp({ tag, mods }));
      const params = {
        type: "keyDown",
        modifiers: modBits(mods),
        key:  d.key,
        code: d.code,
        windowsVirtualKeyCode: d.windowsVirtualKeyCode,
      };
      if (d.text) params.text = d.text;
      return session.Input.dispatchKeyEvent(params);
    },

    async keyUp(key, mods = {}) {
      const d = keyDescriptor(key);
      log("dbg", `keyUp(${key})`, stamp({ tag, mods }));
      return session.Input.dispatchKeyEvent({
        type: "keyUp",
        modifiers: modBits(mods),
        key:  d.key,
        code: d.code,
        windowsVirtualKeyCode: d.windowsVirtualKeyCode,
      });
    },

    async press(key, mods = {}) {
      await this.keyDown(key, mods);
      await this.keyUp(key, mods);
    },

    async type(text, opts2 = {}) {
      // For each character emit keyDown + char + keyUp. Input.insertText
      // would be a shortcut but it bypasses the page's keydown/keypress
      // listeners — defeating any test that wants to verify keyboard
      // event handlers fired correctly. We pay the per-char round-trip
      // cost.
      const intervalMs = opts2.intervalMs ?? 8;
      for (const ch of text) {
        await this.keyDown(ch, opts2);
        await this.keyUp(ch, opts2);
        if (intervalMs > 0) await sleep(intervalMs);
      }
    },

    /**
     * Insert text via Input.insertText — bypasses keyboard event
     * dispatch, useful when a scenario explicitly wants the IME-style
     * batched insert path (e.g. testing IME composition end + commit
     * delivers text without firing key events).
     */
    async insertText(text) {
      return session.Input.insertText({ text });
    },
  };
}

function sleep(ms) { return new Promise((r) => setTimeout(r, ms)); }

// ---------------------------------------------------------------------------
// Wire-path InputBag — emits v1 envelopes (per docs/protocols/input-channel.md)
// onto a WebRTC DataChannel. The streamer page's InputRelay forwards each
// message verbatim to the input-bridge sidecar at ws://127.0.0.1:9100/input.
// The bridge translates envelopes back into CDP Input.dispatch* calls. This
// is the production wire path — what runs when an LLM agent or human user
// drives the browser via the chromeless protocol.
// ---------------------------------------------------------------------------

// Modifier bitmask per chromeless protocol v1:
//   1 = Shift, 2 = Ctrl, 4 = Alt, 8 = Meta
const WIRE_MOD = { Shift: 1, Ctrl: 2, Alt: 4, Meta: 8 };

function wireModBits(mods = {}) {
  return (mods.shift ? WIRE_MOD.Shift : 0)
       | (mods.ctrl  ? WIRE_MOD.Ctrl  : 0)
       | (mods.alt   ? WIRE_MOD.Alt   : 0)
       | (mods.meta  ? WIRE_MOD.Meta  : 0);
}

const WIRE_BUTTON = { left: 0, middle: 1, right: 2, back: 3, forward: 4 };

function wireKeyDescriptor(keyOrChar) {
  // Reuse the CDP key map (same KeyboardEvent.key/code semantics as
  // the wire format), but only emit `key` + `code` + `mods`. Wire
  // format omits text — bridge synthesises it from `key` if needed.
  if (KEY_MAP[keyOrChar]) {
    return { key: KEY_MAP[keyOrChar].key, code: KEY_MAP[keyOrChar].code };
  }
  if (keyOrChar.length === 1) {
    const upper = keyOrChar.toUpperCase();
    const codeName = /[A-Z]/.test(upper) ? `Key${upper}`
                   : /[0-9]/.test(keyOrChar) ? `Digit${keyOrChar}`
                   : /\s/.test(keyOrChar) ? "Space" : "";
    return { key: keyOrChar, code: codeName };
  }
  throw new Error(`unknown key: ${keyOrChar}`);
}

/**
 * Build a WireInputBag bound to a particular RTCDataChannel. Emits
 * v1 envelopes that match the wire format the input-bridge expects.
 * The bag deliberately mirrors the CDP-direct InputBag's surface
 * (click, drag, type, wheel, ...) so scenarios can swap between
 * "page-level" and "wire-format" tests by switching which bag they
 * use.
 */
function makeWireInputBag(dataChannel, opts = {}) {
  const log = opts.log || (() => {});
  const tag = opts.tag || "wire";
  let seq = 0;

  function send(type, data) {
    if (!dataChannel || dataChannel.readyState !== "open") {
      throw new Error(`wire DC not open (state=${dataChannel?.readyState})`);
    }
    const envelope = {
      v: 1,
      type,
      t: Date.now(),
      seq: seq++,
      data,
    };
    dataChannel.send(JSON.stringify(envelope));
    log("dbg", `wire ${type}`, { seq: envelope.seq, data });
  }

  return {
    tag,
    seq: () => seq,

    async mouseMove(x, y) {
      send("mouse_move", { x, y });
    },

    async mouseDown(x, y, opts2 = {}) {
      const button = WIRE_BUTTON[opts2.button || "left"] ?? 0;
      send("mouse_button", { button, action: "down", x, y });
    },

    async mouseUp(x, y, opts2 = {}) {
      const button = WIRE_BUTTON[opts2.button || "left"] ?? 0;
      send("mouse_button", { button, action: "up", x, y });
    },

    async click(x, y, opts2 = {}) {
      // The protocol has no "click" envelope — it's down + up, with
      // the bridge applying its own double-click timing rules.
      // Modifiers travel via key_down envelopes that bracket the
      // mouse_button events; for simple clicks we just send raw
      // mouse_button. (The bridge's CDP dispatch reads modifier
      // state from any held key_down events — but for tests we
      // emit transient key_down/key_up around the click to mimic
      // a real "shift-click" wire pattern.)
      const heldKeys = [];
      if (opts2.shift) heldKeys.push("Shift");
      if (opts2.ctrl)  heldKeys.push("Control");
      if (opts2.alt)   heldKeys.push("Alt");
      if (opts2.meta)  heldKeys.push("Meta");
      for (const k of heldKeys) {
        send("key_down", { key: k, code: k === "Control" ? "ControlLeft" : `${k}Left`,
                           mods: wireModBits(opts2) });
      }
      await this.mouseDown(x, y, opts2);
      await this.mouseUp(x, y, opts2);
      for (const k of heldKeys.reverse()) {
        send("key_up", { key: k, code: k === "Control" ? "ControlLeft" : `${k}Left`,
                         mods: 0 });
      }
    },

    async doubleClick(x, y, opts2 = {}) {
      await this.click(x, y, opts2);
      // Bridge applies double-click timing — pause < bridge's
      // dblclick interval.
      await sleep(40);
      await this.click(x, y, opts2);
    },

    async drag(x1, y1, x2, y2, opts2 = {}) {
      const steps = opts2.steps ?? 16;
      const settleMs = opts2.settleMs ?? 18;
      await this.mouseDown(x1, y1, opts2);
      for (let i = 1; i <= steps; i++) {
        const t = i / steps;
        const x = Math.round(x1 + (x2 - x1) * t);
        const y = Math.round(y1 + (y2 - y1) * t);
        send("mouse_move", { x, y });
        if (settleMs > 0) await sleep(settleMs);
      }
      await this.mouseUp(x2, y2, opts2);
    },

    async wheel(x, y, deltaX, deltaY, mods = {}) {
      send("mouse_wheel", {
        dx: deltaX, dy: deltaY, mode: 0, delta_mode: "pixel",
        phase: "changed", momentum: false, x, y,
      });
    },

    async keyDown(key, mods = {}) {
      const d = wireKeyDescriptor(key);
      send("key_down", { key: d.key, code: d.code, mods: wireModBits(mods) });
    },

    async keyUp(key, mods = {}) {
      const d = wireKeyDescriptor(key);
      send("key_up", { key: d.key, code: d.code, mods: wireModBits(mods) });
    },

    async press(key, mods = {}) {
      await this.keyDown(key, mods);
      await this.keyUp(key, mods);
    },

    async type(text, opts2 = {}) {
      // No insertText path on the wire — every printable character
      // ships as a key_down + key_up pair. Bridge synthesises the
      // CDP key event with `text` derived from `key`.
      const intervalMs = opts2.intervalMs ?? 12;
      for (const ch of text) {
        await this.keyDown(ch, opts2);
        await this.keyUp(ch, opts2);
        if (intervalMs > 0) await sleep(intervalMs);
      }
    },
  };
}

// ---------------------------------------------------------------------------
// Static server — same shape as drive-recorder.mjs, scoped to scenarios/
// so each scenario can serve its own fixture page from a known origin.
// ---------------------------------------------------------------------------

function startStaticServer(host, port, root) {
  return new Promise((resolve, reject) => {
    const server = http.createServer((req, res) => {
      try {
        const reqUrl = new URL(req.url, `http://${host}:${port || 0}/`);
        let rel = decodeURIComponent(reqUrl.pathname);
        if (rel === "/") rel = "/index.html";
        // Resolve relative to root + WEBRTC_ROOT — the harness's
        // streamer-page.html and demo.js live one level above this dir.
        const candidates = [
          path.normalize(path.join(root, rel.replace(/^\//, ""))),
          path.normalize(path.join(WEBRTC_ROOT, rel.replace(/^\//, ""))),
        ];
        const target = candidates.find(
          (p) => p.startsWith(root) || p.startsWith(WEBRTC_ROOT),
        );
        if (!target) { res.writeHead(403); res.end("forbidden"); return; }
        const found = candidates.find(
          (p) => fs.existsSync(p) && fs.statSync(p).isFile(),
        );
        if (!found) { res.writeHead(404); res.end(`not found: ${rel}`); return; }
        const ext = path.extname(found).toLowerCase();
        const ct = ext === ".html" ? "text/html; charset=utf-8"
                 : ext === ".js" || ext === ".mjs"
                                  ? "application/javascript; charset=utf-8"
                 : ext === ".json" ? "application/json; charset=utf-8"
                 : "application/octet-stream";
        res.writeHead(200, { "content-type": ct, "cache-control": "no-store" });
        fs.createReadStream(found).pipe(res);
      } catch (err) {
        res.writeHead(500); res.end(String(err));
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

// ---------------------------------------------------------------------------
// Scenario class — what each *.scenario.mjs hands its `run` function.
// ---------------------------------------------------------------------------

export class Scenario {
  constructor({ name, page, durationMs, cbUrl, artifactsDir, wire }) {
    this.name = name;
    this.page = page;
    this.durationMs = durationMs ?? 5_000;
    this.cbUrl = cbUrl;
    this.artifactsDir = artifactsDir;
    this.wire = wire || null;  // { url } enables ?wire=1 + DC capture
    this.log = makeLogger(name);
    this.assertions = []; // {label, ok, expected, actual, atMs}
    this.markers = [];    // {label, atMs}
    this.startedAt = null;

    this._browser = null;
    this._session = null;
    this._secondarySessions = [];
    this._peer = null;
    this._sink = null;
    this._ffmpeg = null;
    this._frameCount = 0;
    this._firstFrameAt = null;
    this._frameDropsSizeMismatch = 0;
    this._frameWidth = 0;
    this._frameHeight = 0;
    this._server = null;
    this._wireDc = null;       // RTCDataChannel from pc.ondatachannel
  }

  // ---- static server + CDP attach ----

  async setup() {
    fs.mkdirSync(this.artifactsDir, { recursive: true });

    // Static server: the scenario page (or default to streamer-page.html
    // with a `?fixture=` param pointing at a fixture HTML).
    const sv = await startStaticServer("127.0.0.1", 0, HERE);
    this._server = sv.server;
    const wireQs = this.wire
      ? `?wire=1&wire-url=${encodeURIComponent(this.wire.url)}`
      : "";
    const navUrl = `http://${sv.host}:${sv.port}/${this.page}${wireQs}`;
    this.log("info", "static server", {
      host: sv.host, port: sv.port, page: this.page,
      wire: this.wire ? this.wire.url : null,
    });

    // /json/version handshake (DNS-rebinding mitigation: Host: localhost)
    const u = new URL(this.cbUrl);
    const versionUrl = new URL("/json/version", u).toString();
    const ver = await fetchJsonWithHostOverride(versionUrl, "localhost");
    let wsUrl = ver.webSocketDebuggerUrl;
    if (!wsUrl) throw new Error("no webSocketDebuggerUrl in /json/version");
    wsUrl = wsUrl.replace(/ws:\/\/[^/]+/, `ws://${u.host}`);

    // local:true uses chrome-remote-interface's bundled protocol
    // descriptor instead of fetching /json/protocol from the target
    // at connect time. The chromeless build at cr7727-sw FATALs in
    // devtools_http_handler.cc:724 ("Could not load protocol") when
    // /json/protocol is requested — the embedder's resource pak is
    // missing the bundled descriptor file. Stock chromium has it
    // baked in; chromeless will need a separate fix to package it.
    // For now the harness sidesteps the path entirely.
    this._browser = await CDP({ target: wsUrl, local: true });
    this.log("ok", "browser CDP attached");

    // BUGS-529 workaround (test-only): chromeless's flat-mode CDP
    // session DOES follow Page.frameNavigated correctly (verified by
    // input-bridge's diagnostic Page.frameNavigated logging — sessionId
    // stays stable across navigation), but Input.dispatch* on a session
    // bound through a navigation never reaches the renderer. This
    // appears to be a chromium-tree bug in chromeless's InputRouter
    // binding-update path.
    //
    // Sidestep by creating a NEW page target boot-loaded directly to
    // the fixture URL: the input-bridge's flat-mode auto-attach picks
    // up that target as a fresh session bound to a RenderFrameHost
    // that never navigated, so the InputRouter binding is stable.
    //
    // Implementation:
    //   * wire mode: Target.createTarget({url: navUrl}) — the bridge
    //     auto-switches its currentSID to this new target's session
    //     and dispatches all input there. We attach our own flat-mode
    //     session for assertion-side reads (Runtime.evaluate of
    //     window.__events).
    //   * non-wire: unchanged — non-wire scenarios already createTarget
    //     against a fresh BrowserContext.
    //
    // This keeps the production chromeless-browserless deployment path
    // unchanged (production never navigates; the streamer page IS
    // the boot URL), and is exactly the test-only workaround the
    // BUGS-529 description names #2.
    let browserContextId, targetId;
    if (this.wire) {
      // chromeless's CbDevToolsManagerDelegate::CreateNewTarget bails
      // with "no context available" when called without an explicit
      // browserContextId AND main_parts hasn't called
      // SetDefaultBrowserContext (which it doesn't — see
      // capture/build-integration/cloud_browser_browser_main_parts.cc;
      // a separate chromium-tree TODO). To unblock the test workaround
      // we call Target.createBrowserContext first; chromeless's
      // CbDevToolsManagerDelegate::CreateBrowserContext pushes onto
      // contexts_, and CreateNewTarget then reads contexts_.back().
      //
      // We deliberately do NOT track the returned browserContextId for
      // teardown disposal — chromeless has a known DisposeBrowserContext
      // destruction-order bug (see cb_devtools_agent.cc TODO header), so
      // Target.closeTarget alone is what we tear down per scenario.
      // The browser process exits on Pod shutdown which cleans up the
      // leaked contexts; at 3 wire scenarios per run the leak is
      // bounded and harmless.
      const createCtx = await this._browser.Target.createBrowserContext({});
      ({ targetId } = await this._browser.Target.createTarget({
        url: navUrl,
        browserContextId: createCtx.browserContextId,
      }));
      browserContextId = null;  // suppress teardown disposal
      this.log("ok",
          "wire mode: created target boot-loaded to fixture (BUGS-529 workaround)",
          { targetId, navUrl, ctx: createCtx.browserContextId });
    } else {
      ({ browserContextId } = await this._browser.Target.createBrowserContext({}));
      ({ targetId } = await this._browser.Target.createTarget({
        url: "about:blank", browserContextId,
      }));
      this.log("ok", "target created", { targetId, browserContextId });
    }
    this._browserContextId = browserContextId;
    this._targetId = targetId;
    // Wire-mode targets are now fresh-per-scenario, so they get the
    // same teardown path as non-wire (closeTarget). Set false so the
    // teardown branch doesn't try to navigate-to-about:blank a target
    // that's about to be closed anyway.
    this._reusedTarget = false;

    const { sessionId } = await this._browser.Target.attachToTarget({
      targetId, flatten: true,
    });
    this._session = makeSessionClient(this._browser, sessionId);
    await this._session.Page.enable();
    await this._session.Runtime.enable();
    this.log("ok", "primary CDP session attached", { sessionId });

    // Wire up console bridge so the page's own emit() lines (CBTEST:…)
    // surface in the harness log for debugging. Page-side input event
    // capture happens via window.__events; we read it after the run.
    this._session.Runtime.consoleAPICalled((evt) => {
      const args = evt.args || [];
      if (args.length === 0) return;
      const v = args[0];
      if (v?.type === "string" && v.value?.startsWith("CBTEST:")) {
        this.log("page", `[${evt.type}] ${v.value}`);
      }
    });

    // Set viewport explicitly so coordinate math is deterministic. The
    // canvas in the streamer-page is 1280×720; we mirror that here so
    // input dispatch coordinates land where the recording shows them.
    await this._session.Emulation.setDeviceMetricsOverride({
      width: 1280, height: 720,
      deviceScaleFactor: 1, mobile: false,
    });

    if (this.wire) {
      // Target was boot-loaded to navUrl via Target.createTarget above
      // (BUGS-529 workaround). Skip the explicit navigate so the
      // input-bridge's session stays bound to the RFH that loaded the
      // fixture from boot — never navigated, so chromeless's broken
      // InputRouter binding-update path doesn't engage. We still need
      // to wait for the page to finish loading before continuing,
      // since createTarget returns synchronously after the URL is
      // queued.
      //
      // We poll Runtime.evaluate for document.readyState rather than
      // subscribing to Page.loadEventFired — the load event may fire
      // before subscribe() is registered (race), and the readyState
      // approach is robust to that.
      const readyDeadline = Date.now() + 5000;
      while (Date.now() < readyDeadline) {
        try {
          const r = await this._session.Runtime.evaluate({
            expression: "document.readyState",
            returnByValue: true,
          });
          if (r?.result?.value === "complete") break;
        } catch { /* tolerate evaluation race during early init */ }
        await new Promise((res) => setTimeout(res, 50));
      }
      this.log("info", "boot-loaded fixture (no navigate)", { navUrl });
    } else {
      await this._session.Page.navigate({ url: navUrl });
      this.log("info", "navigated", { navUrl });
    }

    // Build the WebRTC peer + start recording. The streamer-page
    // (loaded above) drives the offer; we answer.
    this._peer = new RTCPeerConnection({});

    await this._setupRecording();
    await this._negotiateAndWait();

    this.startedAt = Date.now();

    // Default user/agent input bags.
    this.user = makeInputBag(this._session, { log: this.log, tag: "user" });

    return this;
  }

  /**
   * Wait for the page's "input" DataChannel to arrive via
   * pc.ondatachannel and reach readyState=="open", then return a
   * WireInputBag bound to it. Throws if wire mode wasn't enabled
   * on the scenario or the channel never opens within the timeout.
   *
   * Also reads the page's relay-state diagnostic to confirm the
   * page has dialed the input-bridge WebSocket — if the bridge
   * isn't reachable we surface that as a setup failure rather
   * than letting downstream send() calls fail with cryptic
   * timeouts.
   */
  async setupWire(opts = {}) {
    if (!this.wire) {
      throw new Error("setupWire called but scenario.wire was not configured");
    }
    const dcDeadline = Date.now() + (opts.dcTimeoutMs ?? 8_000);
    while (!this._wireDc) {
      if (Date.now() > dcDeadline) {
        throw new Error("wire DC never arrived from page (pc.ondatachannel timeout)");
      }
      await sleep(50);
    }
    const dc = this._wireDc;
    if (dc.readyState !== "open") {
      await new Promise((resolve, reject) => {
        const t = setTimeout(
          () => reject(new Error(`wire DC stuck in ${dc.readyState}`)),
          (opts.dcOpenTimeoutMs ?? 4_000),
        );
        dc.addEventListener("open", () => { clearTimeout(t); resolve(); }, { once: true });
        dc.addEventListener("error", (e) => {
          clearTimeout(t);
          reject(new Error("wire DC error: " + (e.error?.message || String(e))));
        }, { once: true });
      });
    }
    this.log("ok", "wire DC open", { label: dc.label });

    // Confirm the page-side relay actually connected to the
    // input-bridge. The relay state is exposed via window.__cbtest.relay
    // — wait briefly for ws_state to flip to "open".
    const wsDeadline = Date.now() + (opts.wsTimeoutMs ?? 4_000);
    while (Date.now() < wsDeadline) {
      const relay = await this.runtimeEval(
        `JSON.stringify(window.__cbtest && window.__cbtest.relay || {})`,
      );
      const r = JSON.parse(relay || "{}");
      if (r.ws_state === "open") {
        this.log("ok", "page→bridge relay open", r);
        break;
      }
      if (r.ws_state === "error" || r.ws_state === "closed") {
        throw new Error(
          `page→bridge relay failed (state=${r.ws_state} err=${r.last_error})`,
        );
      }
      await sleep(100);
    }

    return makeWireInputBag(dc, { log: this.log, tag: "wire" });
  }

  /**
   * Read the page-side relay diagnostic. Useful for assertions that
   * the wire path actually carried traffic.
   */
  async readWireRelay() {
    const raw = await this.runtimeEval(
      `JSON.stringify(window.__cbtest && window.__cbtest.relay || null)`,
    );
    return raw ? JSON.parse(raw) : null;
  }

  /**
   * Open a SECOND CDP session attached to the SAME target.
   * Used for concurrency scenarios: this represents an "agent" driving
   * the browser via CDP while the harness's primary session represents
   * a "user" driving via input-bridge / DataChannel. Two flat-mode
   * sessions on the same target multiplex over the same browser ws —
   * exactly the production scenario when an LLM agent issues
   * Input.dispatch* concurrently with a human user's events.
   */
  async openSecondaryCdpSession(tag = "agent") {
    const { sessionId } = await this._browser.Target.attachToTarget({
      targetId: this._targetId, flatten: true,
    });
    const session = makeSessionClient(this._browser, sessionId);
    await session.Page.enable();
    await session.Runtime.enable();
    this._secondarySessions.push(session);
    const inputBag = makeInputBag(session, { log: this.log, tag });
    this.log("ok", `secondary CDP session attached (${tag})`, { sessionId });
    return { session, ...inputBag };
  }

  // ---- recording pipeline ----

  async _setupRecording() {
    this._peer.addTransceiver("video", { direction: "recvonly" });
    // Capture any DataChannel the page creates (the wire-path
    // "input" channel when wire mode is on). The harness sends v1
    // envelopes on this channel; the page's InputRelay forwards
    // them to the input-bridge sidecar.
    this._peer.ondatachannel = (ev) => {
      const dc = ev.channel;
      this.log("ok", "← page-created DataChannel", {
        label: dc.label, ordered: dc.ordered,
      });
      if (dc.label === "input") this._wireDc = dc;
    };
    this._peer.ontrack = (ev) => {
      const track = ev.track;
      if (track.kind !== "video") return;
      this._sink = new RTCVideoSink(track);
      this._sink.onframe = ({ frame }) => {
        this._frameCount += 1;
        if (this._firstFrameAt === null) {
          this._firstFrameAt = Date.now();
          this._frameWidth = frame.width;
          this._frameHeight = frame.height;
          this.log("ok", "first frame", { w: frame.width, h: frame.height });
          this._spawnFfmpeg(frame.width, frame.height);
        } else if (frame.width !== this._frameWidth || frame.height !== this._frameHeight) {
          this._frameDropsSizeMismatch += 1;
          if (this._frameDropsSizeMismatch === 1) {
            this.log("warn", "frame size mismatch — dropping",
                { expected: { w: this._frameWidth, h: this._frameHeight },
                  got:      { w: frame.width,    h: frame.height } });
          }
          return;
        }
        if (this._ffmpeg && !this._ffmpeg.killed && this._ffmpeg.stdin
            && !this._ffmpeg.stdin.destroyed) {
          try { this._ffmpeg.stdin.write(Buffer.from(frame.data)); }
          catch { /* ffmpeg likely exited */ }
        }
      };
    };
  }

  _spawnFfmpeg(w, h) {
    this.outFile = path.join(
      this.artifactsDir,
      `webrtc-${this.name}.webm`,
    );
    const ffArgs = [
      "-loglevel", "warning",
      "-f", "rawvideo",
      "-pix_fmt", "yuv420p",
      "-s", `${w}x${h}`,
      "-r", "30",
      "-i", "pipe:0",
      "-c:v", "libvpx-vp9",
      "-deadline", "realtime",
      "-cpu-used", "8",
      "-y", this.outFile,
    ];
    this._ffmpeg = spawn("ffmpeg", ffArgs, { stdio: ["pipe", "ignore", "inherit"] });
    this._ffmpeg.on("error", (err) => {
      this.log("err", "ffmpeg spawn failed — is ffmpeg on PATH?", String(err));
    });
  }

  async _negotiateAndWait() {
    // The streamer-page emits an SDP offer via console.log("CBTEST:…")
    // once it boots; we receive it via Runtime.consoleAPICalled and
    // dispatch the answer through window.__cbtest.handle.
    let resolveOffer;
    const offerReady = new Promise((r) => { resolveOffer = r; });
    let pageReady = false;

    this._session.Runtime.consoleAPICalled(async (evt) => {
      const args = evt.args || [];
      if (args.length === 0) return;
      const v = args[0];
      if (v?.type !== "string" || !v.value?.startsWith("CBTEST:")) return;
      let msg;
      try { msg = JSON.parse(v.value.slice("CBTEST:".length)); } catch { return; }
      if (msg.type === "sdp-offer") resolveOffer(msg.sdp);
      else if (msg.type === "ice" && msg.candidate) {
        try { await this._peer.addIceCandidate(msg.candidate); } catch { /* tolerant */ }
      } else if (msg.type === "ready") pageReady = true;
    });

    this._peer.onicecandidate = async (ev) => {
      const cand = ev.candidate ? ev.candidate.toJSON() : null;
      try {
        await this._session.Runtime.evaluate({
          expression: `window.__cbtest && window.__cbtest.handle(${
            JSON.stringify({ type: "ice", candidate: cand })
          })`,
          awaitPromise: true,
          returnByValue: true,
        });
      } catch { /* page may not be ready */ }
    };

    // Two paths to resolving the offer:
    //   1. Live console subscription (above) — works when subscribe
    //      ran before the page emitted CBTEST:sdp-offer.
    //   2. Polled Runtime.evaluate of window.__cbtest.sdpOffer — the
    //      fixture stores the offer on window state so the harness can
    //      retrieve it even if its console subscription attached too
    //      late. Required for the BUGS-529 workaround where wire-mode
    //      scenarios boot a fresh target directly to the fixture URL,
    //      so page emission can race ahead of subscribe.
    const polledOffer = (async () => {
      const deadline = Date.now() + 30_000;
      while (Date.now() < deadline) {
        try {
          const r = await this._session.Runtime.evaluate({
            expression: "window.__cbtest && window.__cbtest.sdpOffer",
            returnByValue: true,
          });
          const sdp = r?.result?.value;
          if (typeof sdp === "string" && sdp.length > 0) return sdp;
        } catch { /* page early init — tolerate */ }
        await sleep(100);
      }
      throw new Error("sdp-offer never appeared on window.__cbtest.sdpOffer");
    })();

    const offerSdp = await Promise.race([
      offerReady,
      polledOffer,
      new Promise((_, rej) => setTimeout(
        () => rej(new Error("sdp-offer never arrived")), 30_000)),
    ]);
    await this._peer.setRemoteDescription({ type: "offer", sdp: offerSdp });
    const answer = await this._peer.createAnswer();
    await this._peer.setLocalDescription(answer);
    await this._session.Runtime.evaluate({
      expression: `window.__cbtest.handle(${
        JSON.stringify({ type: "sdp-answer", sdp: answer.sdp })
      })`,
      awaitPromise: true, returnByValue: true,
    });
    this.log("ok", "→ sdp-answer pushed");

    // Wait for first frame so the recording window is real.
    const frameDeadline = Date.now() + 20_000;
    while (this._firstFrameAt === null) {
      if (Date.now() > frameDeadline) throw new Error("no frames received");
      await sleep(50);
    }
    // Quiesce briefly so the demo's "boot" scene paints before scenarios
    // start dispatching.
    await sleep(300);
  }

  // ---- assertions + page reads ----

  /**
   * Read the page's window.__events log (populated by the input-mirror
   * fixture page's listeners). Returns the captured event sequence
   * since the last reset.
   */
  async readEvents() {
    const r = await this._session.Runtime.evaluate({
      expression: `JSON.stringify(window.__events || [])`,
      returnByValue: true,
    });
    if (r.exceptionDetails) throw new Error(safeJson(r.exceptionDetails));
    return JSON.parse(r.result.value);
  }

  async resetEvents() {
    await this._session.Runtime.evaluate({
      expression: `if (window.__events) window.__events.length = 0; null`,
      returnByValue: true,
    });
  }

  async runtimeEval(expression) {
    const r = await this._session.Runtime.evaluate({
      expression, awaitPromise: true, returnByValue: true,
    });
    if (r.exceptionDetails) throw new Error(safeJson(r.exceptionDetails));
    return r.result?.value;
  }

  /**
   * Tag a moment in the recording for human review. Markers are
   * written into events.json with timestamps so the reviewer can
   * scrub the webm to the labelled moment.
   */
  marker(label) {
    const atMs = this.startedAt ? Date.now() - this.startedAt : 0;
    this.markers.push({ label, atMs });
    this.log("mark", `[+${atMs}ms] ${label}`);
  }

  assert(label, predicate, { expected, actual } = {}) {
    const atMs = this.startedAt ? Date.now() - this.startedAt : 0;
    let ok = false;
    let err;
    try { ok = !!predicate(); }
    catch (e) { ok = false; err = String(e); }
    this.assertions.push({
      label, ok, expected, actual, atMs,
      ...(err ? { error: err } : {}),
    });
    this.log(ok ? "ok" : "fail", `${label}`,
        ok ? undefined : { expected, actual, error: err });
    return ok;
  }

  // ---- teardown ----

  async finish() {
    // Flush ffmpeg, write events.json + assertions.json + summary.
    if (this._sink) try { this._sink.stop(); } catch { /* */ }
    if (this._ffmpeg && !this._ffmpeg.killed) {
      try { this._ffmpeg.stdin.end(); } catch { /* */ }
      await new Promise((resolve) => {
        const t = setTimeout(() => {
          try { this._ffmpeg.kill("SIGKILL"); } catch { /* */ }
          resolve();
        }, 5_000);
        this._ffmpeg.on("exit", () => { clearTimeout(t); resolve(); });
      });
    }

    const events = await this.readEvents().catch(() => []);
    const summary = {
      name: this.name,
      page: this.page,
      startedAt: this.startedAt,
      finishedAt: Date.now(),
      durationMs: this.startedAt ? Date.now() - this.startedAt : null,
      frames: this._frameCount,
      frameDropsSizeMismatch: this._frameDropsSizeMismatch,
      videoSize: { w: this._frameWidth, h: this._frameHeight },
      outFile: this.outFile,
      assertions: this.assertions,
      markers: this.markers,
      passed: this.assertions.every((a) => a.ok),
      eventCount: events.length,
    };

    const eventsFile = path.join(this.artifactsDir, `events-${this.name}.json`);
    const summaryFile = path.join(this.artifactsDir, `summary-${this.name}.json`);
    fs.writeFileSync(eventsFile, JSON.stringify(events, null, 2));
    fs.writeFileSync(summaryFile, JSON.stringify(summary, null, 2));

    let outSize = 0;
    try { outSize = fs.statSync(this.outFile).size; } catch { /* */ }

    process.stderr.write(
      `TEST_ARTIFACT scenario=${this.name} ` +
      `path=${this.outFile} bytes=${outSize} ` +
      `frames=${this._frameCount} ` +
      `passed=${summary.passed} ` +
      `assertions=${this.assertions.length} ` +
      `events=${events.length}\n`,
    );

    try { this._peer.close(); } catch { /* */ }
    if (!this._reusedTarget) {
      // Wire-mode scenarios share the bridge's target — leaving it
      // alive across scenarios is intentional.
      try {
        await this._browser.Target.closeTarget({ targetId: this._targetId });
        if (this._browserContextId) {
          await this._browser.Target.disposeBrowserContext({
            browserContextId: this._browserContextId,
          });
        }
      } catch { /* tolerate */ }
    } else {
      // Scrub state on the reused target so the next scenario starts
      // clean. Navigate to about:blank — the bridge stays attached to
      // the same targetId, just with a fresh document.
      try {
        await this._session.Page.navigate({ url: "about:blank" });
      } catch { /* */ }
    }
    try { await this._browser.close(); } catch { /* */ }
    try { this._server.close(); } catch { /* */ }

    return summary;
  }
}

// ---------------------------------------------------------------------------
// Entry helper used by individual scenario files.
// ---------------------------------------------------------------------------

/**
 * Each scenario file does:
 *
 *   import { runScenario } from "./_lib.mjs";
 *   export const scenario = {
 *     name: "01-mouse-click",
 *     page: "fixture-input-mirror.html",
 *     async run(s) {
 *       await s.user.click(640, 360);
 *       const events = await s.readEvents();
 *       s.assert("got one click", () => events.filter(e=>e.type==="click").length === 1);
 *     },
 *   };
 *   if (import.meta.url === `file://${process.argv[1]}`) runScenario(scenario);
 */
export async function runScenario(scenario, opts = {}) {
  const cbUrl = opts.cbUrl
    ?? process.env.CHROMELESS_URL
    ?? "http://127.0.0.1:9222";
  const artifactsDir = opts.artifactsDir
    ?? process.env.TEST_ARTIFACTS_DIR
    ?? path.resolve(process.cwd(), "artifacts");
  const s = new Scenario({
    name: scenario.name,
    page: scenario.page,
    durationMs: scenario.durationMs,
    cbUrl,
    artifactsDir,
    wire: scenario.wire,  // { url } enables the wire-path mode
  });
  let runErr = null;
  try {
    await s.setup();
    await scenario.run(s);
  } catch (err) {
    runErr = err;
    s.assert(`scenario threw: ${err.message}`, () => false,
        { expected: "no exception", actual: String(err.stack || err) });
  } finally {
    const summary = await s.finish();
    if (runErr) process.exitCode = 1;
    else if (!summary.passed) process.exitCode = 2;
  }
}

export { makeInputBag, makeSessionClient, sleep, MOD };

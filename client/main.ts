// Demo page for chromeless — a thin DOM shell over ChromelessSession.
//
// Track 5 split: the ~350-line answerer state machine that used to
// live in this file (signaling dial, offer/answer/ICE pump, reconnect
// + pc rebuild, stats, probe, teardown) is now
// client/src/session.ts's ChromelessSession, importable by any app
// without dragging this page along. What remains HERE is exactly the
// demo: DOM lookups, the log pane, the status pill, wiring the video
// element, input/cursor/file-upload channel attachment (all DOM-
// coupled by nature), and the T81 passthrough button.
//
// Integration recipe (what a third party copies):
//   const session = new ChromelessSession({ signalingBase });
//   session.on("track", (_t, stream) => video.srcObject = stream);
//   session.on("dataChannel", (dc) => { /* input/cursor/files */ });
//   await session.connect(sessionId);

import { InputChannel } from "./src/input.js";
import { FileUploadChannel, FileUploadError } from "./src/file-upload.js";
import { attachCursorChannel } from "./src/cursor.js";
import { attachControlChannel, type ControlChannelHandle } from "./src/control.js";
import { ClipboardChannel } from "./src/clipboard.js";
import { CameraPassthrough, PassthroughError } from "./src/passthrough.js";
import { resolveSignalingUrl } from "./src/config.js";
import {
  navigate,
  goBack,
  goForward,
  reload,
  currentUrl,
} from "./src/navigate.js";
import {
  ChromelessSession,
  type SessionLogLevel,
  type SessionStatus,
} from "./src/session.js";
import type { ReconnectState } from "./src/reconnect.js";

// OSS-W1 — resolved at load from ?signaling=, then window.__CHROMELESS_CONFIG__
// (injected by infra/compose.yaml's generated config.js), then the built-in
// localhost fallback. See client/src/config.ts.
const DEFAULT_SIGNALING = resolveSignalingUrl();

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
  // T81: webcam/mic passthrough toggle. Disabled until a pc is up.
  passthrough: $<HTMLButtonElement>("passthrough-toggle"),
  // Address bar. Navigation goes over HTTP to the gateway, not over the peer
  // connection — see src/navigate.ts.
  addressBar: $<HTMLFormElement>("addressbar"),
  navUrl: $<HTMLInputElement>("nav-url"),
  navGo: $<HTMLButtonElement>("nav-go"),
  navBack: $<HTMLButtonElement>("nav-back"),
  navForward: $<HTMLButtonElement>("nav-forward"),
  navReload: $<HTMLButtonElement>("nav-reload"),
};

function setStatus(state: SessionStatus, text?: string): void {
  els.status.dataset["state"] = state;
  els.statusText.textContent = text ?? state;
}

function log(level: SessionLogLevel, msg: string, extra?: unknown): void {
  const ts = new Date().toISOString().slice(11, 23);
  const tail = extra === undefined ? "" : "  " + safeStringify(extra);
  const line = document.createElement("div");
  line.innerHTML =
    `<span class="ts">${ts}</span> <span class="lvl-${level}">${level.toUpperCase().padEnd(4)}</span> ${escapeHtml(msg)}${escapeHtml(tail)}`;
  els.log.appendChild(line);
  els.log.scrollTop = els.log.scrollHeight;
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

// Map page coords into the source video's intrinsic pixel space,
// undoing object-fit:contain. The remote expects coords in the source
// coordinate system.
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

// ---------- demo state (DOM-side channel wiring) ----------

// Per-connection DOM attachments the demo owns. The session owns the
// pc/signaling/stats lifecycle; everything here is what touches the
// document, torn down on "pcCreated" (rebuild) and "closed".
interface DemoAttachments {
  detachInput: (() => void) | null;
  cursor: { update(env: unknown): void; dispose(): void } | null;
  detachDrop: (() => void) | null;
  passthrough: CameraPassthrough | null;
  fileUpload: FileUploadChannel | null;
  control: ControlChannelHandle | null;
  detachClipboard: (() => void) | null;
}

let session: ChromelessSession | null = null;
let attach: DemoAttachments = emptyAttachments();

function emptyAttachments(): DemoAttachments {
  return { detachInput: null, cursor: null, detachDrop: null, passthrough: null, fileUpload: null, control: null, detachClipboard: null };
}

function dropAttachments(): void {
  try { attach.detachInput?.(); } catch { /* ignore */ }
  try { attach.detachDrop?.(); } catch { /* ignore */ }
  try { attach.cursor?.dispose(); } catch { /* ignore */ }
  // Drops any open prompt. A dialog left on screen after the peer is gone
  // is a question nobody is listening to.
  try { attach.control?.dispose(); } catch { /* ignore */ }
  // Remove the document-level paste listener. Left attached across a peer
  // rebuild it would keep firing into a closed channel — sendPaste() returns
  // false on a non-open channel, so it would fail silently rather than loudly.
  try { attach.detachClipboard?.(); } catch { /* ignore */ }
  // T81: stop camera/mic so tracks fire "ended" and the user-agent's
  // recording indicator clears; also the §T10 threat-model rule — no
  // silent re-sharing across a rebuild, a fresh click is required.
  try { attach.passthrough?.disable(); } catch { /* ignore */ }
  attach = emptyAttachments();
  els.dc.textContent = "—";
  setPassthroughButtonState("off", true);
}

function wireDataChannel(dc: RTCDataChannel): void {
  log("ok", `wiring data channel "${dc.label}"`);
  if (dc.label === "input") return wireInputChannel(dc);
  if (dc.label === "cursor") return wireCursorChannel(dc);
  if (dc.label === "files") return wireFilesChannel(dc);
  if (dc.label === "control") return wireControlChannel(dc);
  if (dc.label === "clipboard") return wireClipboardChannel(dc);
  log("warn", `ignoring unknown data channel label: ${dc.label}`);
}

function wireClipboardChannel(dc: RTCDataChannel): void {
  // src/clipboard.ts has existed, fully implemented and unit-tested, with NO
  // caller: the demux had arms for input/cursor/files/control and none for
  // clipboard, so the guest opened the channel and this function's absence
  // sent it straight to "ignoring unknown data channel label". Copy and paste
  // could not work in this client no matter what the guest did.
  //
  // Found by tests/interactive/suite_channels once its oracle was tightened
  // to distinguish "wired" from "appeared in the log" — the previous
  // substring check passed happily on the ignore line. Same defect class as
  // the control channel, one file over.
  attach.detachClipboard?.();
  const clip = new ClipboardChannel(dc, {
    onOversize: (bytes) =>
      log("warn", `clipboard: paste too large (${bytes} bytes), dropped`),
    onDropped: (why) => log("warn", `clipboard: dropped (${why})`),
  });
  // document, not els.video: a paste is delivered to whatever has focus and
  // the <video> is not focusable. Listening on the document is what makes
  // Cmd/Ctrl+V work wherever the user's caret happens to be.
  attach.detachClipboard = clip.attach(document);
  dc.addEventListener("close", () => {
    try { attach.detachClipboard?.(); } catch { /* ignore */ }
    attach.detachClipboard = null;
  });
}

function wireCursorChannel(dc: RTCDataChannel): void {
  attach.cursor?.dispose();
  attach.cursor = attachCursorChannel(dc, els.video);
  dc.addEventListener("close", () => {
    try { attach.cursor?.dispose(); } catch { /* ignore */ }
    attach.cursor = null;
  });
}

/**
 * Wire the "control" RTCDataChannel — the guest's ask-a-human path.
 *
 * Until this existed the label fell through to "ignoring unknown data
 * channel label", so every confirm() the streamed page raised was answered
 * by the guest's own safe default and the user never saw it. The channel
 * opened, a warning went to the console, and nothing distinguished that
 * from working.
 */
function wireControlChannel(dc: RTCDataChannel): void {
  attach.control?.dispose();
  attach.control = attachControlChannel(dc, { log });
  dc.addEventListener("close", () => {
    try { attach.control?.dispose(); } catch { /* ignore */ }
    attach.control = null;
  });
}

/**
 * Wire the "files" RTCDataChannel for v1 file uploads (T74). Drop a
 * file onto the page → upload over this channel; the bridge attaches
 * it via DOM.setFileInputFiles using the `data-file-target` selector
 * on the video element (default `input[type=file]`).
 */
function wireFilesChannel(dc: RTCDataChannel): void {
  const fc = new FileUploadChannel(dc);
  attach.fileUpload = fc;

  const onDrop = async (e: DragEvent) => {
    if (!e.dataTransfer || !attach.fileUpload) return;
    const files = Array.from(e.dataTransfer.files);
    if (files.length === 0) return;
    e.preventDefault();
    const targetSelector = els.video.dataset["fileTarget"] ?? "input[type=file]";
    for (const f of files) {
      log("info", `→ file_upload start`, { name: f.name, size: f.size, type: f.type });
      try {
        const handle = attach.fileUpload.uploadFile(f, {
          target_selector: targetSelector,
          onProgress: (p) => log("info", `file_upload progress`,
            `${p.bytes_sent}/${p.bytes_total}`),
        });
        const r = await handle.done;
        log("ok", `← file_upload_complete`, r);
      } catch (err) {
        if (err instanceof FileUploadError) {
          log("err", `file_upload error code=${err.code}`, err.message);
        } else {
          log("err", "file_upload threw", String(err));
        }
      }
    }
  };
  // dragover preventDefault is required for `drop` to fire. Window-
  // level so "drop the PDF onto my cloud browser" needs no precision.
  const onDragOver = (e: DragEvent) => {
    if (e.dataTransfer && Array.from(e.dataTransfer.types).includes("Files")) {
      e.preventDefault();
    }
  };
  window.addEventListener("dragover", onDragOver);
  window.addEventListener("drop", onDrop);
  attach.detachDrop = () => {
    window.removeEventListener("dragover", onDragOver);
    window.removeEventListener("drop", onDrop);
  };
}

function wireInputChannel(dc: RTCDataChannel): void {
  els.dc.textContent = dc.readyState;
  const input = new InputChannel(dc, {
    onCoalesce: (n) => log("info", `coalesced ${n} mouse_move`),
    onError: (err) => log("err", "input send failed", String(err)),
  });

  const attachListeners = () => {
    attach.detachInput = input.attach(els.video, { toContentCoords: videoContentMapper });
  };
  if (dc.readyState === "open") attachListeners();
  else dc.addEventListener("open", attachListeners, { once: true });

  dc.addEventListener("close", () => {
    els.dc.textContent = "closed";
    log("info", "input data-channel closed");
    attach.detachInput?.();
    attach.detachInput = null;
  });
  dc.addEventListener("error", (e) => {
    els.dc.textContent = "error";
    log("err", "input data-channel error", String((e as RTCErrorEvent).error?.message ?? e));
  });
  dc.addEventListener("message", (e) => log("info", "← input.message", e.data));
}

// E2E hook (tests/e2e/03-receives-video-track.spec.ts). Gated on
// `?e2e=1` so production users never get the active PC pinned to
// window; this page is end-user-reachable, so we opt in.
function maybeExposePcForE2e(pc: RTCPeerConnection): void {
  if (new URLSearchParams(location.search).get("e2e") === "1") {
    (window as unknown as { __cbwrtc_pc?: RTCPeerConnection })
      .__cbwrtc_pc = pc;
  }
}

// Which optional consumers this BUNDLE contains. Not which channels the
// guest opened — that is the guest's half and is already visible in the log.
//
// The gateway image bakes the client bundle in at build time, so a client
// fix does not reach a browser until the gateway image is rebuilt AND
// rolled. When those drift, the failure is indistinguishable from a guest
// defect: the `control` channel opens, nothing consumes it, and no dialog
// ever appears. That happened on 2026-08-24 — gateway standalone-v9 was
// published from a branch predating src/control.ts, and the served main.js
// had wireControlChannel=0 while the guest was entirely correct.
//
// A literal list rather than a computed one: it must be wrong at COMPILE
// time if a consumer is removed, not silently true because some symbol
// still happens to exist.
(window as unknown as { __cb_client_consumers?: readonly string[] })
  .__cb_client_consumers = ["input", "cursor", "files", "control", "clipboard"];

// ---------- connect ----------

function connect(sessionId: string): void {
  setStatus("connecting", "ws://");
  els.connect.disabled = true;

  const s = new ChromelessSession({ signalingBase: DEFAULT_SIGNALING });
  session = s;

  s.on("log", log);
  s.on("status", (st, text) => {
    setStatus(st, text);
    if (st === "connected") {
      const enabled = attach.passthrough?.getState().enabled ?? false;
      setPassthroughButtonState(enabled ? "on" : "off", false);
      setNavEnabled(true);
      void syncAddressBar();
    } else if (st === "failed") {
      setPassthroughButtonState("off", true);
      setNavEnabled(false);
    }
  });
  s.on("connectionState", (st) => { els.conn.textContent = st; });
  s.on("signalingState", (st) => { els.sig.textContent = st; });
  s.on("iceConnectionState", (st) => { els.ice.textContent = st; });
  s.on("iceGatheringState", (st) => { els.iceg.textContent = st; });
  s.on("track", (_track, stream) => {
    if (els.video.srcObject !== stream) els.video.srcObject = stream;
  });
  s.on("dataChannel", wireDataChannel);
  s.on("pcCreated", (pc) => {
    // Initial pc AND every reconnect rebuild land here: drop the
    // previous generation's DOM attachments, re-arm the e2e hook.
    dropAttachments();
    setPassthroughButtonState("off", false);
    maybeExposePcForE2e(pc);
  });
  s.on("closed", () => {
    dropAttachments();
    setNavEnabled(false);
    session = null;
    els.connect.disabled = false;
    els.connect.textContent = "Connect";
  });

  void s.connect(sessionId).catch((err) => {
    log("err", "connect failed", String(err));
    s.disconnect("connect failed");
  });
}

// ---------- wire up ----------

els.connect.addEventListener("click", () => {
  if (session) {
    session.disconnect("user clicked");
    return;
  }
  const sessionId = els.sessionId.value.trim() || "dev";
  els.connect.textContent = "Disconnect";
  connect(sessionId);
});

// ---------- address bar ----------
//
// Navigation is a control-plane operation, not a media one: the URL goes over
// HTTP to the gateway, which drives CDP Page.navigate on the worker. The peer
// connection carries only pixels and input. Same split the triform portal uses.

function setNavEnabled(enabled: boolean): void {
  for (const el of [els.navUrl, els.navGo, els.navBack, els.navForward, els.navReload]) {
    el.disabled = !enabled;
  }
}

/** Show what the remote browser is actually on, unless the user is typing. */
async function syncAddressBar(): Promise<void> {
  // Never clobber a half-typed URL. The portal hit this: an async refresh
  // overwriting the input mid-keystroke makes the bar feel broken.
  if (document.activeElement === els.navUrl) return;
  const url = await currentUrl();
  if (url && url !== "about:blank") els.navUrl.value = url;
}

async function runNav(
  action: () => Promise<{ ok: boolean; url?: string; error?: string }>,
  what: string,
): Promise<void> {
  const res = await action();
  if (res.error) {
    log("err", `${what} failed`, res.error);
    return;
  }
  if (!res.ok) {
    // A declined history command — Back with nothing behind it. Not an error;
    // the button is simply a no-op there.
    log("info", `${what}: nothing to do`);
    return;
  }
  if (res.url) els.navUrl.value = res.url;
  log("ok", `${what} → ${res.url ?? "ok"}`);
  // The page needs a moment to commit before /api/current-url reflects it.
  setTimeout(() => { void syncAddressBar(); }, 600);
}

els.addressBar.addEventListener("submit", (ev) => {
  ev.preventDefault();
  const raw = els.navUrl.value;
  if (!raw.trim()) return;
  els.navUrl.blur();
  void runNav(() => navigate(raw), "navigate");
});

// Select-all on focus, the way a real address bar behaves — typing replaces
// the URL rather than appending to it.
els.navUrl.addEventListener("focus", () => { els.navUrl.select(); });

els.navBack.addEventListener("click", () => { void runNav(goBack, "back"); });
els.navForward.addEventListener("click", () => { void runNav(goForward, "forward"); });
els.navReload.addEventListener("click", () => { void runNav(reload, "reload"); });

// ---------- T81: passthrough button ----------

function setPassthroughButtonState(state: "off" | "pending" | "on", disabled: boolean): void {
  els.passthrough.dataset["state"] = state;
  els.passthrough.disabled = disabled;
  els.passthrough.textContent =
    state === "on"      ? "Stop sharing"
    : state === "pending" ? "Requesting…"
    :                     "Share camera/mic";
}

els.passthrough.addEventListener("click", async () => {
  const pc = session?.getPeerConnection();
  if (!session || !pc) return;
  if (attach.passthrough && attach.passthrough.getState().enabled) {
    log("info", "passthrough: user clicked stop");
    attach.passthrough.disable();
    setPassthroughButtonState("off", false);
    return;
  }
  setPassthroughButtonState("pending", true);
  const cp = new CameraPassthrough(pc, {
    onStateChange: (st) => log("info", "passthrough state",
      `enabled=${st.enabled} v=${st.videoTrackActive} a=${st.audioTrackActive}`),
    onNeedRenegotiate: (reason) => {
      // The offerer needs to re-offer with the new m= sections.
      session?.requestRenegotiate(reason);
    },
    onError: (err) => log("err", `passthrough ${err.code}`, err.message),
  });
  try {
    await cp.enable();
    attach.passthrough = cp;
    setPassthroughButtonState("on", false);
    log("ok", "passthrough enabled");
  } catch (err) {
    setPassthroughButtonState("off", false);
    if (err instanceof PassthroughError) {
      log("err", `passthrough enable failed [${err.code}]`, err.message);
    } else {
      log("err", "passthrough enable threw", String(err));
    }
  }
});

// Teardown on BOTH events, because `beforeunload` is not reliable.
//
// It does not fire when a browser context is closed programmatically (which is
// what Playwright does between specs), and on mobile/bfcache paths it is
// skipped entirely. Measured 2026-08-20 across a full e2e run: the broker
// received ZERO `bye` envelopes, so the worker was never told the session had
// ended — it sat on a peer connection that rotted connected -> disconnected ->
// failed, and never re-offered for the next viewer.
//
// `pagehide` DOES fire on programmatic close and on bfcache eviction, and is
// the modern recommendation. Both are registered: beforeunload still covers
// the "user confirms navigation away" case, and disconnect() is idempotent
// (teardown() early-returns once rws and pc are null), so a double fire is
// harmless.
const teardown = (why: string) => session?.disconnect(why);
window.addEventListener("pagehide", () => teardown("page hidden"));
window.addEventListener("beforeunload", () => teardown("page unload"));

// ---------- start automatically ----------
//
// A user who has just logged in expects a working browser, not a button. The
// Connect button stays (it doubles as Disconnect, and reconnecting by hand is
// genuinely useful when a session goes wrong), but nobody should have to press
// it to get the thing they asked for.
//
// Deliberately NOT gated on any query flag. `?e2e=1` exists only to expose the
// PeerConnection to tests, and gating behaviour on it is precisely how the
// suite ended up green while the page a real user loads was broken.
//
// Failures surface exactly as before — connect() reports through the status
// pill and the log — so an auto-start that cannot reach the worker looks the
// same as a hand-clicked one that cannot.
function autoConnect(): void {
  if (session) return;                    // already up (e.g. hot reload)
  const sessionId = els.sessionId.value.trim() || "dev";
  els.connect.textContent = "Disconnect";
  connect(sessionId);
}

log("info", "client loaded — connecting…");
autoConnect();

// Part of the public surface via the session's signaling event.
export type { ReconnectState };

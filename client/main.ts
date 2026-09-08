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

import { InputChannel, keyBelongsToClient } from "./src/input.js";
import { summarise, formatResolution } from "./src/hud.js";
import { FileUploadChannel, FileUploadError } from "./src/file-upload.js";
import { attachCursorChannel } from "./src/cursor.js";
import {
  attachControlChannel,
  type ControlChannelHandle,
  type FileChooserRequest,
} from "./src/control.js";
import { ClipboardChannel } from "./src/clipboard.js";
import { CameraPassthrough, PassthroughError } from "./src/passthrough.js";
import { resolveSignalingUrl } from "./src/config.js";
import { followViewport, type ViewportFollower } from "./src/viewport.js";
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
  // The element the video FILLS. The viewport follower watches this, not the
  // <video>, whose box tracks the stream's own aspect ratio (src/viewport.ts).
  stage: $<HTMLElement>("stage"),
  log: $<HTMLPreElement>("log"),
  sig: $<HTMLElement>("state-sig"),
  ice: $<HTMLElement>("state-ice"),
  iceg: $<HTMLElement>("state-iceg"),
  conn: $<HTMLElement>("state-conn"),
  dc: $<HTMLElement>("state-dc"),
  // T81: webcam/mic passthrough toggle. Disabled until a pc is up.
  passthrough: $<HTMLButtonElement>("passthrough-toggle"),
  // Unmute. The guest sends audio from the first session; until this
  // existed the <video> was hard-muted with no control, so it was decoded
  // and discarded.
  audio: $<HTMLButtonElement>("audio-toggle"),
  fullscreen: $<HTMLButtonElement>("fullscreen-toggle"),
  hudDot: $<HTMLElement>("hud-dot"),
  hudQuality: $<HTMLElement>("hud-quality"),
  hudFps: $<HTMLElement>("hud-fps"),
  hudBitrate: $<HTMLElement>("hud-bitrate"),
  hudRtt: $<HTMLElement>("hud-rtt"),
  hudLoss: $<HTMLElement>("hud-loss"),
  hudRes: $<HTMLElement>("hud-res"),
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
// Focus in one of our own controls means the key is ours, not the cloud
// browser's. The rule itself lives in src/input.ts (pure, and tested).
function shouldForwardKey(e: KeyboardEvent): boolean {
  return !keyBelongsToClient(e.target as HTMLElement | null);
}

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

// ---------- viewport follows the window ----------
//
// The remote browser is resized to match the stage, so the stream fills the
// window instead of sitting in a 1280x720 letterbox. Process-lifetime: the
// stage exists for the life of the page, and the follower only sends while a
// session is connected (it is armed on "connected" and re-armed on every
// reconnect, since a fresh guest may have booted at the default size).
const viewport: ViewportFollower = followViewport(els.stage, {
  onResult: (r, requested) => {
    if (!r.ok) {
      // One line, then silence: the follower stops itself. The common cause is
      // a guest image that predates Cb.setViewport, which is a fact about the
      // deployment, not something to retry on every resize.
      log("warn", `viewport: remote resize unavailable (${r.error}); the stream stays at the guest's size`);
      return;
    }
    const a = r.applied;
    if (r.clamped) {
      log("info", `viewport: asked ${requested.width}x${requested.height}, guest applied ${a.width}x${a.height}`);
    } else {
      log("info", `viewport: ${a.width}x${a.height}`);
    }
  },
});

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
  // Nothing to hear between sessions. The MUTE STATE is left alone on
  // purpose — a reconnect must not silently re-mute a user who unmuted, so
  // only the control is disabled, not the preference behind it.
  els.audio.disabled = true;
  els.fullscreen.disabled = true;
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
  attach.control = attachControlChannel(dc, {
    log,
    onFileChooser: pickAndUpload,
    onEvent: onGuestEvent,
  });
  dc.addEventListener("close", () => {
    try { attach.control?.dispose(); } catch { /* ignore */ }
    attach.control = null;
  });
}

/**
 * A fire-and-forget notice from the guest.
 *
 * `fullscreen_changed` is the load-bearing one: the streamed PAGE called
 * requestFullscreen (a video player, a game) and chromium granted it, so
 * the page's :fullscreen CSS applies and its layout has already changed.
 * If the viewer's chrome does not follow, the page is fullscreen inside a
 * window that still shows a 380px sidebar — the guest emitted this event
 * from the day it was written and nothing ever listened.
 *
 * Requesting fullscreen here can be refused: the browser wants a user
 * gesture and a network event is not one. setFullscreen() logs the refusal
 * rather than pretending, and the page is no worse off than before.
 */
function onGuestEvent(kind: string, data: Record<string, unknown>): void {
  if (kind === "fullscreen_changed") {
    void setFullscreen(data["fullscreen"] === true);
    return;
  }
  // tab_opened / tab_closed are advisory; the gateway's /json list is the
  // source of truth and the tab strip is not built yet. Logged by
  // control.ts already, so nothing to add here.
}

/**
 * The streamed page opened <input type=file>. Show the viewer a real file
 * picker, then send the chosen file over the "files" channel — the guest
 * hands it to the page's own input.
 *
 * Returns true once the upload has STARTED, which is the answer the guest
 * needs: "a file is coming, keep the chooser parked". We do not wait for the
 * upload to finish, because a 100 MB file over a slow link would otherwise
 * hold the control request open past its deadline and the guest would cancel
 * a chooser that was about to be satisfied.
 *
 * Why a click-triggered <input> and not a drop zone: browsers only open a
 * file dialog from a user gesture, and this call arrives from the network.
 * So we show our own button and let the viewer's click be the gesture.
 */
async function pickAndUpload(req: FileChooserRequest): Promise<boolean> {
  const fc = attach.fileUpload;
  if (!fc) {
    log("warn", "file_chooser: no files channel — declining");
    return false;
  }
  const file = await promptForFile(req);
  if (!file) {
    log("info", "file_chooser: viewer chose nothing");
    return false;
  }
  log("info", "→ file_upload start (page chooser)", { name: file.name, size: file.size });
  const handle = fc.uploadFile(file);
  // Report the outcome, but do not make the guest wait for it.
  void handle.done.then(
    (r) => log("ok", "← file_upload_complete", r),
    (err) => log("err", "file_upload failed", err instanceof FileUploadError
      ? `code=${err.code} ${err.message}` : String(err)),
  );
  return true;
}

/**
 * Mount a one-shot "the page wants a file" bar and resolve with what the
 * viewer picks, or null if they dismiss it.
 *
 * ALWAYS resolves. A bar that could resolve zero times would leave the page
 * blocked until the guest's five-minute deadline, with nothing on screen to
 * explain it.
 */
function promptForFile(req: FileChooserRequest): Promise<File | null> {
  return new Promise((resolve) => {
    const bar = document.createElement("div");
    bar.className = "cb-filepick";
    const label = document.createElement("span");
    label.className = "cb-filepick-label";
    label.textContent = req.title || "The page is asking for a file";
    const input = document.createElement("input");
    input.type = "file";
    input.className = "cb-filepick-input";
    if (req.accept.length > 0) input.accept = req.accept.join(",");
    const cancel = document.createElement("button");
    cancel.type = "button";
    cancel.className = "cb-filepick-cancel";
    cancel.textContent = "Cancel";

    let settled = false;
    const finish = (f: File | null): void => {
      if (settled) return;
      settled = true;
      try { bar.remove(); } catch { /* ignore */ }
      resolve(f);
    };
    input.addEventListener("change", () => finish(input.files?.[0] ?? null));
    cancel.addEventListener("click", () => finish(null));

    bar.append(label, input, cancel);
    document.body.appendChild(bar);
    // Chrome will not open the dialog without a gesture, so this is an
    // affordance the viewer clicks, not an input we can click for them.
    input.focus();
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
    attach.detachInput = input.attach(els.video, {
      toContentCoords: videoContentMapper,
      shouldForwardKey,
    });
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
      // Only offer sound once there is a session to hear. The mute state
      // itself is deliberately NOT reset on reconnect: a user who unmuted
      // should not be re-muted by a broker blip.
      els.audio.disabled = false;
      syncAudioButton();
      els.fullscreen.disabled = false;
      syncFullscreenButton();
      // The button reads "Disconnect" from the moment connect() ran, but it
      // was left DISABLED until `closed` — so it could never be pressed while
      // a session existed, and the `if (session)` branch of its click handler
      // was dead code. Auto-connect made that visible: the only button on
      // the page was greyed out for the whole session. Usable from here on.
      els.connect.disabled = false;
      void syncAddressBar();
      // Size the remote to the stage NOW, and allow requests again if an
      // earlier guest had refused them — a reconnect may land on a newer one.
      viewport.resume();
      viewport.sync();
    } else if (st === "failed") {
      setPassthroughButtonState("off", true);
      setNavEnabled(false);
      els.audio.disabled = true;
      // A failed connect used to leave a disabled "Disconnect" and no way
      // back but a reload. Let the user end it and try again.
      els.connect.disabled = false;
    }
  });
  s.on("connectionState", (st) => { els.conn.textContent = st; });
  s.on("signalingState", (st) => { els.sig.textContent = st; });
  s.on("iceConnectionState", (st) => { els.ice.textContent = st; });
  s.on("iceGatheringState", (st) => { els.iceg.textContent = st; });
  // The `stats` event has fired once a second since T82 with NOBODY
  // listening — the sample was assembled, shipped to the broker over the
  // stats channel, and dropped locally. src/hud.ts turns it into five
  // numbers; this renders them.
  s.on("stats", (sample, prev) => {
    const h = summarise(sample, prev);
    els.hudFps.textContent = h.fps;
    els.hudBitrate.textContent = h.bitrate;
    els.hudRtt.textContent = h.rtt;
    els.hudLoss.textContent = h.loss;
    els.hudQuality.textContent = h.quality;
    els.hudDot.dataset["q"] = h.quality;
    // Resolution comes off the <video>, not getStats: videoWidth/Height is
    // what is actually being PAINTED, which is the number a user can check
    // against their own window. getStats reports the decoder's frame size,
    // which can differ mid-renegotiation.
    els.hudRes.textContent =
      formatResolution(els.video.videoWidth, els.video.videoHeight);
  });

  s.on("track", (_track, stream) => {
    if (els.video.srcObject !== stream) els.video.srcObject = stream;
  });
  s.on("dataChannel", wireDataChannel);
  s.on("pcCreated", (pc) => {
    // Initial pc AND every reconnect rebuild land here: drop the
    // previous generation's DOM attachments, re-arm the e2e hook.
    //
    // The passthrough button stays DISABLED here (dropAttachments leaves it
    // so) and is enabled by the "connected" status above — not by the
    // existence of a PeerConnection. Enabling it on pcCreated let a user add
    // camera tracks to a PeerConnection that was still negotiating, with no
    // session to renegotiate on; with connect-on-load that window opened on
    // every page load, and tests/e2e/06 ("disabled until a peer connection
    // is up") went red because the contract in its name was never the one
    // the code implemented.
    dropAttachments();
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

/**
 * Mute state lives on the <video>, not in a variable — the element is the
 * single source of truth, and a user who mutes via the browser's own media
 * controls must not leave the button lying.
 */
function syncAudioButton(): void {
  const muted = els.video.muted || els.video.volume === 0;
  els.audio.dataset["state"] = muted ? "muted" : "on";
  els.audio.textContent = muted ? "🔇 Unmute" : "🔊 Mute";
  els.audio.title = muted
    ? "Unmute the cloud browser's audio"
    : "Mute the cloud browser's audio";
}

els.audio.addEventListener("click", () => {
  const v = els.video;
  const unmuting = v.muted || v.volume === 0;
  v.muted = !unmuting;
  if (unmuting && v.volume === 0) v.volume = 1;
  syncAudioButton();
  // Unmuting can require a fresh play(): a stream that began muted may be
  // paused by the autoplay policy the instant it gains an audible track.
  // This click IS the user gesture that makes the retry allowed, so it has
  // to happen here and not on a later tick.
  if (unmuting) {
    void v.play().catch((err: unknown) => {
      log("warn", "audio: the browser refused to play with sound", String(err));
      // Leave the button honest rather than claiming sound the user
      // cannot hear.
      v.muted = true;
      syncAudioButton();
    });
  }
  log("info", `audio: ${unmuting ? "unmuted" : "muted"}`);
});

// The element can be muted from outside our button (the browser's own
// media controls, or another script). Keep the label truthful.
els.video.addEventListener("volumechange", syncAudioButton);

/**
 * Fullscreen the stage. The label follows the DOCUMENT's state rather than
 * our own flag, because the user can leave fullscreen with Escape or the
 * browser's own control and never touch this button.
 */
function syncFullscreenButton(): void {
  const on = document.fullscreenElement === els.stage;
  els.fullscreen.dataset["state"] = on ? "on" : "off";
  els.fullscreen.textContent = on ? "⛶ Exit fullscreen" : "⛶ Fullscreen";
}

async function setFullscreen(want: boolean): Promise<void> {
  try {
    if (want && document.fullscreenElement !== els.stage) {
      await els.stage.requestFullscreen();
    } else if (!want && document.fullscreenElement) {
      await document.exitFullscreen();
    }
  } catch (err) {
    // Refused (no gesture, an iframe without allowfullscreen, a platform
    // that does not do it). Say so — silence here looks like a dead button.
    log("warn", "fullscreen: the browser refused", String(err));
  }
  // Whatever happened, the label must match reality.
  syncFullscreenButton();
  // The stage just changed size; the remote should follow it.
  viewport.sync();
}

els.fullscreen.addEventListener("click", () => {
  void setFullscreen(document.fullscreenElement !== els.stage);
});
document.addEventListener("fullscreenchange", () => {
  syncFullscreenButton();
  viewport.sync();
});

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

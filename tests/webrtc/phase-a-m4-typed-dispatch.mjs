#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
//
// CV2-81 M4 typed-dispatcher composite-delegate verification harness.
//
// Closes the verification loop for CV2-81 commit 07e66f0 on branch
// cv2/m4-typed-dispatcher-runtime-wire (merged into cv2/wave-2-integration
// at 297a7dd). Asserts that the CbInputDispatchCompositeDelegate (the
// transitional fan-out class that replaced CV2-75 R1's
// CbInputLoggingDelegate stand-in) is actually instantiated at boot and
// receives + fans envelopes from the M4 R1 input DC into all six M4
// R3..R8 typed dispatchers (R3 mouse, R4 keyboard, R5 IME, R6 touch,
// R7 drag, R8 clipboard).
//
// Verdict-design discipline (single-variable axes, matches CV2-72)
// =================================================================
//
// This harness is a HYBRID of phase-a-m4-r1-input-dispatch.mjs (WebRTC
// peer with input-DC envelope send) and phase-a-m5-r1-percursor-event.mjs
// (CDP attach with `local: true` for renderer-side observability). It
// fires six envelopes (one per typed-dispatcher class) over the input DC,
// then leaves the verdict-derivation to the orchestrator's kubectl-logs
// grep — same split-axes convention as the two reference harnesses.
//
// Empirically-verified LOG anchors (from 07e66f0 source files)
// -------------------------------------------------------------
//
//   CONSTRUCTION (one-time at PreMainMessageLoopRun):
//     [cb_input_dispatch_composite.cc:60]
//       LOG(INFO) << "CV2-81: CbInputDispatchCompositeDelegate ctor — 6 "
//                    "typed dispatchers (R3 mouse, R4 keyboard, R5 IME, "
//                    "R6 touch, R7 drag, R8 clipboard) wired";
//
//   MAIN-PARTS WIRING (one-time at DC create):
//     [cloud_browser_browser_main_parts.cc:697]
//       LOG(INFO) << "CV2-81: \"input\" DC observer = CbInputDispatch "
//                    "(composite delegate = R3..R8 typed pipeline)";
//
//   PRE-CV2-81 BASELINE (must be ABSENT post-CV2-81):
//     [cb_input_dispatch.cc:307 — inside CbInputLoggingDelegate::OnInputEvent]
//       LOG(INFO) << "CbInputDispatch: dispatched type=" << envelope.type ...
//     The CbInputLoggingDelegate is replaced by CbInputDispatchComposite
//     under CV2-81, so this line is gone in production runtime. Its
//     ABSENCE is itself a CV2-81-landed signal (negative-evidence anchor,
//     same pattern as phase-a-m5.5-r0-pulse-config.mjs).
//
// Per-dispatch SUCCESS is SILENT — typed dispatchers (R3..R8) emit
// LOG(WARNING) ONLY on dispatch FAILURE (no resolver / no active
// WebContents / no RWH / missing fields). Successful dispatch produces
// no INFO line per envelope; the chromium event-injection path is silent
// by design — see e.g. cb_input_dispatch_mouse.cc:144-200 which logs only
// on the early-return branches.
//
// Verdict (derived externally — orchestrator greps cb-chromium stderr)
// ---------------------------------------------------------------------
//
// PASS shape (anchored evidence required + negative evidence required):
//   ANCHOR-A   composite construction LOG PRESENT exactly once
//                ("CV2-81: CbInputDispatchCompositeDelegate ctor")
//   ANCHOR-B   main-parts wiring LOG PRESENT exactly once
//                ("CV2-81: \"input\" DC observer = CbInputDispatch
//                  (composite delegate = R3..R8 typed pipeline)")
//   NEG-C      ABSENCE of pre-CV2-81 baseline line for the six envelopes
//                this harness sends ("CbInputDispatch: dispatched type=")
//   NEG-D      ABSENCE of WARNING lines for the six envelopes
//                (e.g. "CbInputDispatchMouse: mouse_move dropped",
//                 "CbInputDispatchKeyboard: key_down dropped",
//                 "cb-ime: composition_start dropped",
//                 "touch_start: no active WebContents",
//                 "CbInputDispatchDrag: drag_start dropped",
//                 "CbInputDispatchClipboard: clipboard_copy_request dropped")
//
// FAIL classes (each maps to a distinct grep signature):
//   F1   ANCHOR-A absent     → CV2-81 not landed in this image; composite
//                              never constructed
//   F2   ANCHOR-A present, ANCHOR-B absent → composite built but main_parts
//                                              didn't bind it as DC observer
//                                              (wiring regression at the
//                                              DC-observer registration site)
//   F3   NEG-C violated (CbInputDispatch: dispatched type= present)
//                              → CV2-75 R1 logging delegate still wired,
//                                CV2-81 swap reverted somewhere
//   F4   NEG-D violated on a specific dispatcher → that dispatcher reached
//                                                   chromium-side but dropped
//                                                   the envelope (resolver
//                                                   not threaded, WebContents
//                                                   not active at dispatch
//                                                   moment, envelope field
//                                                   shape mismatch). Maps
//                                                   1:1 to dispatcher.
//   F5   NEG-D shows DIFFERENT typed dispatcher's WARNING for a non-claimed
//        envelope type (e.g. CbInputDispatchKeyboard warning while we sent
//        only mouse) → type-switch logic broken in that dispatcher; envelope
//        leaked into another dispatcher's payload-handler.
//
// HALT classes (image-level regression, not CV2-81 defect):
//   H1   handshake timeout (expected native DCs don't all open) →
//        CV2-77/CV2-83 DC-creation regression
//        regression (route to build-czar). Same as M4 R1 harness exit 2.
//   H2   no input DC → CV2-77 input DC name regression (route to build-czar)
//   H3   CDP attach fail → CV2-87 / SwANGLE-era CDP unavailability (route
//        to functional-test-lead for image-level triage)
//
// R7 (drag) partial coverage — Option B per CV2-81 NON-GOAL
// ----------------------------------------------------------
//
// R7's chromium-side rwh->DragTarget*() calls are deferred to CV2-82
// (commented-out at cb_input_dispatch_drag.cc:248..253 + 5 other sites
// with TODO(M4-R7-rwh-api)). The state machine FULLY processes the
// drag_start envelope — coord-mapping, EnsureBroughtToFront, phase
// transitions, payload caching — but does NOT actually call DragTarget*
// on chromium. Therefore:
//
//   * This harness's drag_start IS exercised end-to-end at the typed-
//     dispatcher OnInputEvent level (NEG-D applies — no
//     "CbInputDispatchDrag: drag_start dropped" line).
//   * The renderer side WILL NOT receive a `dragstart` DOM event today
//     because the Phase 2 chromium-API wiring isn't landed. So a CDP
//     Runtime.evaluate(document.lastDragEvent) check would FAIL not
//     because R7 is broken, but because CV2-82 is unfinished.
//   * The harness therefore intentionally STOPS at the typed-dispatcher
//     OnInputEvent boundary for R7 (NEG-D coverage only) and does not
//     attempt to assert renderer-side drag DOM observation.
//
// CDP role (limited — sanity check only)
// ---------------------------------------
//
// CDP is used solely to attach to a non-blank renderer target so the
// six typed dispatchers have a non-null active WebContents to dispatch
// into. Without an active WebContents, R3..R8 take their early-return
// LOG(WARNING) branch and NEG-D fails universally. The CDP attach uses
// the `local: true` pattern from phase-a-m5-r1-percursor-event.mjs to
// dodge the /json/protocol CHECK-FATAL at devtools_http_handler.cc:724
// (latent path; see CV2-78 verification re-fire docstring).
//
// CDP also does a Runtime.evaluate(document.body.innerHTML) sanity-check
// after navigation so the harness can distinguish "renderer-DOM empty"
// (HALT H3 — CV2-89 packaging fix didn't land OR deeper-ring regression)
// from "renderer-DOM present but typed-dispatcher dropped envelope"
// (FAIL F4).
//
// Lesson-(g) probe-semantics applied
// ------------------------------------
//
// All LOG strings verified verbatim against commit 07e66f0 source files
// (not from memory). Envelope shapes verified against the documented
// protocol in input-channel.md AND cb_input_dispatch.cc:44-67's
// kKnownInputTypes list AND each typed dispatcher's data.Find* call site.
//
// Envelope types this harness exercises (one per typed-dispatcher class):
//   R3 mouse:    "mouse_move"     (x, y)
//   R4 keyboard: "key_down"       (key, code, mods)
//   R5 IME:      "composition_start" (text)
//   R6 touch:    "touch_start"    (identifier, x, y)
//   R7 drag:     "drag_start"     (x, y, types, items)  — see Option B note
//   R8 clipboard: "clipboard_copy_request"  (no required fields)
//
// All six type names verified against cb_input_dispatch.cc:44-67's
// alphabetically-sorted kKnownInputTypes constexpr array.

import WebSocket from "ws";
import wrtc from "@roamhq/wrtc";
import CDP from "chrome-remote-interface";

const { RTCPeerConnection, RTCIceCandidate, RTCSessionDescription } = wrtc;

const BROKER_URL = process.env.BROKER_URL
  || "ws://localhost:8080/api/webrtc/signaling/cv2-81-m4-typed-dispatch-001";
const HANDSHAKE_TIMEOUT_MS = parseInt(process.env.HANDSHAKE_TIMEOUT_MS || "60000", 10);
const POST_SEND_WAIT_MS = parseInt(process.env.POST_SEND_WAIT_MS || "4000", 10);
const CDP_HOST = process.env.CDP_HOST || "localhost";
const CDP_PORT = parseInt(process.env.CDP_PORT || "9222", 10);
const CDP_CONNECT_TIMEOUT_MS = parseInt(process.env.CDP_CONNECT_TIMEOUT_MS || "60000", 10);
// The HTML payload of a `data:text/html,` URL MUST be percent-encoded: the
// raw `#` in `href="#"` is otherwise parsed as the URL fragment delimiter,
// truncating the document at `<a href="#` — the renderer lays out an empty
// body and the renderer-DOM probe reports a false HALT H3. encodeURIComponent
// escapes `#` `<` `>` `"` and spaces so the whole payload stays inside the
// data: URL. (Wave 2 rv8 verification finding — same fix as
// phase-a-m5-r1-percursor-event.mjs.)
const STIMULUS_TARGET_HTML =
  '<html><body style="margin:0">'
  + '<a href="#" style="display:inline-block;padding:10px 20px;font-size:24px">link</a>'
  + '<input id="t" autofocus style="position:absolute;top:80px;left:10px;width:200px;height:30px"/>'
  + '</body></html>';
const STIMULUS_TARGET_URL = process.env.STIMULUS_TARGET_URL
  || ("data:text/html," + encodeURIComponent(STIMULUS_TARGET_HTML));
const EXPECTED_LABELS = Object.freeze(["input", "stats", "cursor", "clipboard", "files"]);

function log(level, msg, extra) {
  const line = { ts: new Date().toISOString(), level, msg, ...(extra || {}) };
  console.log(JSON.stringify(line));
}

// Build the six envelopes the harness will send over the input DC.
// Each envelope's `type` is one of kKnownInputTypes (cb_input_dispatch.cc:44).
// `t` is set to 1 — see phase-a-m4-r1-input-dispatch.mjs:152 for the
// int32 overflow note (cb_input_dispatch.cc:160 uses FindInt which is
// int32-only; TODO(M4-R1-int64-timestamps) acknowledges).
//
// `seq` is a per-harness monotonic counter so the orchestrator can
// disambiguate the six envelopes in stderr if multiple WARNINGs fire.
function buildEnvelopes() {
  return [
    // R3 mouse: protocol shape per cb_input_dispatch_mouse.cc:183
    //   data.FindInt("x"), data.FindInt("y") — both required.
    { v: 1, type: "mouse_move", t: 1, seq: 1,
      data: { x: 50, y: 50 } },

    // R4 keyboard: protocol shape per cb_input_dispatch_keyboard.cc:89
    //   data.FindString("key") OR data.FindString("code") (at least
    //   one required), data.FindInt("mods") (optional).
    { v: 1, type: "key_down", t: 1, seq: 2,
      data: { key: "A", code: "KeyA", mods: 0 } },

    // R5 IME: protocol shape per cb_input_dispatch_ime.cc:147 (ReadDataAsU16)
    //   data carries composition string; selection_start/_end optional.
    //   composition_start typically has empty text per the source comment.
    { v: 1, type: "composition_start", t: 1, seq: 3,
      data: { text: "" } },

    // R6 touch: protocol shape per cb_input_dispatch_touch.cc:147
    //   data needs identifier + x + y (ParseTouchPointData). The
    //   identifier is the blink touch-point id (0-based; matches the
    //   Go bridge's contract).
    { v: 1, type: "touch_start", t: 1, seq: 4,
      data: { identifier: 0, x: 50, y: 50 } },

    // R7 drag: protocol shape per cb_input_dispatch_drag.cc:209
    //   data.FindInt("x"), data.FindInt("y") required;
    //   items array per the BuildDropData walker (cb_input_dispatch_drag.cc:88-130).
    //   Note: the rwh->DragTarget* calls are commented out (CV2-82 follow-up;
    //   see NON-GOAL banner at cc:30-58). This envelope exercises the
    //   state machine end-to-end; the chromium-side dispatch is deferred.
    { v: 1, type: "drag_start", t: 1, seq: 5,
      data: { x: 100, y: 100, types: ["text/plain"],
              items: [{ kind: "string", type: "text/plain", data: "hello" }] } },

    // R8 clipboard: protocol shape per cb_input_dispatch_clipboard.cc:42
    //   clipboard_copy_request needs no required fields; the dispatcher
    //   synthesises a Ctrl+C against the focused renderer.
    { v: 1, type: "clipboard_copy_request", t: 1, seq: 6,
      data: {} },
  ];
}

async function waitForCdpReady() {
  const deadline = Date.now() + CDP_CONNECT_TIMEOUT_MS;
  let lastErr;
  while (Date.now() < deadline) {
    try {
      const targets = await CDP.List({ host: CDP_HOST, port: CDP_PORT });
      if (targets && targets.length > 0) {
        log("ok", "cb-chromium CDP up", { targets: targets.length });
        return targets;
      }
      lastErr = new Error("CDP up but no targets");
    } catch (e) {
      lastErr = e;
    }
    await new Promise((r) => setTimeout(r, 500));
  }
  throw new Error(`CDP not ready after ${CDP_CONNECT_TIMEOUT_MS}ms: ${lastErr}`);
}

async function ensureRendererPresent() {
  // Renderer-DOM sanity check: distinguish HALT H3 (no DOM, CV2-89
  // packaging regression) from FAIL F4 (DOM present, dispatcher dropped).
  // Uses `local: true` per phase-a-m5-r1-percursor-event.mjs:116 to
  // avoid the /json/protocol CHECK-FATAL latent path.
  let client;
  try {
    client = await CDP({ host: CDP_HOST, port: CDP_PORT, local: true });
  } catch (e) {
    log("err", "HALT H3: CDP attach FAIL", { err: String(e) });
    return { halt: "H3", reason: String(e) };
  }

  const { Page, Runtime } = client;
  try {
    await Page.enable();
    await Runtime.enable();

    const navStart = Date.now();
    await Page.navigate({ url: STIMULUS_TARGET_URL });
    await Page.loadEventFired();
    log("ok", "navigation complete", { nav_ms: Date.now() - navStart });
    // Brief settle for layout/style pass + focus on the autofocus input
    // (gives R4/R5/R8 a focused text field for their dispatch to land on).
    await new Promise((r) => setTimeout(r, 300));

    // DOM-empty check.
    const domResult = await Runtime.evaluate({
      expression: "(() => ({ body_len: document.body ? document.body.innerHTML.length : 0, has_link: !!document.querySelector('a'), has_input: !!document.querySelector('input') }))()",
      returnByValue: true,
    });
    const dom = domResult.result?.value || {};
    log("info", "renderer-DOM probe", dom);
    if (!dom.body_len || dom.body_len === 0) {
      log("err", "HALT H3: renderer-DOM empty", { dom });
      try { await client.close(); } catch {}
      return { halt: "H3", reason: "renderer-DOM empty post-navigation" };
    }

    try {
      const captureResult = await client.send("Cb.startFrameSinkCapture");
      log("ok", "Cb.startFrameSinkCapture complete", captureResult || {});
    } catch (e) {
      log("err", "HALT H3: Cb.startFrameSinkCapture failed", { err: String(e) });
      try { await client.close(); } catch {}
      return { halt: "H3", reason: `Cb.startFrameSinkCapture failed: ${String(e)}` };
    }

    try { await client.close(); } catch {}
    return { ok: true };
  } catch (e) {
    log("err", "HALT H3: renderer probe threw", { err: String(e), stack: e?.stack });
    try { await client.close(); } catch {}
    return { halt: "H3", reason: String(e) };
  }
}

async function main() {
  log("info", "cv2-81 m4-typed-dispatch test start", {
    broker: BROKER_URL,
    handshake_timeout_ms: HANDSHAKE_TIMEOUT_MS,
    post_send_wait_ms: POST_SEND_WAIT_MS,
    cdp: `${CDP_HOST}:${CDP_PORT}`,
    expected_labels: EXPECTED_LABELS,
    stimulus_url_preview: STIMULUS_TARGET_URL.slice(0, 80),
  });

  // ───── Phase 0: CDP up + non-blank renderer (HALT H3 gate) ─────
  const t0 = Date.now();
  try {
    await waitForCdpReady();
  } catch (e) {
    log("err", "HALT H3: CDP not ready", { err: String(e) });
    process.exit(5);
  }
  log("info", "cdp ready", { wait_ms: Date.now() - t0 });

  const dom = await ensureRendererPresent();
  if (dom.halt) {
    log("err", "VERDICT: HALT", { class: dom.halt, reason: dom.reason });
    process.exit(5);
  }

  // ───── Phase 1: WebRTC handshake (native DC set opens) ─────
  const ws = new WebSocket(BROKER_URL);
  const pc = new RTCPeerConnection({ iceServers: [] });

  const openedLabels = new Set();
  const dcs = new Map();
  let resolveHandshake, rejectHandshake;
  const handshakeDone = new Promise((res, rej) => { resolveHandshake = res; rejectHandshake = rej; });

  const handshakeTimeout = setTimeout(() => {
    rejectHandshake(new Error(`handshake timeout ${HANDSHAKE_TIMEOUT_MS}ms; opened=[${[...openedLabels].join(",")}]`));
  }, HANDSHAKE_TIMEOUT_MS);

  function send(env) {
    if (ws.readyState !== WebSocket.OPEN) return;
    ws.send(JSON.stringify(env));
  }

  pc.onicecandidate = (ev) => {
    if (!ev.candidate) { send({ type: "ice", from: "client", data: null }); return; }
    send({ type: "ice", from: "client", data: ev.candidate.toJSON() });
  };
  pc.oniceconnectionstatechange = () => log("info", `iceConnectionState=${pc.iceConnectionState}`);
  pc.onconnectionstatechange = () => log("info", `connectionState=${pc.connectionState}`);

  pc.ondatachannel = (ev) => {
    const dc = ev.channel;
    log("info", "ondatachannel", { label: dc.label, readyState: dc.readyState });
    dcs.set(dc.label, dc);
    const onOpen = () => {
      if (openedLabels.has(dc.label)) return;
      openedLabels.add(dc.label);
      log("ok", `DC.onopen "${dc.label}"`, { opened: [...openedLabels] });
      if (EXPECTED_LABELS.every((l) => openedLabels.has(l))) {
        log("ok", "handshake complete — all expected DCs open", {
          expected_labels: EXPECTED_LABELS,
        });
        clearTimeout(handshakeTimeout);
        resolveHandshake();
      }
    };
    if (dc.readyState === "open") onOpen();
    dc.onopen = onOpen;
    dc.onerror = (err) => log("err", `DC.onerror "${dc.label}"`, { err: String(err?.error || err) });
  };

  ws.on("open", () => send({ type: "hello", from: "client" }));
  ws.on("message", async (raw) => {
    let env;
    try { env = JSON.parse(raw.toString("utf8")); } catch { return; }
    if (env.type === "offer") {
      const sdp = env.data?.sdp;
      if (!sdp) return;
      log("info", "offer received", { sdp_len: sdp.length });
      await pc.setRemoteDescription(new RTCSessionDescription({ type: "offer", sdp }));
      const answer = await pc.createAnswer();
      await pc.setLocalDescription(answer);
      send({ type: "answer", from: "client", data: { type: "answer", sdp: answer.sdp } });
    } else if (env.type === "ice") {
      if (env.data === null || env.data === undefined) {
        try { await pc.addIceCandidate(null); } catch {}
        return;
      }
      try { await pc.addIceCandidate(new RTCIceCandidate(env.data)); } catch (e) {
        log("warn", "addIceCandidate failed", { err: String(e) });
      }
    }
  });
  ws.on("error", (err) => { log("err", "ws error", { err: String(err) }); rejectHandshake(err); });

  try {
    await handshakeDone;
  } catch (e) {
    log("err", "VERDICT: HALT H1 — handshake failed", { err: String(e) });
    try { pc.close(); ws.close(1011); } catch {}
    process.exit(2);
  }

  // ───── Phase 2: locate input DC ─────
  const inputDc = dcs.get("input");
  if (!inputDc) {
    log("err", "VERDICT: HALT H2 — no input DC", {});
    process.exit(3);
  }

  // ───── Phase 3: send the six envelopes serially ─────
  //
  // We pace the sends ~150ms apart so each typed dispatcher's UI-hop
  // PostTask completes before the next envelope's hop is queued. This
  // is purely for stderr-grep readability — the dispatchers are
  // serialised on BrowserThread::UI either way, but interleaved
  // dispatcher output makes the orchestrator's per-dispatcher seq
  // attribution noisier.
  const envelopes = buildEnvelopes();
  for (const env of envelopes) {
    const payload = JSON.stringify(env);
    log("info", "sending envelope", { type: env.type, seq: env.seq, bytes: payload.length });
    try {
      inputDc.send(payload);
    } catch (e) {
      log("err", "VERDICT: DC SEND FAIL", { seq: env.seq, type: env.type, err: String(e) });
      process.exit(4);
    }
    // Inter-send delay — see comment above.
    await new Promise((r) => setTimeout(r, 150));
  }

  // ───── Phase 4: wait for the worker to process + (potentially) LOG ─────
  await new Promise((r) => setTimeout(r, POST_SEND_WAIT_MS));

  log("ok", "WIRE VERDICT: ALL SIX ENVELOPES SENT; verdict derived externally from cb-chromium stderr", {
    seqs_sent: envelopes.map((e) => ({ seq: e.seq, type: e.type })),
    grep_anchor_A_composite_ctor:
      'CV2-81: CbInputDispatchCompositeDelegate ctor — 6 typed dispatchers (R3 mouse, R4 keyboard, R5 IME, R6 touch, R7 drag, R8 clipboard) wired',
    grep_anchor_B_main_parts_wiring:
      'CV2-81: "input" DC observer = CbInputDispatch (composite delegate = R3..R8 typed pipeline)',
    grep_neg_C_must_be_absent_pre_cv281_baseline:
      'CbInputDispatch: dispatched type=',
    grep_neg_D_must_be_absent_dispatcher_drop_R3:
      'CbInputDispatchMouse: mouse_move dropped',
    grep_neg_D_must_be_absent_dispatcher_drop_R4:
      'CbInputDispatchKeyboard: key_down dropped',
    grep_neg_D_must_be_absent_dispatcher_drop_R5:
      'cb-ime: composition_start dropped',
    grep_neg_D_must_be_absent_dispatcher_drop_R6:
      'touch_start: no active WebContents',
    grep_neg_D_must_be_absent_dispatcher_drop_R7:
      'CbInputDispatchDrag: drag_start dropped',
    grep_neg_D_must_be_absent_dispatcher_drop_R8:
      'CbInputDispatchClipboard: clipboard_copy_request dropped',
    grep_neg_D_must_be_absent_field_shape_R3:
      'CbInputDispatchMouse: mouse_move missing x/y',
    grep_neg_D_must_be_absent_field_shape_R4:
      'CbInputDispatchKeyboard: envelope missing both key and code',
    grep_neg_D_must_be_absent_field_shape_R6:
      'touch_start missing required identifier/x/y',
    grep_neg_D_must_be_absent_field_shape_R7:
      'CbInputDispatchDrag: drag_start missing x/y',
    r7_partial_coverage_note:
      'R7 chromium-side rwh->DragTarget* calls are commented out per CV2-81 NON-GOAL banner; this harness exercises the R7 state machine through OnInputEvent ONLY. Renderer-side dragstart DOM event NOT asserted (CV2-82 follow-up).',
  });

  try { pc.close(); ws.close(1000); } catch {}
  process.exit(0);
}

main().catch((e) => {
  log("err", "main threw", { err: String(e), stack: e?.stack });
  process.exit(1);
});

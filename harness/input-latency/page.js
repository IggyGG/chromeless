/*
 * Input latency harness — client side.
 *
 * On each keydown event:
 *   1. Stamp the keydown timestamp (perf + epoch).
 *   2. Synchronously toggle the stage to RESPONSE_COLOR. The browser
 *      will paint this on the next vsync.
 *   3. On the immediately-following requestAnimationFrame (the rAF
 *      callback runs *before* paint per spec), encode the keydown
 *      timestamp as a QR code in the corner of the response frame —
 *      so the webcam frame that contains the response color also
 *      contains the timestamp of the keydown that caused it.
 *   4. Emit a JSON record to the websocket sink for ground-truth
 *      cross-referencing.
 *
 * The reconcile script then:
 *   - Detects the keystroke moment in the cam recording (operator
 *     keyboard LED, phone-stopwatch OCR, or manual annotation —
 *     see calibration.md).
 *   - Detects each screen-response flash by decoding the QR.
 *   - Pairs each keystroke with the next screen response within a
 *     configurable window.
 *   - Reports latency = response_frame_capture_time -
 *                       keystroke_frame_capture_time.
 *
 * Wire format for the QR payload (mirrors latency/encode-timestamp.js
 * shape so future tooling can share one parser):
 *
 *   v2-input|<runId>|<keydownEpochMs>|<keyCode>|<keystrokeId>
 *
 * v2- prefix distinguishes from latency/v1| envelopes so the
 * reconcile script can tell which run a frame belongs to.
 */

(() => {
  "use strict";

  const params = new URLSearchParams(location.search);
  const SINK_URL = params.get("sink") || "ws://localhost:9001";
  const RUN_ID = params.get("run") || `inputlat-${Date.now()}`;
  const RESPONSE_COLOR = params.get("color") || "#ffffff";
  const RESPONSE_HOLD_MS = parseInt(params.get("hold") || "200", 10);
  // Limit which keys trigger a flash; default: any key. Useful when
  // the operator wants the spacebar to be the only signal.
  const KEY_FILTER = params.get("key") || "";

  const stage = document.getElementById("stage");
  const qr = document.getElementById("qr");
  const status = document.getElementById("status");
  stage.style.setProperty("--response-color", RESPONSE_COLOR);

  // ---------------- sink ----------------
  let sink = null;
  let sinkOpen = false;
  function connectSink() {
    try {
      sink = new WebSocket(SINK_URL);
      sink.onopen = () => { sinkOpen = true; };
      sink.onclose = () => { sinkOpen = false; setTimeout(connectSink, 2000); };
      sink.onerror = () => { sinkOpen = false; };
    } catch (e) {
      setTimeout(connectSink, 2000);
    }
  }
  connectSink();

  const log = [];
  function emit(rec) {
    log.push(rec);
    if (sinkOpen) {
      try { sink.send(JSON.stringify(rec)); } catch {}
    }
  }

  // ---------------- per-keystroke pipeline ----------------
  let keystrokeId = 0;
  let pendingResetId = null;

  function onKeyDown(e) {
    if (KEY_FILTER && e.key !== KEY_FILTER && e.code !== KEY_FILTER) return;
    // Don't trigger on autorepeat — those are not new physical
    // keystrokes and would mess up the pairing.
    if (e.repeat) return;

    const id = ++keystrokeId;
    const keydownPerfMs = performance.now();
    const keydownEpochMs = Date.now();

    // Synchronous repaint trigger — by adding the .responding class
    // *before* this event handler returns, the browser is committed
    // to rendering the new color on its next paint.
    stage.classList.add("responding");

    // Encode the keystroke metadata into a QR. We do it in the next
    // rAF so the QR canvas update lands in the same frame as the
    // color change.
    requestAnimationFrame((rafTs) => {
      const payload = `v2-input|${RUN_ID}|${keydownEpochMs}|${e.code}|${id}`;
      drawQrInto(qr, payload, 12);

      const rec = {
        type: "input_keydown",
        runId: RUN_ID,
        keystrokeId: id,
        code: e.code,
        key: e.key,
        keydownPerfMs,
        keydownEpochMs,
        rafPerfMs: rafTs,
        responseColor: RESPONSE_COLOR,
      };
      emit(rec);

      status.textContent =
        `k=${id} code=${e.code} key=${e.key} t=${keydownEpochMs} run=${RUN_ID}`;
    });

    // Decay the flash back to black after the hold window.
    if (pendingResetId !== null) clearTimeout(pendingResetId);
    pendingResetId = setTimeout(() => {
      stage.classList.remove("responding");
      pendingResetId = null;
    }, RESPONSE_HOLD_MS);
  }

  document.addEventListener("keydown", onKeyDown, { capture: true });

  // Convenience: download the in-memory log as JSONL.
  document.addEventListener("keydown", (e) => {
    if (e.key === "d" && e.ctrlKey && e.shiftKey) {
      const blob = new Blob(log.map(r => JSON.stringify(r) + "\n"),
                            { type: "application/x-ndjson" });
      const a = document.createElement("a");
      a.href = URL.createObjectURL(blob);
      a.download = `${RUN_ID}.jsonl`;
      a.click();
      e.preventDefault();
      e.stopPropagation();
    }
  }, { capture: true });

  console.log("input-latency harness running",
              { RUN_ID, SINK_URL, RESPONSE_COLOR, RESPONSE_HOLD_MS, KEY_FILTER });
})();

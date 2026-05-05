// tests/webrtc/fixtures/demo.js — canvas-rendered demo content for the
// WebRTC harness. The harness captures a MediaStream from the canvas
// this module animates, so whatever we paint here is what lands in
// out.webm.
//
// Why a canvas-based demo and not a fullscreen DOM page captured via
// getDisplayMedia:
//
//   * canvas.captureStream(fps) works in headless chromium with no
//     display server, no window picker, no --auto-select-desktop-
//     capture-source flag. It produces a real MediaStream that goes
//     through the WebRTC encoder pipeline identically to a camera
//     source (which means our embedded encoder factory still services
//     it — the encoder-identity assertion is unaffected).
//   * Deterministic bytes-out — no Xvfb compositor introducing race
//     conditions on what's visible at frame N.
//   * The harness can call into the demo (via window.__cbtest.handle)
//     to step scenes; the demo's response is visible in the recording
//     within a single frame.
//
// Self-containment rules (mirrors the rules in fixtures/spinner.html):
//   - No external fetches. Everything renders from in-process code.
//   - Pure 2D canvas — no WebGL (sandbox/test images may not have a
//     working ANGLE backend).
//   - All fonts via the canvas API's CSS-font shorthand using
//     ui-monospace fallbacks.
//
// Public API:
//   const { dispose, send, render } = attachDemo(canvas, { fps: 30 })
//
//   send({ type: 'set-scene', value: 'streaming' })   // 'boot'|'idle'|'streaming'|'error'
//   send({ type: 'log-line',  text: 'foo' })          // append to log feed
//   send({ type: 'bar-set',   index: 0, value: 0.85 })// set chart bar [0..1]
//   send({ type: 'flash',     color: '#ff5577' })     // brief overlay flash
//   send({ type: 'badge',     text: 'STREAMING' })    // header status badge
//
//   render() returns a one-shot draw (used by the standalone preview
//   page when rAF is suspended; the harness uses the internal interval).

export function attachDemo(canvas, opts = {}) {
  const fps = Number.isFinite(opts.fps) && opts.fps > 0 ? opts.fps : 30;
  const ctx = canvas.getContext("2d", { alpha: false });
  if (!ctx) throw new Error("attachDemo: 2D context unavailable");

  const W = canvas.width;
  const H = canvas.height;

  // ---- state ----------------------------------------------------------------

  const state = {
    scene: "boot",
    badge: "BOOT",
    bars: [0.20, 0.55, 0.40, 0.70, 0.30, 0.85],
    barTargets: [0.20, 0.55, 0.40, 0.70, 0.30, 0.85],
    log: [
      { ts: 0, text: "[init] chromeless attached" },
      { ts: 0, text: "[cdp] Target.attachToTarget OK" },
    ],
    flashColor: null,
    flashUntil: 0,
    particles: makeParticles(40),
    started: performance.now(),
    frame: 0,
  };

  function send(msg) {
    if (!msg || typeof msg !== "object") return { ok: false, error: "bad-msg" };
    switch (msg.type) {
      case "set-scene": {
        const v = String(msg.value || "");
        if (!["boot", "idle", "streaming", "error"].includes(v)) {
          return { ok: false, error: "unknown-scene" };
        }
        state.scene = v;
        // Auto-rotate the badge unless caller pinned one.
        state.badge = v.toUpperCase();
        if (v === "streaming") {
          state.barTargets = state.bars.map(() => 0.4 + Math.random() * 0.5);
        }
        return { ok: true };
      }
      case "log-line": {
        state.log.push({
          ts: performance.now() - state.started,
          text: String(msg.text || ""),
        });
        if (state.log.length > 12) state.log.splice(0, state.log.length - 12);
        return { ok: true };
      }
      case "bar-set": {
        const i = Number(msg.index);
        const v = Math.max(0, Math.min(1, Number(msg.value)));
        if (Number.isFinite(i) && i >= 0 && i < state.bars.length) {
          state.barTargets[i] = v;
          return { ok: true };
        }
        return { ok: false, error: "bad-index" };
      }
      case "flash": {
        state.flashColor = String(msg.color || "#ffffff");
        state.flashUntil = performance.now() + 350;
        return { ok: true };
      }
      case "badge": {
        state.badge = String(msg.text || "").slice(0, 24).toUpperCase();
        return { ok: true };
      }
      default:
        return { ok: false, error: "unknown-type" };
    }
  }

  // ---- drawing --------------------------------------------------------------

  function draw() {
    state.frame += 1;
    const now = performance.now();
    const t = (now - state.started) / 1000;

    // Background
    ctx.fillStyle = "#0b0e14";
    ctx.fillRect(0, 0, W, H);

    drawHeader(ctx, W, H, state, t);
    drawSpinner(ctx, W, H, t, state.scene);
    drawBars(ctx, W, H, state);
    drawParticles(ctx, W, H, state, t);
    drawLog(ctx, W, H, state);
    drawFooter(ctx, W, H, state, t);

    if (state.flashColor && now < state.flashUntil) {
      const remaining = (state.flashUntil - now) / 350;
      ctx.fillStyle = state.flashColor;
      ctx.globalAlpha = 0.35 * remaining;
      ctx.fillRect(0, 0, W, H);
      ctx.globalAlpha = 1;
    }

    // Ease bar values toward their targets so harness-set values
    // visibly animate rather than snap.
    for (let i = 0; i < state.bars.length; i++) {
      state.bars[i] += (state.barTargets[i] - state.bars[i]) * 0.12;
    }
  }

  // Drive draws via setInterval so the demo runs even when the tab is
  // background-throttled (rAF gets clamped to 1Hz in some chromium
  // configurations; setInterval honours capture cadence regardless).
  const interval = setInterval(draw, Math.round(1000 / fps));
  draw();

  return {
    send,
    render: draw,
    dispose() { clearInterval(interval); },
  };
}

// ---- scene primitives -------------------------------------------------------

function drawHeader(ctx, W, H, state, t) {
  // Top status bar.
  ctx.fillStyle = "#11161f";
  ctx.fillRect(0, 0, W, 44);
  ctx.fillStyle = "#1f2a3a";
  ctx.fillRect(0, 43, W, 1);

  ctx.fillStyle = "#7a8da6";
  ctx.font = "13px ui-monospace, Menlo, Consolas, monospace";
  ctx.textBaseline = "middle";
  ctx.fillText("chromeless webrtc test fixture", 16, 22);

  // Right-aligned: scene badge.
  const badgeColor = badgeColorFor(state.scene);
  const badgeText = state.badge;
  ctx.font = "12px ui-monospace, Menlo, Consolas, monospace";
  const tw = ctx.measureText(badgeText).width;
  const bx = W - tw - 32;
  const bw = tw + 16;
  ctx.fillStyle = badgeColor.bg;
  roundRect(ctx, bx, 14, bw, 22, 4);
  ctx.fill();
  ctx.fillStyle = badgeColor.fg;
  ctx.fillText(badgeText, bx + 8, 25);

  // Frame counter (proof of motion).
  ctx.font = "11px ui-monospace, Menlo, Consolas, monospace";
  ctx.fillStyle = "#5a6c87";
  ctx.fillText(
    `t=${t.toFixed(2).padStart(7)}  f=${String(state.frame).padStart(5)}`,
    W - 220, 22,
  );
}

function badgeColorFor(scene) {
  switch (scene) {
    case "streaming": return { bg: "#1f4d2c", fg: "#7fffaa" };
    case "error":     return { bg: "#4d1f1f", fg: "#ff8888" };
    case "idle":      return { bg: "#1f2a3a", fg: "#7fbfff" };
    default:          return { bg: "#332a1f", fg: "#ffcc66" };
  }
}

function drawSpinner(ctx, W, H, t, scene) {
  // Spinner and inner counter-rotating ring, drawn in the upper-left
  // working area below the header.
  const cx = 130, cy = 140, r = 64;

  ctx.save();
  ctx.translate(cx, cy);
  ctx.rotate(t * 2 * Math.PI / 1.5); // 1.5s/rev like the original CSS
  drawConic(ctx, r, [
    [0, "#ff5577"],
    [1/6, "#ffaa00"],
    [2/6, "#44dd66"],
    [3/6, "#00bbff"],
    [4/6, "#aa66ff"],
    [5/6, "#ff66dd"],
    [1, "#ff5577"],
  ]);
  ctx.restore();

  // Inner mask + counter-rotation arc.
  ctx.fillStyle = "#0b0e14";
  ctx.beginPath();
  ctx.arc(cx, cy, r - 18, 0, 2 * Math.PI);
  ctx.fill();

  ctx.save();
  ctx.translate(cx, cy);
  ctx.rotate(-t * 2 * Math.PI / 2.7);
  ctx.strokeStyle = "rgba(255,255,255,0.35)";
  ctx.lineWidth = 4;
  ctx.beginPath();
  ctx.arc(0, 0, r - 24, 0, Math.PI / 3);
  ctx.stroke();
  ctx.restore();

  // Centre label per scene.
  ctx.font = "12px ui-monospace, Menlo, Consolas, monospace";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  const lbl = scene === "streaming" ? "LIVE"
            : scene === "error"     ? "ERR"
            : scene === "idle"      ? "IDLE"
            :                         "BOOT";
  ctx.fillStyle = scene === "error" ? "#ff8888" : "#d6e3ff";
  ctx.fillText(lbl, cx, cy);
  ctx.textAlign = "start";
  ctx.textBaseline = "alphabetic";
}

function drawConic(ctx, r, stops) {
  // Manual conic gradient via wedges — chromium's 2D ctx.createConicGradient
  // exists but rendering parity differs across pkgs; manual wedges are
  // deterministic.
  const N = 60;
  for (let i = 0; i < N; i++) {
    const a0 = (i / N) * 2 * Math.PI - Math.PI / 2;
    const a1 = ((i + 1) / N) * 2 * Math.PI - Math.PI / 2;
    const f = i / N;
    ctx.fillStyle = lerpStops(stops, f);
    ctx.beginPath();
    ctx.moveTo(0, 0);
    ctx.arc(0, 0, r, a0, a1);
    ctx.closePath();
    ctx.fill();
  }
}

function lerpStops(stops, f) {
  for (let i = 0; i < stops.length - 1; i++) {
    const [p0, c0] = stops[i];
    const [p1, c1] = stops[i + 1];
    if (f >= p0 && f <= p1) {
      const k = (f - p0) / (p1 - p0);
      return mixColor(c0, c1, k);
    }
  }
  return stops[stops.length - 1][1];
}

function mixColor(a, b, k) {
  const A = parseHex(a), B = parseHex(b);
  const r = Math.round(A.r + (B.r - A.r) * k);
  const g = Math.round(A.g + (B.g - A.g) * k);
  const bl = Math.round(A.b + (B.b - A.b) * k);
  return `rgb(${r},${g},${bl})`;
}

function parseHex(s) {
  const v = s.replace("#", "");
  return {
    r: parseInt(v.slice(0, 2), 16),
    g: parseInt(v.slice(2, 4), 16),
    b: parseInt(v.slice(4, 6), 16),
  };
}

function drawBars(ctx, W, H, state) {
  // Right of spinner, above mid line.
  const x0 = 240, y0 = 80, w = W - 240 - 32, h = 130;
  ctx.fillStyle = "#11161f";
  roundRect(ctx, x0, y0, w, h, 6);
  ctx.fill();

  ctx.font = "11px ui-monospace, Menlo, Consolas, monospace";
  ctx.fillStyle = "#7a8da6";
  ctx.fillText("encoder throughput (relative)", x0 + 12, y0 + 18);

  const barCount = state.bars.length;
  const innerX = x0 + 12, innerY = y0 + 28;
  const innerW = w - 24, innerH = h - 40;
  const gap = 8;
  const bw = (innerW - gap * (barCount - 1)) / barCount;

  for (let i = 0; i < barCount; i++) {
    const v = state.bars[i];
    const bh = Math.max(2, v * innerH);
    const bx = innerX + i * (bw + gap);
    const by = innerY + (innerH - bh);

    // Backdrop
    ctx.fillStyle = "#1a2230";
    roundRect(ctx, bx, innerY, bw, innerH, 3);
    ctx.fill();

    // Bar gradient
    const grad = ctx.createLinearGradient(bx, by, bx, by + bh);
    grad.addColorStop(0, "#5fb4ff");
    grad.addColorStop(1, "#3a6db0");
    ctx.fillStyle = grad;
    roundRect(ctx, bx, by, bw, bh, 3);
    ctx.fill();

    // Value label
    ctx.fillStyle = "#a4b6cf";
    ctx.font = "10px ui-monospace, Menlo, Consolas, monospace";
    const lbl = (v * 100).toFixed(0) + "%";
    const lblW = ctx.measureText(lbl).width;
    ctx.fillText(lbl, bx + bw / 2 - lblW / 2, innerY + innerH + 12);
  }
}

function makeParticles(n) {
  const p = [];
  for (let i = 0; i < n; i++) {
    p.push({
      x: Math.random(),
      y: Math.random(),
      vx: (Math.random() - 0.5) * 0.04,
      vy: (Math.random() - 0.5) * 0.04,
      r: 1 + Math.random() * 2,
      hue: 200 + Math.random() * 60,
    });
  }
  return p;
}

function drawParticles(ctx, W, H, state, t) {
  // Bottom-left panel.
  const x0 = 16, y0 = 224, w = 220, h = H - y0 - 60;
  ctx.fillStyle = "#11161f";
  roundRect(ctx, x0, y0, w, h, 6);
  ctx.fill();
  ctx.fillStyle = "#7a8da6";
  ctx.font = "11px ui-monospace, Menlo, Consolas, monospace";
  ctx.fillText("network · packet flow", x0 + 12, y0 + 18);

  ctx.save();
  ctx.beginPath();
  ctx.rect(x0 + 4, y0 + 28, w - 8, h - 32);
  ctx.clip();

  for (const p of state.particles) {
    p.x += p.vx;
    p.y += p.vy;
    if (p.x < 0) p.x += 1;
    if (p.x > 1) p.x -= 1;
    if (p.y < 0) p.y += 1;
    if (p.y > 1) p.y -= 1;
    const cx = x0 + 4 + p.x * (w - 8);
    const cy = y0 + 28 + p.y * (h - 32);
    ctx.fillStyle = `hsl(${p.hue + Math.sin(t + p.x * 4) * 20},70%,65%)`;
    ctx.beginPath();
    ctx.arc(cx, cy, p.r, 0, 2 * Math.PI);
    ctx.fill();
  }
  ctx.restore();
}

function drawLog(ctx, W, H, state) {
  // Right-bottom log panel.
  const x0 = 252, y0 = 224, w = W - x0 - 32, h = H - y0 - 60;
  ctx.fillStyle = "#11161f";
  roundRect(ctx, x0, y0, w, h, 6);
  ctx.fill();

  ctx.fillStyle = "#7a8da6";
  ctx.font = "11px ui-monospace, Menlo, Consolas, monospace";
  ctx.fillText("event log", x0 + 12, y0 + 18);

  ctx.font = "12px ui-monospace, Menlo, Consolas, monospace";
  let y = y0 + 38;
  const maxLines = Math.floor((h - 32) / 16);
  const lines = state.log.slice(-maxLines);
  for (const line of lines) {
    const ts = `+${(line.ts / 1000).toFixed(2)}s`;
    ctx.fillStyle = "#5a6c87";
    ctx.fillText(ts.padStart(8), x0 + 12, y);
    ctx.fillStyle = "#cfd8e8";
    ctx.fillText(line.text, x0 + 80, y);
    y += 16;
  }
}

function drawFooter(ctx, W, H, state, t) {
  // 40px tall footer with a moving pulse band — the pulse is the
  // single most reliable "frames moved" signal for a human eye to
  // glance at the recording and confirm motion.
  const y0 = H - 44;
  ctx.fillStyle = "#11161f";
  ctx.fillRect(0, y0, W, 44);
  ctx.fillStyle = "#1f2a3a";
  ctx.fillRect(0, y0, W, 1);

  // Sliding gradient band.
  const phase = (t * 0.6) % 1;
  const grad = ctx.createLinearGradient(0, y0, W, y0);
  grad.addColorStop(Math.max(0, phase - 0.2), "rgba(95,180,255,0)");
  grad.addColorStop(phase, "rgba(95,180,255,0.6)");
  grad.addColorStop(Math.min(1, phase + 0.2), "rgba(95,180,255,0)");
  ctx.fillStyle = grad;
  ctx.fillRect(0, y0 + 16, W, 12);

  ctx.font = "11px ui-monospace, Menlo, Consolas, monospace";
  ctx.fillStyle = "#7a8da6";
  ctx.fillText("captured via canvas.captureStream(30) — recorded to out.webm", 16, y0 + 32);
}

function roundRect(ctx, x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.lineTo(x + w - r, y);
  ctx.quadraticCurveTo(x + w, y, x + w, y + r);
  ctx.lineTo(x + w, y + h - r);
  ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h);
  ctx.lineTo(x + r, y + h);
  ctx.quadraticCurveTo(x, y + h, x, y + h - r);
  ctx.lineTo(x, y + r);
  ctx.quadraticCurveTo(x, y, x + r, y);
  ctx.closePath();
}

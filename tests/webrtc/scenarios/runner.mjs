#!/usr/bin/env node
// scenarios/runner.mjs
//
// Discovers all *.scenario.mjs files in this directory, runs them
// sequentially against a single cb-chromium endpoint (each scenario
// owns its own BrowserContext / Target / recording), and emits a
// browseable HTML index (artifacts/index.html) embedding every webm
// alongside its assertion table — so a human reviewer scrubs the
// recordings and reads pass/fail without leaving the browser.
//
// Each scenario is an isolated Job step: a fresh BrowserContext +
// fresh Target ensures one scenario's hover state, scroll position,
// or focus don't leak into the next. Sequential because we want
// deterministic recording boundaries; the cluster Job parallelises
// across pods, not across scenarios within a pod.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import process from "node:process";

const HERE = path.dirname(fileURLToPath(import.meta.url));

function parseArgs(argv) {
  const out = {
    cbUrl: process.env.CB_URL || "http://127.0.0.1:9222",
    artifactsDir: process.env.TEST_ARTIFACTS_DIR
      || path.resolve(process.cwd(), "artifacts"),
    only: null,    // run only matching scenario name (substring)
    skip: null,    // skip matching scenario name (substring)
    failFast: false,
  };
  for (const a of argv) {
    if (a.startsWith("--cb-url="))       out.cbUrl = a.slice("--cb-url=".length);
    else if (a.startsWith("--artifacts=")) out.artifactsDir = path.resolve(a.slice("--artifacts=".length));
    else if (a.startsWith("--only="))    out.only = a.slice("--only=".length);
    else if (a.startsWith("--skip="))    out.skip = a.slice("--skip=".length);
    else if (a === "--fail-fast")        out.failFast = true;
    else if (a === "-h" || a === "--help") { printHelp(); process.exit(0); }
    else { console.error(`unknown arg: ${a}`); process.exit(2); }
  }
  return out;
}
function printHelp() {
  process.stderr.write(`Usage: node runner.mjs [options]
  --cb-url=<url>        cb-chromium /json/version base. Default 127.0.0.1:9222
  --artifacts=<dir>     Output directory for webm + summary.json + index.html
  --only=<substring>    Run only scenarios whose name matches
  --skip=<substring>    Skip scenarios whose name matches
  --fail-fast           Stop on first failed scenario
`);
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  fs.mkdirSync(args.artifactsDir, { recursive: true });

  // Discover scenarios. The runner imports them dynamically and
  // expects each to export a `scenario` object with { name, page, run }.
  const scenarioFiles = fs
    .readdirSync(HERE)
    .filter((f) => f.endsWith(".scenario.mjs"))
    .sort();

  const filtered = scenarioFiles.filter((f) => {
    if (args.only && !f.includes(args.only)) return false;
    if (args.skip &&  f.includes(args.skip))  return false;
    return true;
  });

  process.stderr.write(`[runner] discovered ${scenarioFiles.length} scenarios; ${filtered.length} after filter\n`);

  const results = [];
  for (const file of filtered) {
    const url = pathToFileURL(path.join(HERE, file)).href;
    const mod = await import(url);
    if (!mod.scenario) {
      process.stderr.write(`[runner] ${file} has no exported \`scenario\` — skipping\n`);
      continue;
    }
    const sc = mod.scenario;
    process.stderr.write(`\n[runner] ━━━ ${sc.name} ━━━\n`);
    const { runScenario } = await import("./_lib.mjs");
    const exitBefore = process.exitCode;
    const startedAt = Date.now();
    try {
      await runScenario(sc, {
        cbUrl: args.cbUrl,
        artifactsDir: args.artifactsDir,
      });
    } catch (err) {
      process.stderr.write(`[runner] ${sc.name} threw: ${err.stack || err}\n`);
    }
    const elapsedMs = Date.now() - startedAt;
    const summaryFile = path.join(args.artifactsDir, `summary-${sc.name}.json`);
    let summary = null;
    try { summary = JSON.parse(fs.readFileSync(summaryFile, "utf8")); }
    catch { /* scenario aborted before summary written */ }
    const result = {
      name: sc.name,
      page: sc.page,
      file,
      elapsedMs,
      passed: summary?.passed ?? false,
      assertions: summary?.assertions ?? [],
      markers: summary?.markers ?? [],
      videoFile: summary?.outFile ?? null,
      eventCount: summary?.eventCount ?? 0,
      videoFrames: summary?.frames ?? 0,
    };
    results.push(result);
    // Reset exit code so subsequent scenarios run; we accumulate
    // failure into the final exit at the end.
    process.exitCode = exitBefore;

    if (args.failFast && !result.passed) {
      process.stderr.write(`[runner] --fail-fast: stopping after ${sc.name}\n`);
      break;
    }
  }

  // Write a runner manifest.
  const manifest = {
    cbUrl: args.cbUrl,
    artifactsDir: args.artifactsDir,
    startedAt: new Date().toISOString(),
    scenarios: results,
    totals: {
      total: results.length,
      passed: results.filter((r) => r.passed).length,
      failed: results.filter((r) => !r.passed).length,
    },
  };
  fs.writeFileSync(
    path.join(args.artifactsDir, "manifest.json"),
    JSON.stringify(manifest, null, 2),
  );

  // Render the HTML index.
  fs.writeFileSync(
    path.join(args.artifactsDir, "index.html"),
    renderIndex(manifest),
  );

  // Final summary line — easy to grep for in CI.
  process.stderr.write(
    `\nRUNNER_RESULT total=${manifest.totals.total} ` +
    `passed=${manifest.totals.passed} failed=${manifest.totals.failed} ` +
    `index=${path.join(args.artifactsDir, "index.html")}\n`,
  );

  if (manifest.totals.failed > 0) process.exit(1);
}

// HTML index template — embeds each webm + assertion table inline so
// a reviewer can `open artifacts/index.html` and scrub through every
// recording without spawning a separate viewer.
function renderIndex(manifest) {
  const css = `
    body { font: 14px ui-monospace,SFMono-Regular,Menlo,monospace; background:#0b0e14; color:#cfd8e8; margin:0; padding:0; }
    header { padding:16px 24px; background:#11161f; border-bottom:1px solid #1f2a3a; }
    header h1 { margin:0 0 6px; font-size:18px; color:#fff; }
    header .meta { color:#7a8da6; font-size:12px; }
    .totals { display:inline-block; margin-left:16px; padding:2px 10px; border-radius:3px; }
    .totals.ok { background:#1f4d2c; color:#7fffaa; }
    .totals.fail { background:#4d1f1f; color:#ff8888; }
    main { padding: 16px 24px; max-width: 1400px; }
    .scenario { background:#11161f; border:1px solid #1f2a3a; border-radius:6px; margin-bottom:24px; overflow:hidden; }
    .scenario header { background:#1a2230; border-bottom:1px solid #1f2a3a; padding:10px 16px; display:flex; align-items:center; gap:12px; }
    .scenario header h2 { margin:0; font-size:14px; color:#fff; }
    .badge { padding:2px 8px; border-radius:3px; font-size:11px; }
    .badge.pass { background:#1f4d2c; color:#7fffaa; }
    .badge.fail { background:#4d1f1f; color:#ff8888; }
    .badge.dim  { background:#1c2433; color:#7a8da6; }
    .body { display:grid; grid-template-columns: 1fr 1fr; gap:16px; padding:16px; }
    .video-col video { width:100%; border-radius:4px; background:#000; }
    .video-col .markers { font-size:11px; color:#7a8da6; margin-top:8px; max-height:140px; overflow:auto; }
    .video-col .markers div { padding:1px 0; }
    .assertions { font-size:12px; }
    .assertions h3 { font-size:12px; color:#7a8da6; margin:0 0 6px; }
    .assertion { padding:6px 8px; border-radius:3px; margin-bottom:4px; }
    .assertion.ok   { background:#152c1f; }
    .assertion.fail { background:#3a1c1c; color:#ff8888; }
    .assertion .label { display:block; }
    .assertion .detail { font-size:10px; color:#7a8da6; margin-top:2px; word-break:break-all; }
    a { color:#5fb4ff; text-decoration:none; }
    a:hover { text-decoration:underline; }
  `;

  const totalsCls = manifest.totals.failed === 0 ? "ok" : "fail";
  const totalsTxt = manifest.totals.failed === 0
    ? `all ${manifest.totals.total} passed`
    : `${manifest.totals.failed}/${manifest.totals.total} failed`;

  const sections = manifest.scenarios.map((s) => {
    const videoBase = s.videoFile ? path.basename(s.videoFile) : null;
    const eventsBase = `events-${s.name}.json`;
    const summaryBase = `summary-${s.name}.json`;
    const passCls = s.passed ? "pass" : "fail";
    const passTxt = s.passed ? "PASS" : "FAIL";
    const aRows = s.assertions.map((a) => `
      <div class="assertion ${a.ok ? "ok" : "fail"}">
        <span class="label">${a.ok ? "✓" : "✗"} ${esc(a.label)}</span>
        ${!a.ok && (a.expected !== undefined || a.actual !== undefined) ? `
        <div class="detail">expected: ${esc(JSON.stringify(a.expected))}<br/>actual: ${esc(JSON.stringify(a.actual))}</div>` : ""}
      </div>`).join("");
    const mRows = s.markers.map((m) =>
      `<div>+${m.atMs}ms · ${esc(m.label)}</div>`).join("");
    return `
      <section class="scenario">
        <header>
          <h2>${esc(s.name)}</h2>
          <span class="badge ${passCls}">${passTxt}</span>
          <span class="badge dim">${s.assertions.filter((a) => a.ok).length}/${s.assertions.length} assertions</span>
          <span class="badge dim">${s.videoFrames} frames</span>
          <span class="badge dim">${s.eventCount} events</span>
          <span class="badge dim">${(s.elapsedMs / 1000).toFixed(1)}s</span>
        </header>
        <div class="body">
          <div class="video-col">
            ${videoBase ? `<video controls preload="metadata" src="${esc(videoBase)}"></video>` : `<div style="padding:24px;text-align:center;color:#7a8da6">no recording</div>`}
            <div style="font-size:11px;margin-top:6px;color:#7a8da6">
              <a href="${esc(eventsBase)}">events.json</a> ·
              <a href="${esc(summaryBase)}">summary.json</a>
              ${videoBase ? ` · <a href="${esc(videoBase)}" download>download webm</a>` : ""}
            </div>
            <div class="markers">${mRows || "<div>no markers</div>"}</div>
          </div>
          <div class="assertions">
            <h3>assertions (${s.assertions.length})</h3>
            ${aRows || "<div style=\"color:#7a8da6\">no assertions recorded</div>"}
          </div>
        </div>
      </section>
    `;
  }).join("");

  return `<!doctype html><html><head><meta charset="utf-8" />
<title>cb-chromium webrtc input test report</title>
<style>${css}</style></head>
<body>
<header>
  <h1>cb-chromium webrtc input test report</h1>
  <div class="meta">
    cb-url: ${esc(manifest.cbUrl)} · started: ${esc(manifest.startedAt)}
    <span class="totals ${totalsCls}">${totalsTxt}</span>
  </div>
</header>
<main>${sections}</main>
</body></html>`;
}

function esc(s) {
  return String(s ?? "")
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

main().catch((err) => {
  console.error("[runner] fatal:", err.stack || err);
  process.exit(1);
});

#!/usr/bin/env node
/* T91: Local fixture probe — runs the WebGL + WebGPU fixtures
 * against Playwright's bundled Chromium with two flag profiles:
 *
 *   1. **default**: whatever Playwright Chromium does on the host.
 *      Establishes a baseline that the fixtures themselves are
 *      working — distinguishes fixture bugs from SwiftShader bugs.
 *   2. **swiftshader-v1**: a flag set as close to v1's
 *      `infra/launch-chromeless.sh` as Playwright will accept on a
 *      developer host (we cannot make Playwright launch under
 *      Xvfb, but we *can* match the GPU/Vulkan disable + ANGLE +
 *      SwiftShader bits).
 *
 * Output: `tests/rendering/local-probe-results.json` with both
 * runs side by side. `docs/research/rendering-matrix.md` reads
 * from this.
 *
 * The docker-compose-driven counterpart lives in
 * `run-rendering-tests.sh`; that is the canonical CI path. This
 * script is the developer-loop tool for iterating on the
 * fixtures + matrix doc without spinning up the stack.
 *
 * Run from the repo root:
 *   node tests/rendering/local-probe.js
 */
"use strict";

const path  = require("node:path");
const fs    = require("node:fs/promises");

const { chromium } = require("/Users/iggy/Documents/GitHub/scrapebrowse/chromeless/tests/e2e/node_modules/playwright/index.js");

const HERE = path.resolve(__dirname);
const FIXTURES = {
  webgl:  "file://" + path.join(HERE, "webgl-fixture.html"),
  webgpu: "file://" + path.join(HERE, "webgpu-fixture.html"),
};

// Closest match to v1's launch-chromeless.sh that Playwright will
// honour on a developer host. We can't pass --display=:99 (no
// Xvfb on a dev laptop) but the GPU/ozone bits transfer.
const SWIFTSHADER_V1_FLAGS = [
  "--disable-features=Vulkan,VaapiVideoDecodeLinuxGL",
  "--use-gl=angle",
  "--use-angle=swiftshader-webgl",
  "--disable-gpu-vsync",
  "--enable-features=Vulkan=false",
];

async function probeOne(profile, fixtureName, fixtureUrl) {
  const launchOpts = profile === "swiftshader-v1"
    ? { args: SWIFTSHADER_V1_FLAGS }
    : {};
  const browser = await chromium.launch(launchOpts);
  const context = await browser.newContext();
  const page = await context.newPage();
  const consoleLines = [];
  page.on("console", (msg) => consoleLines.push(`[${msg.type()}] ${msg.text()}`));
  const errors = [];
  page.on("pageerror", (err) => errors.push(String(err.message || err)));

  await page.goto(fixtureUrl, { waitUntil: "load" });

  // WebGL fixture is synchronous; WebGPU is async + may hang on
  // requestDevice. Wait up to 15s for either to finish. The
  // fixtures all write window.__results__ when done.
  let result = null;
  try {
    await page.waitForFunction(
      () => typeof window["__results__"] === "object" &&
             window["__results__"] !== null &&
             window["__results__"].summary,
      undefined,
      { timeout: 15_000 },
    );
    result = await page.evaluate(() => window["__results__"]);
  } catch (e) {
    // Probe didn't finish — record what we have.
    result = await page.evaluate(() => window["__results__"] || null)
      .catch(() => null);
  }

  // Collect a screenshot for the matrix doc.
  const shotPath = path.join(HERE, `local-probe-${profile}-${fixtureName}.png`);
  await page.screenshot({ path: shotPath, fullPage: false }).catch(() => {});

  await browser.close();
  return {
    profile,
    fixture: fixtureName,
    url: fixtureUrl,
    screenshot: path.relative(path.resolve(HERE, "../.."), shotPath),
    result,
    consoleLines,
    errors,
  };
}

async function main() {
  const profiles = ["default", "swiftshader-v1"];
  const out = { ts: new Date().toISOString(), runs: [] };
  for (const profile of profiles) {
    for (const [name, url] of Object.entries(FIXTURES)) {
      console.error(`[probe] ${profile} / ${name}`);
      const r = await probeOne(profile, name, url);
      out.runs.push(r);
      const summary = r.result?.summary ?? { error: "no result" };
      console.error(`  ok=${summary.ok} warn=${summary.warn} err=${summary.err} total=${summary.total}`);
    }
  }
  const outPath = path.join(HERE, "local-probe-results.json");
  await fs.writeFile(outPath, JSON.stringify(out, null, 2));
  console.error(`[probe] wrote ${outPath}`);
}

main().catch((e) => {
  console.error("[probe] outer catch:", e);
  process.exit(1);
});

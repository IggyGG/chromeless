#!/usr/bin/env node
// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// harness/m4-uat/uat.mjs — M4 functional UAT driver (CV2-49 / R9).
//
// Shape mirrors verification/native-peer-gate.mjs from M0 R1: CLI with
// --scaffold / --strict / --image / --target, ONE JSON object on
// stdout, human-readable progress on stderr, exit 0 on PASS.
//
// What it does:
//   1. Connects to the container's CDP endpoint.
//   2. Tells the renderer to navigate to the fixture page that
//      installs the document.lastInputEvent observer.
//   3. Opens a WebRTC peer connection to the native peer via M3's
//      signaling client and binds the `input` DataChannel.
//   4. For each enabled scenario, sends representative envelopes per
//      docs/protocols/input-channel.md and reads document.lastInputEvent
//      back via CDP Runtime.evaluate.
//   5. Probes the image manifest to assert input-bridge IS present
//      (negative-of-M7).
//   6. Emits the unified JSON verdict.
//
// DRAFT — many of the steps below are stubs that call into TODO'd
// helpers. The C++/native side (M3 R5 consumer-bind, M4 R1 sink) is
// not yet shipping, so this harness CANNOT execute end-to-end on
// integration/native-peer HEAD. The shape is locked though, so the
// moment those drafts merge and a chromeless:ci is built from them,
// uat.mjs ought to run.

import { argv, exit, stdout, stderr } from 'node:process';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

import { buildEnvelope, ENVELOPE_TYPES } from './lib/envelope.mjs';
import { openCdp, navigatePage, readLastInputEvent, evaluateScript } from './lib/cdp.mjs';
import { openNativeDataChannel } from './lib/datachannel.mjs';
import { probeBridgePresence } from './lib/bridge-presence.mjs';
import { Status } from './lib/registry.mjs';

import { scenario as mouseScenario } from './scenarios/mouse.mjs';
import { scenario as keyboardScenario } from './scenarios/keyboard.mjs';
import { scenario as imeScenario } from './scenarios/ime.mjs';
import { scenario as touchScenario } from './scenarios/touch.mjs';
import { scenario as dragScenario } from './scenarios/drag.mjs';
import { scenario as clipboardScenario } from './scenarios/clipboard.mjs';
import { scenario as pointerLeaveScenario } from './scenarios/pointer-leave.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const SCHEMA_NAME = 'm4-uat';
const SCHEMA_VERSION = 1;

const SCENARIOS = [
  mouseScenario,
  keyboardScenario,
  imeScenario,
  touchScenario,
  dragScenario,
  clipboardScenario,
  pointerLeaveScenario,
];

function parseArgs(args) {
  const opts = {
    mode: 'scaffold',
    image: 'chromeless:ci',
    cdp: 'http://localhost:9222',
    pageUrl: 'http://127.0.0.1:8765/uat-page.html',
    scenario: null,        // single-scenario filter
    timeoutMs: 30_000,
    _errors: [],
  };
  for (const a of args) {
    if (a === '--strict') opts.mode = 'strict';
    else if (a === '--scaffold') opts.mode = 'scaffold';
    else if (a.startsWith('--image=')) opts.image = a.slice('--image='.length);
    else if (a.startsWith('--cdp=')) opts.cdp = a.slice('--cdp='.length);
    else if (a.startsWith('--page-url=')) opts.pageUrl = a.slice('--page-url='.length);
    else if (a.startsWith('--scenario=')) opts.scenario = a.slice('--scenario='.length);
    else if (a.startsWith('--mode=')) opts.mode = a.slice('--mode='.length);
    else if (a.startsWith('--timeout=')) opts.timeoutMs = parseInt(a.slice('--timeout='.length), 10);
    else if (a === '-h' || a === '--help') opts.help = true;
    else opts._errors.push(`unknown argument: ${a}`);
  }
  return opts;
}

function usage() {
  return [
    'usage: node harness/m4-uat/uat.mjs [options]',
    '',
    '  --scaffold              permit NYI for scenarios whose M4 R# is not yet shipping',
    '  --strict                every enabled scenario must PASS',
    '  --image=TAG             chromeless image tag (for bridge-presence probe)',
    '  --cdp=URL               container CDP endpoint (default: http://localhost:9222)',
    '  --page-url=URL          fixture page url (default: http://127.0.0.1:8765/uat-page.html)',
    '  --scenario=NAME         run only the named scenario',
    '  --timeout=MS            per-scenario timeout (default: 30000)',
    '  -h, --help              show this help',
  ].join('\n');
}

function emitJsonError(code, message) {
  const out = {
    schema: SCHEMA_NAME,
    schemaVersion: SCHEMA_VERSION,
    error: { code, message },
  };
  stdout.write(JSON.stringify(out) + '\n');
}

async function main() {
  const opts = parseArgs(argv.slice(2));
  if (opts.help) {
    stderr.write(usage() + '\n');
    return 0;
  }
  if (opts._errors.length) {
    stderr.write(opts._errors.join('\n') + '\n');
    stderr.write(usage() + '\n');
    emitJsonError('usage', opts._errors[0]);
    return 1;
  }

  stderr.write(`m4-uat: mode=${opts.mode} cdp=${opts.cdp} image=${opts.image}\n`);

  // 1. Open CDP + navigate to the fixture page.
  let cdp;
  try {
    cdp = await openCdp(opts.cdp, opts.timeoutMs);
    await navigatePage(cdp, opts.pageUrl);
  } catch (err) {
    emitJsonError('cdp-unreachable', err.message);
    return 1;
  }

  // 2. Open the native DataChannel via the M3 signaling client.
  //    On scaffold mode, a connection failure is recoverable —
  //    every scenario will mark itself SKIPPED-for-verdict with
  //    reason 'datachannel-not-open'. On strict mode it's fatal.
  let dc;
  let dcOpenError = null;
  try {
    dc = await openNativeDataChannel({ cdp: opts.cdp, timeoutMs: opts.timeoutMs });
  } catch (err) {
    dcOpenError = err;
    stderr.write(`m4-uat: datachannel open failed: ${err.message}\n`);
    if (opts.mode === 'strict') {
      emitJsonError('datachannel-not-open', err.message);
      return 1;
    }
  }

  // 3. Run scenarios.
  const scenarioFilter = opts.scenario
    ? (s => s.id === opts.scenario)
    : (() => true);
  const enabled = SCENARIOS.filter(scenarioFilter);
  if (opts.scenario && enabled.length === 0) {
    emitJsonError('unknown-scenario', `no scenario named '${opts.scenario}'`);
    return 1;
  }

  const ctx = {
    cdp,
    dc,
    dcOpenError,
    buildEnvelope,
    readLastInputEvent,
    evaluateScript,
    mode: opts.mode,
    timeoutMs: opts.timeoutMs,
    pageUrl: opts.pageUrl,
  };

  const results = [];
  for (const s of enabled) {
    const t0 = Date.now();
    let r;
    try {
      r = await s.run(ctx);
    } catch (err) {
      r = { status: Status.FAIL, evidence: { error: err.message, stack: err.stack } };
    }
    const durationMs = Date.now() - t0;
    results.push({ id: s.id, r9Surface: s.r9Surface, ...r, durationMs });
    stderr.write(
      `m4-uat: ${s.id.padEnd(20)} ${(r.status || 'UNKNOWN').padEnd(8)} ` +
      `${durationMs}ms\n`
    );
  }

  // 4. Bridge-presence assertion (negative-of-M7).
  let bridgePresence;
  try {
    bridgePresence = await probeBridgePresence({ imageTag: opts.image });
  } catch (err) {
    bridgePresence = { status: Status.FAIL, evidence: { error: err.message } };
  }
  stderr.write(
    `m4-uat: ${'bridge-presence'.padEnd(20)} ${bridgePresence.status.padEnd(8)} (negative-of-M7)\n`
  );

  // 5. Compute verdict.
  //
  // SCAFFOLD: PASS as long as no scenario is FAIL, and bridge-presence is
  //           PASS. NYI / SKIPPED are tolerated.
  // STRICT:   PASS only if every scenario is PASS and bridge-presence is
  //           PASS.
  const anyFail = results.some(r => r.status === Status.FAIL)
    || bridgePresence.status === Status.FAIL;
  const allPass = results.every(r => r.status === Status.PASS);
  const verdict = opts.mode === 'strict'
    ? (anyFail || !allPass ? 'FAIL' : 'PASS')
    : (anyFail ? 'FAIL' : 'PASS');
  const exitCode = verdict === 'PASS' ? 0 : 1;

  // 6. Tear down.
  try { dc && await dc.close(); } catch { /* noop */ }
  try { cdp && await cdp.close(); } catch { /* noop */ }

  // 7. Emit JSON.
  const out = {
    schema: SCHEMA_NAME,
    schemaVersion: SCHEMA_VERSION,
    mode: opts.mode,
    imageTag: opts.image,
    cdp: opts.cdp,
    pageUrl: opts.pageUrl,
    gate: { verdict, exitCode },
    scenarios: results,
    bridgePresence,
  };
  stdout.write(JSON.stringify(out, null, 2) + '\n');
  return exitCode;
}

main()
  .then(code => exit(code))
  .catch(err => {
    stderr.write(`m4-uat: fatal: ${err.message}\n${err.stack}\n`);
    emitJsonError('fatal', err.message);
    exit(1);
  });

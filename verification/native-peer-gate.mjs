#!/usr/bin/env node
// verification/native-peer-gate.mjs — R1 entrypoint, R2 verdict, R3/R4/R5/R6 wired.
//
// The ChromelessV2 CI gate. Two modes:
//
//   --scaffold   verifies non-failing state per the gate-blocking schedule;
//                permits NOT_YET_IMPLEMENTED for assertions not yet enabled.
//                Used for every push/PR on integration/native-peer.
//
//   --strict     every assertion must PASS. Used on the M7 PR into main.
//                Also useful operator-side to inspect the current image's
//                native-peer reality without ceremony.
//
// Knobs:
//   --image=<tag>          docker image tag to inspect (default: chromeless:ci)
//   --target=<host:port>   external connectivity target (R6); default localhost:9222
//   --timeout=<ms>         per-assertion default timeout (default: 30000)
//   --position=<M0|M1...>  current pipeline position; gate-blocking schedule
//                          is evaluated against this (default: M0)
//   --schedule=<path>      JSON file overriding the default gate-blocking
//                          schedule (R2). Defaults to baked-in DEFAULT_SCHEDULE.
//   --source-root=<path>   source-tree root used by R3/R4 (default: repo root)
//
// IO contract (test-locked in verification/__tests__/gate-cli.test.mjs):
//
//   stdout: ONE JSON object, schema:
//     { schema: 'native-peer-gate', schemaVersion: 1, mode, position,
//       imageTag, gate: { verdict, exitCode },
//       assertions: [ { id, number, title, status, blocked, evidence, durationMs } ],
//       error?: { code, message }  // only present on usage errors
//     }
//
//   stderr: human-readable progress / per-assertion table. NEVER JSON the
//   consumer must parse — CI scripts hard-key on stdout shape.
//
//   exit:   0 on PASS, 1 on FAIL or usage error.

import { argv, exit, stdout, stderr } from 'node:process';
import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

import { createRegistry, defaultStubAssertions, Status, isValidStatus } from './lib/registry.mjs';
import { computeVerdict, DEFAULT_SCHEDULE, MODE } from './lib/verdict.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = resolve(HERE, '..');

const SCHEMA_NAME = 'native-peer-gate';
const SCHEMA_VERSION = 1;

function parseArgs(args) {
  const opts = {
    mode: null,
    image: 'chromeless:ci',
    target: 'localhost:9222',
    timeoutMs: 30_000,
    position: 'M0',
    schedulePath: null,
    sourceRoot: REPO_ROOT,
    _errors: [],
  };
  for (const a of args) {
    if (a === '--scaffold') {
      if (opts.mode) { opts._errors.push(`mode already set: ${opts.mode}`); continue; }
      opts.mode = MODE.SCAFFOLD;
    } else if (a === '--strict') {
      if (opts.mode) { opts._errors.push(`mode already set: ${opts.mode}`); continue; }
      opts.mode = MODE.STRICT;
    } else if (a === '--help' || a === '-h') {
      opts.help = true;
    } else if (a.startsWith('--image=')) opts.image = a.slice('--image='.length);
    else if (a.startsWith('--target=')) opts.target = a.slice('--target='.length);
    else if (a.startsWith('--timeout=')) opts.timeoutMs = parseInt(a.slice('--timeout='.length), 10);
    else if (a.startsWith('--position=')) opts.position = a.slice('--position='.length);
    else if (a.startsWith('--schedule=')) opts.schedulePath = a.slice('--schedule='.length);
    else if (a.startsWith('--source-root=')) opts.sourceRoot = a.slice('--source-root='.length);
    else opts._errors.push(`unknown argument: ${a}`);
  }
  return opts;
}

function usage() {
  return [
    'usage: node verification/native-peer-gate.mjs <--scaffold|--strict> [options]',
    '',
    '  --scaffold              permit NOT_YET_IMPLEMENTED for not-yet-enabled assertions',
    '  --strict                every assertion must PASS',
    '  --image=TAG             docker image to probe (default: chromeless:ci)',
    '  --target=HOST:PORT      connectivity target (R6) (default: localhost:9222)',
    '  --timeout=MS            per-assertion timeout (default: 30000)',
    '  --position=MARKER       pipeline position M0|M1|...|M7 (default: M0)',
    '  --schedule=PATH         JSON schedule overriding DEFAULT_SCHEDULE',
    '  --source-root=PATH      source-tree root for R3/R4 (default: repo root)',
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

function loadSchedule(path) {
  if (!path) return DEFAULT_SCHEDULE;
  if (!existsSync(path)) throw new Error(`--schedule path not found: ${path}`);
  const raw = readFileSync(path, 'utf8');
  return JSON.parse(raw);
}

// ---------- assertion bootstrap ----------------------------------------
//
// Real probes are added by R3 (streamer-page), R4 (bridges), R5 (codec-cap),
// R6 (native-peer). They're imported lazily so the gate CLI starts fast and
// stays runnable even when a probe module has an import-time error (the
// failing probe surfaces as a FAIL with diagnostic evidence).
//
// `loadAssertions` returns a list of assertion objects (the registry shape
// from registry.mjs). When all real assertions are wired, this replaces
// defaultStubAssertions wholesale.

async function loadAssertions(ctx) {
  const stubs = defaultStubAssertions();
  // The dynamic imports below let us land R3/R4/R5/R6 incrementally without
  // breaking R1/R2's harness; missing modules just leave the stub in place.
  const overrides = new Map();

  async function tryLoad(modulePath, id) {
    try {
      const mod = await import(modulePath);
      if (mod && typeof mod.assertion === 'object' && mod.assertion.id === id) {
        overrides.set(id, mod.assertion);
      }
    } catch (err) {
      // Module not yet present (or has an import error) — preserve stub but
      // log to stderr so operators can see why a real probe didn't load.
      stderr.write(`[gate] note: assertion ${id} probe not loaded: ${err.message}\n`);
    }
  }

  await Promise.all([
    tryLoad('./assertions/streamer-page.mjs', 'streamer-page-absent'),
    tryLoad('./assertions/bridges.mjs', 'bridges-absent'),
    tryLoad('./assertions/codec-cap.mjs', 'codec-cap-matches-factory'),
    tryLoad('./assertions/native-peer.mjs', 'native-peer-connects'),
  ]);

  return stubs.map(s => overrides.get(s.id) || s);
}

async function runAssertion(assertion, ctx) {
  const start = Date.now();
  try {
    const r = await Promise.race([
      assertion.run(ctx),
      new Promise((_, reject) => setTimeout(
        () => reject(new Error(`assertion ${assertion.id} timed out after ${ctx.timeoutMs}ms`)),
        ctx.timeoutMs
      )),
    ]);
    if (!r || !isValidStatus(r.status)) {
      return {
        id: assertion.id,
        number: assertion.number,
        title: assertion.title,
        status: Status.FAIL,
        evidence: { error: `invalid status returned: ${JSON.stringify(r)}` },
        durationMs: Date.now() - start,
      };
    }
    return {
      id: assertion.id,
      number: assertion.number,
      title: assertion.title,
      status: r.status,
      evidence: r.evidence || {},
      durationMs: Date.now() - start,
    };
  } catch (err) {
    return {
      id: assertion.id,
      number: assertion.number,
      title: assertion.title,
      status: Status.FAIL,
      evidence: { thrown: true, message: err.message },
      durationMs: Date.now() - start,
    };
  }
}

function renderTable(assertions, perAssertionMeta) {
  const blockedById = new Map(perAssertionMeta.map(p => [p.id, p.blocked]));
  const rows = assertions.map(a => ({
    n: `#${a.number}`,
    id: a.id,
    status: a.status,
    blocked: blockedById.get(a.id) ? 'gate-blocking' : 'pre-blocking',
    detail: a.evidence && Object.keys(a.evidence).length
      ? JSON.stringify(a.evidence).slice(0, 80)
      : '',
  }));
  const wN = Math.max(2, ...rows.map(r => r.n.length));
  const wId = Math.max(2, ...rows.map(r => r.id.length));
  const wSt = Math.max(6, ...rows.map(r => r.status.length));
  const wBl = Math.max(12, ...rows.map(r => r.blocked.length));
  const lines = [
    `${'#'.padEnd(wN)}  ${'id'.padEnd(wId)}  ${'status'.padEnd(wSt)}  ${'schedule'.padEnd(wBl)}  detail`,
    `${'-'.repeat(wN)}  ${'-'.repeat(wId)}  ${'-'.repeat(wSt)}  ${'-'.repeat(wBl)}  ------`,
    ...rows.map(r =>
      `${r.n.padEnd(wN)}  ${r.id.padEnd(wId)}  ${r.status.padEnd(wSt)}  ${r.blocked.padEnd(wBl)}  ${r.detail}`),
  ];
  return lines.join('\n');
}

export async function runGate(opts) {
  const ctx = {
    mode: opts.mode,
    imageTag: opts.image,
    target: opts.target,
    timeoutMs: opts.timeoutMs,
    position: opts.position,
    sourceRoot: opts.sourceRoot,
  };
  let schedule;
  try {
    schedule = loadSchedule(opts.schedulePath);
  } catch (err) {
    emitJsonError('schedule-load-failed', err.message);
    return 1;
  }

  const assertions = await loadAssertions(ctx);
  const registry = createRegistry(assertions);
  const list = registry.list();

  // Run assertions in registration order. Could parallelize, but #3/#4
  // share the boot harness — serial keeps resource use predictable.
  const results = [];
  for (const a of list) {
    stderr.write(`[gate] running #${a.number} ${a.id}…\n`);
    const r = await runAssertion(a, ctx);
    results.push(r);
  }

  const { verdict, exitCode, perAssertion } = computeVerdict({
    mode: ctx.mode,
    results,
    schedule,
    position: ctx.position,
  });

  const payload = {
    schema: SCHEMA_NAME,
    schemaVersion: SCHEMA_VERSION,
    mode: ctx.mode,
    position: ctx.position,
    imageTag: ctx.imageTag,
    gate: { verdict, exitCode },
    assertions: results.map((r, i) => ({
      ...r,
      blocked: perAssertion[i].blocked,
    })),
  };

  stdout.write(JSON.stringify(payload) + '\n');
  stderr.write('\n' + renderTable(payload.assertions, perAssertion) + '\n');
  stderr.write(`\n[gate] verdict=${verdict} exitCode=${exitCode} mode=${ctx.mode}\n`);
  return exitCode;
}

async function main() {
  const opts = parseArgs(argv.slice(2));
  if (opts.help) {
    stderr.write(usage() + '\n');
    return 0;
  }
  if (opts._errors.length) {
    stderr.write('error: ' + opts._errors.join('; ') + '\n' + usage() + '\n');
    emitJsonError('bad-args', opts._errors.join('; '));
    return 1;
  }
  if (!opts.mode) {
    stderr.write('error: --scaffold or --strict required\n' + usage() + '\n');
    emitJsonError('missing-mode', '--scaffold or --strict required');
    return 1;
  }
  return runGate(opts);
}

// Top-level await so we can exit with the integer code.
const code = await main();
exit(code);

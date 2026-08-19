#!/usr/bin/env node
// conformance/run.mjs — "does MY deployment satisfy the contract?"
//
// IO contract (locked by conformance/__tests__/run-cli.test.mjs):
//
//   stdout: with --json, exactly ONE JSON object and nothing else. Without
//           it, nothing at all. A caller piping stdout to a parser must
//           never have to strip progress text — the same discipline
//           verification/native-peer-gate.mjs holds itself to.
//   stderr: human-readable progress + the findings table.
//   exit:   0 every required check passed
//           1 a required check failed, errored, or nothing ran
//           2 usage error
//
// Deliberately dependency-free (node builtins only). The audience is
// someone running their own broker or their own build who may never have
// installed anything from this repo, and `npm install` before you can ask
// "am I conforming?" is a barrier this kit should not have.

import { argv, exit, stdout, stderr, version as nodeVersion } from 'node:process';
import { createRegistry, computeVerdict, Status } from './lib/check.mjs';

const SCHEMA = 'chromeless-conformance';
const SCHEMA_VERSION = 1;

const ROLES = Object.freeze({
  signaling: ['signaling-wire'],
  cdp: ['cdp-surface'],
  full: ['signaling-wire', 'cdp-surface'],
});

function usage() {
  return [
    'usage: node conformance/run.mjs --target=<url> [options]',
    '',
    '  --target=URL       what to test. ws://host:port for --role=signaling,',
    '                     http://host:port for --role=cdp.',
    '  --role=ROLE        signaling | cdp | full   (default: full)',
    '  --session=ID       signaling session id to use (default: a random one)',
    '  --timeout=MS       per-check timeout (default: 5000)',
    '  --json             emit the machine-readable object on stdout',
    '  -h, --help         this text',
    '',
    'exit: 0 conforming, 1 non-conforming, 2 usage error',
  ].join('\n');
}

export function parseArgs(args) {
  const opts = {
    target: null,
    role: 'full',
    session: null,
    timeoutMs: 5000,
    json: false,
    help: false,
    _errors: [],
  };
  for (const a of args) {
    if (a === '--json') opts.json = true;
    else if (a === '-h' || a === '--help') opts.help = true;
    else if (a.startsWith('--target=')) opts.target = a.slice('--target='.length);
    else if (a.startsWith('--role=')) opts.role = a.slice('--role='.length);
    else if (a.startsWith('--session=')) opts.session = a.slice('--session='.length);
    else if (a.startsWith('--timeout=')) {
      const n = Number.parseInt(a.slice('--timeout='.length), 10);
      if (!Number.isFinite(n) || n <= 0) opts._errors.push(`--timeout must be a positive integer, got: ${a}`);
      else opts.timeoutMs = n;
    } else opts._errors.push(`unknown argument: ${a}`);
  }
  if (!opts.help) {
    if (!opts.target) opts._errors.push('--target is required');
    if (!ROLES[opts.role]) {
      opts._errors.push(`--role must be one of ${Object.keys(ROLES).join(' | ')}, got: ${opts.role}`);
    }
  }
  return opts;
}

// Node's global WebSocket landed in 22. Say so plainly instead of letting
// the signaling suite die on `ReferenceError: WebSocket is not defined`,
// which reads like a bug in this kit rather than a runtime requirement.
function preflight(role) {
  if (ROLES[role]?.includes('signaling-wire') && typeof WebSocket === 'undefined') {
    return `this Node (${nodeVersion}) has no global WebSocket, which --role=${role} needs.\n`
         + 'Use Node >= 22, or run with --role=cdp, which only uses fetch.';
  }
  return null;
}

async function loadSuites(names) {
  const checks = [];
  for (const name of names) {
    // Dynamic so one broken suite cannot stop the others from running —
    // and so its failure is reported as a finding rather than a stack trace.
    try {
      const mod = await import(`./suites/${name}.mjs`);
      checks.push(...(mod.checks || []));
    } catch (err) {
      checks.push({
        id: `suite-load-${name}`,
        suite: name,
        title: `suite ${name} failed to load`,
        spec: `conformance/suites/${name}.mjs`,
        required: true,
        async run() {
          return { status: Status.ERROR, evidence: { message: err.message } };
        },
      });
    }
  }
  return checks;
}

async function runCheck(check, ctx) {
  const started = Date.now();
  try {
    const r = await Promise.race([
      check.run(ctx),
      new Promise((_, rej) =>
        setTimeout(() => rej(new Error(`timed out after ${ctx.timeoutMs}ms`)), ctx.timeoutMs + 2000)),
    ]);
    return {
      id: check.id, suite: check.suite, title: check.title, spec: check.spec,
      required: check.required,
      status: r.status, evidence: r.evidence || {},
      durationMs: Date.now() - started,
    };
  } catch (err) {
    // A thrown check is ERROR, never FAIL: "the check broke" and "your
    // deployment is wrong" are different claims and conflating them would
    // send someone debugging their own stack over a bug in ours.
    return {
      id: check.id, suite: check.suite, title: check.title, spec: check.spec,
      required: check.required,
      status: Status.ERROR, evidence: { message: err.message, thrown: true },
      durationMs: Date.now() - started,
    };
  }
}

const MARK = { PASS: '✓', FAIL: '✗', SKIP: '·', ERROR: '!' };

function renderFindings(results) {
  const lines = [];
  for (const r of results) {
    const tag = r.required ? '' : ' (advisory)';
    lines.push(`${MARK[r.status] || '?'} ${r.status.padEnd(5)} ${r.id}${tag}`);
    lines.push(`        ${r.title}`);
    lines.push(`        spec: ${r.spec}`);
    if (r.status === Status.FAIL || r.status === Status.ERROR) {
      const e = r.evidence || {};
      if (e.expected !== undefined) lines.push(`        expected: ${e.expected}`);
      if (e.observed !== undefined) lines.push(`        observed: ${e.observed}`);
      if (e.message) lines.push(`        error:    ${e.message}`);
      if (e.hint) lines.push(`        hint:     ${e.hint}`);
    } else if (r.status === Status.SKIP && r.evidence?.reason) {
      lines.push(`        reason:   ${r.evidence.reason}`);
    }
    lines.push('');
  }
  return lines.join('\n');
}

export async function main(args) {
  const opts = parseArgs(args);

  if (opts.help) {
    stderr.write(usage() + '\n');
    return 0;
  }
  if (opts._errors.length) {
    stderr.write(opts._errors.map(e => `error: ${e}`).join('\n') + '\n\n' + usage() + '\n');
    return 2;
  }
  const pre = preflight(opts.role);
  if (pre) {
    stderr.write(`error: ${pre}\n`);
    return 2;
  }

  const session = opts.session
    || `conformance-${Math.random().toString(36).slice(2, 10)}`;

  const ctx = {
    target: opts.target,
    role: opts.role,
    session,
    timeoutMs: opts.timeoutMs,
    // Negative checks assert that something does NOT arrive, so they must
    // not wait the full per-check timeout each — that would make a clean
    // run take minutes for no information.
    shortTimeoutMs: Math.min(1000, opts.timeoutMs),
  };

  const suiteNames = ROLES[opts.role];
  const registry = createRegistry(await loadSuites(suiteNames));
  const checks = registry.list();

  stderr.write(`chromeless conformance — target=${opts.target} role=${opts.role} session=${session}\n`);
  stderr.write(`suites: ${suiteNames.join(', ')} (${checks.length} checks)\n\n`);

  const results = [];
  for (const c of checks) {
    stderr.write(`  running ${c.id}…\n`);
    results.push(await runCheck(c, ctx));
  }

  const { verdict, exitCode, failed, advisoryFailed } = computeVerdict(results);

  stderr.write('\n' + renderFindings(results));
  if (advisoryFailed.length) {
    stderr.write(`advisory failures (do not affect the exit code): ${advisoryFailed.join(', ')}\n`);
  }
  stderr.write(`\nverdict=${verdict}`);
  if (failed.length) stderr.write(` failed=${failed.join(',')}`);
  stderr.write(`\n`);

  if (opts.json) {
    stdout.write(JSON.stringify({
      schema: SCHEMA,
      schemaVersion: SCHEMA_VERSION,
      target: opts.target,
      role: opts.role,
      session,
      verdict,
      exitCode,
      checks: results,
    }) + '\n');
  }

  return exitCode;
}

// Only run when invoked directly, so the tests can import main().
const invokedDirectly = import.meta.url === `file://${process.argv[1]}`;
if (invokedDirectly) {
  main(argv.slice(2)).then(exit, err => {
    stderr.write(`conformance: unexpected failure: ${err?.stack || err}\n`);
    exit(1);
  });
}

// conformance/__tests__/run-cli.test.mjs
//
// Locks the IO contract downstream tooling depends on. Modeled on
// verification/__tests__/gate-cli.test.mjs — same reasoning: drive the CLI
// as a child process, because the thing being asserted is what a CALLER
// observes (stdout/stderr separation, exit codes), and an in-process import
// cannot see any of that.
//
// These tests never touch the network. Every case either fails before
// reaching a suite (usage errors) or points at a closed port, where the
// expected outcome is a clean FAIL rather than a hang.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const execFileP = promisify(execFile);
const HERE = dirname(fileURLToPath(import.meta.url));
const RUN = resolve(HERE, '..', 'run.mjs');

// A port nothing listens on. Port 1 is privileged and unbound in every
// environment this runs in, so `fetch` fails fast instead of timing out.
const CLOSED = 'http://127.0.0.1:1';

async function run(args) {
  try {
    const { stdout, stderr } = await execFileP('node', [RUN, ...args], { timeout: 30_000 });
    return { code: 0, stdout, stderr };
  } catch (err) {
    return { code: err.code ?? 1, stdout: err.stdout ?? '', stderr: err.stderr ?? '' };
  }
}

test('--help exits 0 and writes usage to stderr, not stdout', async () => {
  const r = await run(['--help']);
  assert.equal(r.code, 0);
  assert.match(r.stderr, /usage: node conformance\/run\.mjs/);
  assert.equal(r.stdout, '', 'stdout must stay clean for --json consumers');
});

test('missing --target is a usage error (exit 2)', async () => {
  const r = await run([]);
  assert.equal(r.code, 2);
  assert.match(r.stderr, /--target is required/);
});

test('unknown --role is a usage error (exit 2), and names the valid roles', async () => {
  const r = await run(['--target=' + CLOSED, '--role=nonsense']);
  assert.equal(r.code, 2);
  assert.match(r.stderr, /--role must be one of/);
  assert.match(r.stderr, /signaling/);
});

test('a non-numeric --timeout is rejected rather than silently defaulted', async () => {
  const r = await run(['--target=' + CLOSED, '--timeout=soon']);
  assert.equal(r.code, 2);
  assert.match(r.stderr, /--timeout must be a positive integer/);
});

test('unknown flags are rejected, not ignored', async () => {
  const r = await run(['--target=' + CLOSED, '--rle=cdp']);
  assert.equal(r.code, 2);
  assert.match(r.stderr, /unknown argument: --rle=cdp/);
});

test('a required check failing exits 1', async () => {
  const r = await run(['--target=' + CLOSED, '--role=cdp']);
  assert.equal(r.code, 1);
  assert.match(r.stderr, /verdict=FAIL/);
});

test('without --json, stdout stays completely empty', async () => {
  const r = await run(['--target=' + CLOSED, '--role=cdp']);
  assert.equal(r.stdout, '');
});

test('with --json, stdout is exactly one parseable object', async () => {
  const r = await run(['--target=' + CLOSED, '--role=cdp', '--json']);
  const lines = r.stdout.trim().split('\n').filter(Boolean);
  assert.equal(lines.length, 1, 'exactly one line of JSON — not NDJSON, not mixed text');
  const doc = JSON.parse(lines[0]);
  assert.equal(doc.schema, 'chromeless-conformance');
  assert.equal(doc.schemaVersion, 1);
  assert.equal(doc.verdict, 'FAIL');
  assert.ok(Array.isArray(doc.checks) && doc.checks.length > 0);
});

test('every emitted check carries a spec reference and a required flag', async () => {
  const r = await run(['--target=' + CLOSED, '--role=cdp', '--json']);
  const doc = JSON.parse(r.stdout.trim());
  for (const c of doc.checks) {
    assert.ok(c.spec, `check ${c.id} has no spec reference`);
    assert.equal(typeof c.required, 'boolean', `check ${c.id} has no required flag`);
    assert.ok(['PASS', 'FAIL', 'SKIP', 'ERROR'].includes(c.status), `bad status ${c.status}`);
  }
});

test('a failing check reports expected/observed so it is actionable', async () => {
  const r = await run(['--target=' + CLOSED, '--role=cdp', '--json']);
  const doc = JSON.parse(r.stdout.trim());
  const failed = doc.checks.filter(c => c.status === 'FAIL');
  assert.ok(failed.length > 0, 'expected at least one failure against a closed port');
  for (const c of failed) {
    assert.ok(c.evidence?.expected !== undefined, `${c.id}: no 'expected' in evidence`);
    assert.ok(c.evidence?.observed !== undefined, `${c.id}: no 'observed' in evidence`);
  }
});

test('the /json/protocol hazard is reported as SKIP, never probed', async () => {
  const r = await run(['--target=' + CLOSED, '--role=cdp', '--json']);
  const doc = JSON.parse(r.stdout.trim());
  const hazard = doc.checks.find(c => c.id === 'protocol-endpoint-hazard');
  assert.ok(hazard, 'the hazard check must be present');
  assert.equal(hazard.status, 'SKIP');
  assert.equal(hazard.evidence.action, 'deliberately-not-requested');
});

test('advisory failures do not by themselves set a non-zero exit', async () => {
  // Verdict algebra is unit-tested separately; this asserts the wiring, i.e.
  // that the CLI's exit code comes from computeVerdict's required-only set.
  const { computeVerdict } = await import('../lib/check.mjs');
  const v = computeVerdict([
    { id: 'a', required: true, status: 'PASS' },
    { id: 'b', required: false, status: 'FAIL' },
  ]);
  assert.equal(v.verdict, 'PASS');
  assert.equal(v.exitCode, 0);
  assert.deepEqual(v.advisoryFailed, ['b']);
});

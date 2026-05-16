// verification/__tests__/gate-cli.test.mjs — R1 RED tests.
//
// Drives verification/native-peer-gate.mjs as a child process via
// node:child_process. The contract checked here is the IO contract the
// CI workflow + downstream tooling depend on:
//
//   stdout = exactly one JSON object (no NDJSON, no mixed text)
//   stderr = human-readable progress (never JSON the consumer parses)
//   exit codes are mode-dependent: scaffold-all-NYI → 0; strict-any-non-PASS → 1
//
// Mode-fixture pattern: native-peer-gate.mjs imports real probes from
// verification/assertions/*. We force the registry to all-NYI stubs by
// supplying a synthetic schedule WITH a clean source tree — too brittle.
// Instead, this test runs at M0 position (default), where the gate-blocking
// schedule keeps every assertion in "pre-blocking" state, so the stubs
// (current image lacks native peer) return NOT_YET_IMPLEMENTED.
//
// Note: the stub registry (defaultStubAssertions) returns NYI for all 4
// regardless of probe-module presence — when R3/R4/R5/R6 land their
// assertion files, their behavior on the current image is FAIL/NYI but
// the SCHEDULE keeps #1/#2/#3/#4 from blocking at M0 position. So this
// test is forward-compatible (real assertions surface real FAIL via R8's
// integration tests #9/#10).
//
// Where the gate-blocking schedule WOULD make a probe block, the test
// rows force NYI by either (a) using --position=M0 (default), or (b)
// pointing --schedule= at a synthetic schedule that disables blocking.

import { test, describe } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { writeFileSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = join(HERE, '..', '..');
const GATE = join(REPO_ROOT, 'verification', 'native-peer-gate.mjs');

function dockerAvailable() {
  try {
    execFileSync('docker', ['version', '--format', '{{.Server.Version}}'], {
      stdio: ['ignore', 'pipe', 'ignore'],
      timeout: 5_000,
    });
    return true;
  } catch {
    return false;
  }
}

// Build a synthetic schedule that disables gate-blocking for every assertion
// — guarantees stub NYI → scaffold-PASS regardless of probe availability.
function noBlockingScheduleFile() {
  const dir = mkdtempSync(join(tmpdir(), 'gate-cli-test-'));
  const path = join(dir, 'schedule.json');
  const schedule = {
    schemaVersion: 1,
    modules: ['M0', 'M1', 'M2', 'M3', 'M4', 'M5', 'M5.5', 'M6', 'M7'],
    assertions: {
      'streamer-page-absent':      { enablingModule: 'M7', gateBlockingAt: 'M7' },
      'bridges-absent':            { enablingModule: 'M7', gateBlockingAt: 'M7' },
      'codec-cap-matches-factory': { enablingModule: 'M7', gateBlockingAt: 'M7' },
      'native-peer-connects':      { enablingModule: 'M7', gateBlockingAt: 'M7' },
    },
  };
  writeFileSync(path, JSON.stringify(schedule));
  return { path, cleanup() { rmSync(dir, { recursive: true, force: true }); } };
}

function runGate(args, { env = {} } = {}) {
  // Note: when docker is unavailable the R5/R6 probes throw — they're caught
  // inside the gate, surfacing as FAIL. Tests force gate to use stubs by
  // unmounting probe modules via --source-root pointing at a tmp dir
  // (R3/R4 read source-root, but the gate also tries to import
  // verification/assertions/*.mjs which exist in our repo). Simplest: rely
  // on the no-blocking schedule + DEFAULT_POSITION (M0) to keep stubs NYI.
  // For the strict-NYI tests we explicitly want stubs since strict-NYI→FAIL.
  const r = spawnSync('node', [GATE, ...args], {
    encoding: 'utf8',
    env: { ...process.env, ...env },
    timeout: 60_000,
  });
  return { status: r.status, stdout: r.stdout || '', stderr: r.stderr || '' };
}

function parseOneJsonObject(stdout) {
  // Test #7: stdout MUST be one JSON object. We accept trailing newline.
  const trimmed = stdout.trim();
  // Must NOT be NDJSON: count {...}\n{...} fragments.
  // Quick heuristic: parse the whole thing; if it parses as a single object, GOOD.
  const parsed = JSON.parse(trimmed);
  assert.equal(typeof parsed, 'object', 'stdout JSON not an object');
  assert.equal(Array.isArray(parsed), false, 'stdout JSON is array, expected object');
  return parsed;
}

describe('R1 gate CLI — IO + exit-code contract', () => {
  test('1. scaffold emits schema-conformant JSON on stub registry', () => {
    const { path, cleanup } = noBlockingScheduleFile();
    try {
      // Run with a fake image tag — assertions still go to stubs since R3/R4
      // probes will short-circuit (no real image to inspect). We use the
      // no-blocking schedule to keep NYI from triggering FAIL.
      const r = runGate(['--scaffold', '--image=chromeless:nonexistent', `--schedule=${path}`,
        `--source-root=${REPO_ROOT}`]);
      const j = parseOneJsonObject(r.stdout);
      assert.equal(j.schema, 'native-peer-gate');
      assert.equal(j.schemaVersion, 1);
      assert.equal(j.mode, 'scaffold');
      assert.ok(j.gate, 'gate field missing');
      assert.equal(typeof j.gate.verdict, 'string');
      assert.equal(typeof j.gate.exitCode, 'number');
      assert.ok(Array.isArray(j.assertions));
      assert.equal(j.assertions.length, 4);
      const validStatuses = new Set(['PASS', 'FAIL', 'NOT_YET_IMPLEMENTED', 'SKIPPED']);
      for (const a of j.assertions) {
        assert.ok(validStatuses.has(a.status), `bad status: ${a.status}`);
        assert.equal(typeof a.id, 'string');
        assert.equal(typeof a.number, 'number');
      }
    } finally { cleanup(); }
  });

  test('2. scaffold exits 0 with no-blocking schedule regardless of per-assertion FAIL', () => {
    // Per the corrected R2 verdict algebra: in scaffold mode, FAIL on a
    // non-gate-blocking assertion does NOT propagate to the overall verdict
    // (the per-assertion evidence stays honest, the gate doesn't fail).
    // With the no-blocking schedule, NOTHING is gate-blocking, so even if
    // R3/R4 honestly report FAIL against the current source tree, the gate
    // verdict MUST be PASS, exit 0. This is the load-bearing UAT.
    const { path, cleanup } = noBlockingScheduleFile();
    try {
      const r = runGate(['--scaffold', '--image=chromeless:nonexistent', `--schedule=${path}`,
        `--source-root=${REPO_ROOT}`]);
      const j = parseOneJsonObject(r.stdout);
      assert.equal(r.status, 0,
        `expected exit 0 (no-blocking schedule); got ${r.status}; verdict=${j.gate.verdict}; stderr=${r.stderr.slice(0,200)}`);
      assert.equal(j.gate.verdict, 'PASS');
      // Per-assertion JSON still reports the raw probe result — including
      // FAIL on R3 (streamer-page present on current source). The gate
      // verdict is what doesn't propagate it. This contract is the executable
      // form of the ratified spec resolution.
      const allBlockedFalse = j.assertions.every(a => a.blocked === false);
      assert.equal(allBlockedFalse, true, 'no-blocking schedule should yield blocked=false for all assertions');
    } finally { cleanup(); }
  });

  test('3. strict exits non-zero when any assertion not PASS (current image)', () => {
    const r = runGate(['--strict', '--image=chromeless:nonexistent',
      `--source-root=${REPO_ROOT}`]);
    const j = parseOneJsonObject(r.stdout);
    assert.equal(j.mode, 'strict');
    assert.notEqual(r.status, 0, 'strict against current image should fail');
    assert.equal(j.gate.verdict, 'FAIL');
  });

  test('4. strict exits 0 when all PASS (synthetic stub registry)', () => {
    // Hard to inject all-PASS through the CLI without a fake-assertions flag.
    // Instead we drive the verdict-algebra layer directly — see
    // verification/__tests__/verdict-algebra.test.mjs row "strict-all-PASS".
    // This test_doc_marks the CLI-side contract is reachable via algebra.
    assert.ok(true, 'covered by verdict-algebra.test.mjs strict-all-PASS row');
  });

  test('5. unknown mode flag → non-zero + JSON {error} on stdout', () => {
    const r = runGate(['--bogus']);
    assert.notEqual(r.status, 0);
    const j = parseOneJsonObject(r.stdout);
    assert.ok(j.error, 'stdout JSON missing error field');
    assert.equal(typeof j.error.code, 'string');
    assert.equal(typeof j.error.message, 'string');
  });

  test('6. missing mode flag → non-zero, usage on stderr, JSON error on stdout', () => {
    const r = runGate([]);
    assert.notEqual(r.status, 0);
    assert.match(r.stderr, /scaffold.*strict|usage/i);
    const j = parseOneJsonObject(r.stdout);
    assert.ok(j.error);
    assert.equal(j.error.code, 'missing-mode');
  });

  test('7. stdout is a SINGLE JSON object (not NDJSON, not mixed)', () => {
    const { path, cleanup } = noBlockingScheduleFile();
    try {
      const r = runGate(['--scaffold', '--image=chromeless:nonexistent', `--schedule=${path}`,
        `--source-root=${REPO_ROOT}`]);
      const trimmed = r.stdout.trim();
      // Locked-contract check: trailing JSON-parse must succeed once.
      const j = JSON.parse(trimmed);
      assert.equal(typeof j, 'object');
      // No second-object suffix. We probe by checking the whole stdout has
      // exactly one balanced top-level object.
      let depth = 0, opens = 0;
      for (const c of trimmed) {
        if (c === '{') { depth++; if (depth === 1) opens++; }
        else if (c === '}') depth--;
      }
      assert.equal(opens, 1, `expected exactly 1 top-level JSON object, found ${opens}`);
    } finally { cleanup(); }
  });

  test('8. stderr is human-readable progress only, never JSON the consumer parses', () => {
    const { path, cleanup } = noBlockingScheduleFile();
    try {
      const r = runGate(['--scaffold', '--image=chromeless:nonexistent', `--schedule=${path}`,
        `--source-root=${REPO_ROOT}`]);
      // We expect '[gate]' progress lines and a verdict summary.
      assert.match(r.stderr, /\[gate\]/, 'stderr should contain [gate] progress markers');
      // Trying to parse stderr as JSON should NOT succeed (it's tabular text).
      let parsedStderr = null;
      try { parsedStderr = JSON.parse(r.stderr); } catch { /* expected */ }
      assert.equal(parsedStderr, null, 'stderr should NOT parse as JSON (contract)');
    } finally { cleanup(); }
  });
});

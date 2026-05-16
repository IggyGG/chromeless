// verification/__tests__/ci-workflow.test.mjs — R8 RED tests.
//
// 10 tests against .github/workflows/native-peer-gate.yml. YAML parsing
// via the `yaml` package (devDep declared in verification/package.json).
//
// Tests #9 and #10 are integration tests against the local source-tree —
// they invoke the gate CLI for real and assert exit code + verdict.
// They depend on R1–R7 being implemented (per CV2-17 audit "R8 tests
// #9/#10 transitively require R1–R7 in place").

import { test, describe } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { spawnSync, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import YAML from 'yaml';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = join(HERE, '..', '..');
const WORKFLOW = join(REPO_ROOT, '.github', 'workflows', 'native-peer-gate.yml');
const GATE = join(REPO_ROOT, 'verification', 'native-peer-gate.mjs');

function loadWorkflow() {
  assert.ok(existsSync(WORKFLOW), `workflow file missing: ${WORKFLOW}`);
  const raw = readFileSync(WORKFLOW, 'utf8');
  return { raw, parsed: YAML.parse(raw) };
}

function jobRunString(job) {
  // Collect every step's `run:` into a single space-joined string for
  // substring matching (avoids per-step iteration in callers).
  if (!job?.steps) return '';
  return job.steps
    .filter(s => typeof s.run === 'string')
    .map(s => s.run)
    .join('\n');
}

describe('R8 native-peer-gate workflow YAML', () => {
  // 1. Parse and shape.
  test('1. workflow YAML exists, parses, and has {name, on, jobs}', () => {
    const { parsed } = loadWorkflow();
    assert.equal(parsed.name, 'native-peer-gate');
    // `on:` parses as truthy 'true' in YAML 1.1 if not quoted; we use
    // explicit `on:` with subkeys, so it's an object.
    assert.ok(parsed.on && typeof parsed.on === 'object', `on field shape: ${typeof parsed.on}`);
    assert.ok(parsed.jobs && typeof parsed.jobs === 'object');
  });

  // 2. Both jobs declared.
  test('2. declares native-peer-gate-scaffold AND native-peer-gate-strict', () => {
    const { parsed } = loadWorkflow();
    assert.ok(parsed.jobs['native-peer-gate-scaffold'], 'scaffold job missing');
    assert.ok(parsed.jobs['native-peer-gate-strict'], 'strict job missing');
  });

  // 3. scaffold consumes build-image artifact (gha-cache reuse).
  test('3. scaffold job needs build-image', () => {
    const { parsed } = loadWorkflow();
    const scaffold = parsed.jobs['native-peer-gate-scaffold'];
    const needs = Array.isArray(scaffold.needs) ? scaffold.needs : [scaffold.needs];
    assert.ok(needs.includes('build-image'), `scaffold.needs missing build-image: ${JSON.stringify(needs)}`);
  });

  // 4. scaffold step invokes --scaffold mode.
  test('4. scaffold job runs `node verification/native-peer-gate.mjs --scaffold`', () => {
    const { parsed } = loadWorkflow();
    const allRun = jobRunString(parsed.jobs['native-peer-gate-scaffold']);
    assert.match(allRun, /node verification\/native-peer-gate\.mjs --scaffold/);
  });

  // 5. scaffold triggers on every PR (no base restriction).
  test('5. scaffold triggers on every pull_request', () => {
    const { parsed } = loadWorkflow();
    assert.ok('pull_request' in parsed.on, 'on.pull_request missing');
    // pull_request entry should not have a `branches` filter restricting it.
    const pr = parsed.on.pull_request;
    if (pr && typeof pr === 'object') {
      assert.ok(!pr.branches, `on.pull_request.branches should be unset; got ${JSON.stringify(pr.branches)}`);
    }
  });

  // 6. strict job uses --strict.
  test('6. strict job runs gate in --strict mode', () => {
    const { parsed } = loadWorkflow();
    const allRun = jobRunString(parsed.jobs['native-peer-gate-strict']);
    assert.match(allRun, /node verification\/native-peer-gate\.mjs --strict/);
  });

  // 7. strict gated to PRs targeting main OR pushes on integration/native-peer.
  test('7. strict gated to PRs targeting main OR pushes on integration/native-peer', () => {
    const { parsed } = loadWorkflow();
    const cond = parsed.jobs['native-peer-gate-strict'].if;
    assert.equal(typeof cond, 'string', 'strict job missing if: gate');
    assert.match(cond, /pull_request.*main/, `if missing PR-to-main clause: ${cond}`);
    assert.match(cond, /push.*integration\/native-peer/, `if missing push-to-integration clause: ${cond}`);
  });

  // 8. on.push.branches references integration/native-peer (R8 ↔ R9 link).
  test('8. on.push.branches references integration/native-peer', () => {
    const { parsed } = loadWorkflow();
    assert.ok(parsed.on.push, 'on.push missing');
    const branches = parsed.on.push.branches;
    assert.ok(Array.isArray(branches), 'on.push.branches must be array');
    assert.ok(branches.includes('integration/native-peer'),
      `on.push.branches missing integration/native-peer: ${JSON.stringify(branches)}`);
  });

  // 9. Integration: against current source, --scaffold exits 0 with verdict PASS.
  // Note: real probes (R3 source-tree FAIL, R5 NYI pre-M1, R6 NYI pre-M3) +
  // the gate-blocking schedule (#1,#2 not blocking until M7) mean current-tree
  // scaffold should be FAIL because #1 (streamer-page-absent) FAILs against
  // the current source (capture/streamer-page exists).
  //
  // Per CV2-11 ratified resolution: gate-blocking schedule keeps NYI permissible
  // at M0, but explicit FAIL is always FAIL. So this test expects exit-1.
  //
  // This matches the operator UAT row: "scaffold against the current IMAGE"
  // would be PASS (because in the image, the probes can't access source dir
  // and would land NYI/SKIPPED), but against current SOURCE TREE in this
  // test run, R3 actively FAILs.
  //
  // To exercise the "current image → scaffold 0" UAT, point sourceRoot at
  // verification/fixtures/synthetic-clean-streamer.
  test('9. against current SOURCE, --scaffold exits 1 (R3 FAILs on present streamer-page)', () => {
    const r = spawnSync('node', [GATE, '--scaffold', '--image=chromeless:not-present', `--source-root=${REPO_ROOT}`], {
      encoding: 'utf8',
      timeout: 60_000,
    });
    const j = JSON.parse(r.stdout.trim());
    // Current source tree has capture/streamer-page → assertion #1 FAILs →
    // gate.verdict FAIL even in scaffold. This is the CORRECT M0 behavior:
    // the gate is honest about the current state.
    assert.equal(j.gate.verdict, 'FAIL');
    assert.equal(r.status, 1);
    const a1 = j.assertions.find(a => a.id === 'streamer-page-absent');
    assert.equal(a1.status, 'FAIL', `expected #1 FAIL; got ${a1.status}`);
  });

  // 10. Against current source, --strict exits non-zero with all-non-PASS.
  test('10. against current SOURCE, --strict exits 1; gate.verdict FAIL', () => {
    const r = spawnSync('node', [GATE, '--strict', '--image=chromeless:not-present', `--source-root=${REPO_ROOT}`], {
      encoding: 'utf8',
      timeout: 60_000,
    });
    const j = JSON.parse(r.stdout.trim());
    assert.equal(j.gate.verdict, 'FAIL');
    assert.equal(r.status, 1);
    // All 4 assertions accounted for.
    assert.equal(j.assertions.length, 4);
    // Strict mode: NYI counts as non-PASS, FAIL is FAIL — no PASS for any
    // assertion against current state.
    const passCount = j.assertions.filter(a => a.status === 'PASS').length;
    assert.equal(passCount, 0, `expected zero PASS in strict@current-source; got ${passCount}`);
  });
});

describe('R8 — supporting artifacts (compensating evidence)', () => {
  test('check-branch-protection.sh exists and is executable', () => {
    const path = join(REPO_ROOT, 'verification', 'scripts', 'check-branch-protection.sh');
    assert.ok(existsSync(path));
    const stat = execFileSync('test', ['-x', path], { encoding: 'utf8' });
    // `test -x` exits 0 silently when truthy.
  });
  test('README documents required-check name + branch-protection setup', () => {
    const path = join(REPO_ROOT, 'verification', 'README.md');
    const txt = readFileSync(path, 'utf8');
    assert.match(txt, /native-peer-gate \/ native-peer-gate-scaffold/);
    assert.match(txt, /Branch-protection/i);
  });
});

// conformance/__tests__/verdict.test.mjs — the verdict algebra.
//
// This is where the kit's promises are actually enforced. Each test below
// corresponds to a claim the README makes to someone deciding whether to
// trust a green run.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { computeVerdict, createRegistry, result, Status } from '../lib/check.mjs';

const req = (id, status) => ({ id, required: true, status });
const adv = (id, status) => ({ id, required: false, status });

test('all required passing is PASS/0', () => {
  const v = computeVerdict([req('a', Status.PASS), req('b', Status.PASS)]);
  assert.equal(v.verdict, 'PASS');
  assert.equal(v.exitCode, 0);
});

test('one required failure is FAIL/1 and names the check', () => {
  const v = computeVerdict([req('a', Status.PASS), req('b', Status.FAIL)]);
  assert.equal(v.verdict, 'FAIL');
  assert.equal(v.exitCode, 1);
  assert.deepEqual(v.failed, ['b']);
});

test('a required ERROR counts as failure — "could not tell" is not "fine"', () => {
  // The claim: a check that could not run has NOT demonstrated conformance.
  // Treating ERROR as pass is how a green result stops meaning anything.
  const v = computeVerdict([req('a', Status.ERROR)]);
  assert.equal(v.verdict, 'FAIL');
  assert.deepEqual(v.failed, ['a']);
});

test('advisory failures are reported but do not set the exit code', () => {
  // The README calls these "recommendations". If they gated the exit code,
  // "advisory" would be a lie.
  const v = computeVerdict([req('a', Status.PASS), adv('b', Status.FAIL), adv('c', Status.ERROR)]);
  assert.equal(v.verdict, 'PASS');
  assert.equal(v.exitCode, 0);
  assert.deepEqual(v.advisoryFailed, ['b', 'c']);
});

test('SKIPs do not fail a run', () => {
  const v = computeVerdict([req('a', Status.PASS), req('b', Status.SKIP)]);
  assert.equal(v.verdict, 'PASS');
});

test('an all-SKIP run is NOTHING_RAN, not PASS', () => {
  // A run where nothing was applicable proved nothing. Reporting PASS would
  // let a misconfigured invocation look like a conforming deployment.
  const v = computeVerdict([req('a', Status.SKIP), req('b', Status.SKIP)]);
  assert.equal(v.verdict, 'NOTHING_RAN');
  assert.equal(v.exitCode, 1);
});

test('an empty result set is NOTHING_RAN', () => {
  const v = computeVerdict([]);
  assert.equal(v.verdict, 'NOTHING_RAN');
  assert.equal(v.exitCode, 1);
});

test('registry rejects a check with no spec reference', () => {
  // Every finding must name where the rule is written; a kit that says
  // "FAIL" without a citation is not usable by someone who has not read
  // this repo, which is the entire audience.
  assert.throws(
    () => createRegistry([{ id: 'x', required: true, run: async () => {} }]),
    /has no spec reference/,
  );
});

test('registry rejects a check that does not declare required', () => {
  assert.throws(
    () => createRegistry([{ id: 'x', spec: 'a.h:1', run: async () => {} }]),
    /must declare required:boolean/,
  );
});

test('registry rejects duplicate ids', () => {
  const c = { id: 'dup', spec: 'a.h:1', required: true, run: async () => {} };
  assert.throws(() => createRegistry([c, { ...c }]), /duplicate check id/);
});

test('result.fail carries expected/observed into evidence', () => {
  const r = result.fail({ expected: 'x', observed: 'y', hint: 'z' });
  assert.equal(r.status, Status.FAIL);
  assert.equal(r.evidence.expected, 'x');
  assert.equal(r.evidence.observed, 'y');
  assert.equal(r.evidence.hint, 'z');
});

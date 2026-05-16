// verification/__tests__/assertion-bridges.test.mjs — R4 RED tests.

import { test, describe } from 'node:test';
import assert from 'node:assert/strict';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { probeSource } from '../lib/image-fs.mjs';
import { assertion, DEFAULT_INSPECTED_PATHS_BRIDGES } from '../assertions/bridges.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = join(HERE, '..', '..');

function ctxFor(over = {}) {
  return {
    mode: 'scaffold',
    sourceRoot: REPO_ROOT,
    imageTag: null,
    timeoutMs: 30_000,
    position: 'M0',
    ...over,
  };
}

describe('R4 bridges-absent assertion', () => {
  // Test 1 — sanity: ground-truth source-tree probe.
  test('1. current source tree contains capture/input-bridge/main.go AND capture/cursor-watcher/main.go', () => {
    const r = probeSource({
      sourceRoot: REPO_ROOT,
      paths: ['capture/input-bridge/main.go', 'capture/cursor-watcher/main.go'],
    });
    assert.deepEqual(r.presentPaths.sort(), [
      'capture/cursor-watcher/main.go',
      'capture/input-bridge/main.go',
    ]);
  });

  // Test 2 — FAIL against current tree.
  test('2. assertion #2 → FAIL against current tree; evidence lists both', async () => {
    const r = await assertion.run(ctxFor());
    assert.equal(r.status, 'FAIL');
    assert.ok(r.evidence.inputBridge.source.presentPaths.length > 0,
      `inputBridge source missing presentPaths: ${JSON.stringify(r.evidence.inputBridge)}`);
    assert.ok(r.evidence.cursorWatcher.source.presentPaths.length > 0,
      `cursorWatcher source missing presentPaths: ${JSON.stringify(r.evidence.cursorWatcher)}`);
  });

  // Test 3 — PASS against synthetic-clean (both removed in source AND image).
  test('3. assertion #2 → PASS against synthetic-clean fixture', async () => {
    const sourceRoot = join(REPO_ROOT, 'verification', 'fixtures', 'synthetic-clean-bridges');
    const imageTag = 'chromeless:synthetic-clean-bridges';
    const r = await assertion.run(ctxFor({ sourceRoot, imageTag }));
    assert.equal(r.status, 'PASS', `evidence=${JSON.stringify(r.evidence)}`);
  });

  // Test 4 — non-default inspected-path list honored (config-overridable).
  test('4. honors non-default inspected-path list (config-overridable)', async () => {
    // Current tree has no capture/decoy-only — assertion should PASS when
    // only that fictional path is inspected (flat-override shape).
    const r = await assertion.run(ctxFor({
      bridgesSourcePaths: ['capture/decoy-only'],
    }));
    assert.equal(r.status, 'PASS', `expected PASS for decoy-only inspection; evidence=${JSON.stringify(r.evidence)}`);
  });

  // Test 5 — DEFAULT_INSPECTED_PATHS_BRIDGES is the documented union per spec.
  test('5. DEFAULT_INSPECTED_PATHS_BRIDGES matches spec (CV2-13 R4)', () => {
    // Spec / R4 description: capture/input-bridge + capture/cursor-watcher
    // are the load-bearing surfaces. The defaults exposed for round-tripping
    // MUST list those directories.
    assert.ok(DEFAULT_INSPECTED_PATHS_BRIDGES.inputBridge.includes('capture/input-bridge'),
      'DEFAULT_INSPECTED_PATHS_BRIDGES.inputBridge missing capture/input-bridge');
    assert.ok(DEFAULT_INSPECTED_PATHS_BRIDGES.cursorWatcher.includes('capture/cursor-watcher'),
      'DEFAULT_INSPECTED_PATHS_BRIDGES.cursorWatcher missing capture/cursor-watcher');
  });

  // Test 6 — evidence distinguishes input-bridge match vs cursor-watcher match.
  test('6. evidence has distinct inputBridge / cursorWatcher fields', async () => {
    const r = await assertion.run(ctxFor());
    assert.ok(r.evidence.inputBridge, 'missing inputBridge in evidence');
    assert.ok(r.evidence.cursorWatcher, 'missing cursorWatcher in evidence');
    // Ground-truth: both are present on the current tree.
    assert.ok(r.evidence.inputBridge.source.presentPaths.length > 0);
    assert.ok(r.evidence.cursorWatcher.source.presentPaths.length > 0);
    // JSON-serializable round-trip.
    const ser = JSON.parse(JSON.stringify(r.evidence));
    assert.deepEqual(ser, r.evidence);
  });
});

// verification/__tests__/assertion-streamer-page.test.mjs — R3 RED tests.
//
// 6 tests against verification/assertions/streamer-page.mjs +
// verification/lib/image-fs.mjs.

import { test, describe } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, mkdtempSync, rmSync, mkdirSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { assertion, probeSource, probeImage, DEFAULT_SOURCE_PATHS, DEFAULT_IMAGE_PATHS } from '../assertions/streamer-page.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = join(HERE, '..', '..');

function ctxFor({ sourceRoot, imageTag }) {
  return {
    mode: 'scaffold',
    sourceRoot,
    imageTag,
    timeoutMs: 30_000,
    position: 'M0',
  };
}

describe('R3 streamer-page-absent assertion', () => {
  // Test 1 — sanity: ground-truth source-tree probe via shared helper.
  // Kills the phantom-fixtures class by anchoring the test to real on-disk state.
  test('1. current source tree contains capture/streamer-page/index.html', () => {
    const r = probeSource({
      sourceRoot: REPO_ROOT,
      paths: ['capture/streamer-page/index.html'],
    });
    assert.deepEqual(r.presentPaths, ['capture/streamer-page/index.html']);
    assert.deepEqual(r.absentPaths, []);
  });

  // Test 2 — assertion against the current source tree returns FAIL.
  test('2. assertion #1 → FAIL against current source tree', async () => {
    const r = await assertion.run(ctxFor({
      sourceRoot: REPO_ROOT,
      imageTag: null, // no image provided — irrelevant; source is enough to FAIL
    }));
    assert.equal(r.status, 'FAIL');
    assert.ok(r.evidence.source.presentPaths.length > 0);
  });

  // Test 3 — PASS against synthetic-clean fixture (source AND image manifest both empty).
  test('3. assertion #1 → PASS against synthetic-clean fixture', async () => {
    const sourceRoot = join(REPO_ROOT, 'verification', 'fixtures', 'synthetic-clean-streamer');
    const imageTag = 'chromeless:synthetic-clean-streamer';
    const r = await assertion.run(ctxFor({ sourceRoot, imageTag }));
    assert.equal(r.status, 'PASS', `expected PASS; evidence=${JSON.stringify(r.evidence)}`);
    assert.deepEqual(r.evidence.source.presentPaths, []);
    assert.deepEqual(r.evidence.image.presentPaths, []);
  });

  // Test 4 — hybrid: source clean BUT image still has streamer → FAIL.
  // Catches the "deleted from tree but image cache still has it" regression.
  test('4. FAIL when source clean but image still has streamer-page', async () => {
    const sourceRoot = join(REPO_ROOT, 'verification', 'fixtures', 'synthetic-clean-streamer');
    const imageTag = 'chromeless:synthetic-hybrid-streamer';
    const r = await assertion.run(ctxFor({ sourceRoot, imageTag }));
    assert.equal(r.status, 'FAIL');
    // Image evidence should list the surviving paths.
    assert.ok(r.evidence.image.presentPaths.length >= 1);
  });

  // Test 5 — image-FS helper duck-type contract for R4 reuse.
  test('5. image-FS helper exports probeSource + probeImage (R4-shared contract)', () => {
    assert.equal(typeof probeSource, 'function');
    assert.equal(typeof probeImage, 'function');
    // Shape: { presentPaths, absentPaths } for source; same + mode for image.
    const s = probeSource({ sourceRoot: REPO_ROOT, paths: ['capture/streamer-page'] });
    assert.ok(Array.isArray(s.presentPaths));
    assert.ok(Array.isArray(s.absentPaths));
    const i = probeImage({ imageRef: 'chromeless:synthetic-clean-streamer', paths: DEFAULT_IMAGE_PATHS });
    assert.ok(Array.isArray(i.presentPaths));
    assert.ok(Array.isArray(i.absentPaths));
    assert.equal(typeof i.mode, 'string');
  });

  // Test 6 — evidence payload is JSON-serializable + lists matched paths.
  test('6. evidence is JSON-serializable; lists exact matched paths', async () => {
    const r = await assertion.run(ctxFor({
      sourceRoot: REPO_ROOT,
      imageTag: null,
    }));
    const ser = JSON.parse(JSON.stringify(r.evidence));
    assert.deepEqual(ser, r.evidence, 'round-trip JSON serialization lossy');
    assert.ok(ser.source.presentPaths.includes('capture/streamer-page'),
      `missing capture/streamer-page in presentPaths: ${JSON.stringify(ser.source.presentPaths)}`);
  });
});

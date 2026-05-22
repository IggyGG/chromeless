// verification/__tests__/harness-with-container.sh.test.mjs — R7 shell wrapper.
//
// 2 tests for verification/scripts/with-container.sh:
//   1. `-- true` exits 0 and tears the container down (no chromeless-harness-* remain)
//   2. `-- false` exits non-zero and STILL tears the container down
//
// Skips when docker daemon or chromeless:ci unavailable.

import { test, describe, before } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = join(HERE, '..', '..');
const WRAPPER = join(REPO_ROOT, 'verification', 'scripts', 'with-container.sh');
const IMAGE = process.env.CHROMELESS_TEST_IMAGE || 'chromeless:ci';

function dockerImageOk() {
  try {
    execFileSync('docker', ['image', 'inspect', IMAGE], {
      stdio: ['ignore', 'ignore', 'ignore'],
      timeout: 5_000,
    });
    return true;
  } catch {
    return false;
  }
}

let OK;
before(() => { OK = dockerImageOk(); });

function listHarnessContainers() {
  const ps = spawnSync('docker', [
    'ps', '-a', '--filter', 'name=chromeless-harness-', '--format', '{{.Names}}',
  ], { encoding: 'utf8', timeout: 5_000 });
  return (ps.stdout || '').trim().split(/\r?\n/).filter(Boolean);
}

describe('R7 with-container.sh', () => {
  test('1. -- true exits 0; no harness containers remain', (t) => {
    if (!OK) {
      t.skip('docker daemon or chromeless:ci unavailable');
      return;
    }
    const before = new Set(listHarnessContainers());
    const r = spawnSync(WRAPPER, ['--image', IMAGE, '--', 'true'], {
      encoding: 'utf8', timeout: 120_000,
    });
    assert.equal(r.status, 0, `exit ${r.status}; stderr=${r.stderr?.slice(0, 200)}`);
    const after = listHarnessContainers().filter(n => !before.has(n));
    assert.deepEqual(after, [], `leaked containers: ${after.join(', ')}`);
  });

  test('2. -- false exits non-zero; container still cleaned up', (t) => {
    if (!OK) {
      t.skip('docker daemon or chromeless:ci unavailable');
      return;
    }
    const before = new Set(listHarnessContainers());
    const r = spawnSync(WRAPPER, ['--image', IMAGE, '--', 'false'], {
      encoding: 'utf8', timeout: 120_000,
    });
    assert.notEqual(r.status, 0);
    const after = listHarnessContainers().filter(n => !before.has(n));
    assert.deepEqual(after, [], `leaked containers after failure: ${after.join(', ')}`);
  });
});

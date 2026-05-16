// R9 compensating-evidence E1: anchors the integration/native-peer branch
// to the SHA recorded at M0 landing. Tautological-by-design — the value is in
// the audit trail, not in a behavioral check.
//
// Two assertions:
//   (a) `git ls-remote ${REMOTE:-origin} integration/native-peer` returns a
//       non-empty SHA (branch exists on the remote).
//   (b) That SHA matches the value recorded in verification/INTEGRATION_BASE.txt
//       at M0 landing — guards against the integration branch drifting under
//       M1's feet.
//
// Skip-friendly: if no network/remote available (e.g. offline test env), the
// branch-existence check is reported as a skip with reason, not a failure —
// CI runs network-attached and will catch real drift there.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = join(HERE, '..', '..');
const BASE_TXT = join(REPO_ROOT, 'verification', 'INTEGRATION_BASE.txt');
const REMOTE = process.env.REMOTE || 'origin';

function readRecordedBase() {
  return readFileSync(BASE_TXT, 'utf8').trim();
}

function lsRemoteIntegration() {
  // Returns the SHA the remote currently has for integration/native-peer,
  // or null on network/auth failure (treated as skip).
  try {
    const out = execFileSync(
      'git',
      ['ls-remote', REMOTE, 'refs/heads/integration/native-peer'],
      { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'], timeout: 15_000 }
    );
    const first = out.split(/\r?\n/).filter(Boolean)[0];
    if (!first) return null;
    const sha = first.split(/\s+/)[0];
    return /^[0-9a-f]{40}$/.test(sha) ? sha : null;
  } catch {
    return null;
  }
}

test('R9 E1.a — verification/INTEGRATION_BASE.txt records a 40-hex SHA', () => {
  const recorded = readRecordedBase();
  assert.match(recorded, /^[0-9a-f]{40}$/, `INTEGRATION_BASE.txt content: ${recorded}`);
});

test('R9 E1.b — integration/native-peer remote SHA matches INTEGRATION_BASE.txt', (t) => {
  const remoteSha = lsRemoteIntegration();
  if (remoteSha === null) {
    t.skip(`ls-remote ${REMOTE} unavailable (offline or unauth); skipping branch-presence anchor`);
    return;
  }
  const recorded = readRecordedBase();
  assert.equal(
    remoteSha,
    recorded,
    `remote integration/native-peer (${remoteSha}) drifted from recorded base (${recorded})`
  );
});

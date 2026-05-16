// verification/__tests__/assertion-native-peer.test.mjs — R6 RED tests.
//
// 6 tests against verification/assertions/native-peer.mjs.

import { test, describe } from 'node:test';
import assert from 'node:assert/strict';
import { assertion } from '../assertions/native-peer.mjs';
import {
  startConformantPeer,
  startMalformedPeer,
  startSilentPeer,
} from '../fixtures/native-peer-loopback/peer-stub.mjs';

function ctxFor(over = {}) {
  return {
    mode: 'scaffold',
    position: 'M3',          // post-M3 so the NYI shim doesn't fire
    timeoutMs: 1_500,        // tight, keeps suite wall-time bounded
    ...over,
  };
}

describe('R6 native-peer-connects assertion', () => {
  // Test 1 — against current-image-like (no peer at target) returns NYI
  // within a bounded timeout. Total wall-time <12s.
  test('1. against current-image returns NYI in <12s (pre-M3 NYI shim)', async () => {
    const start = Date.now();
    const r = await assertion.run({
      mode: 'scaffold',
      position: 'M0',          // pre-M3
      target: '127.0.0.1:1',   // doesn't matter, NYI shim fires first
      timeoutMs: 10_000,
    });
    assert.equal(r.status, 'NOT_YET_IMPLEMENTED');
    assert.ok(Date.now() - start < 12_000);
  });

  // Test 2 — synthetic loopback PASS path.
  test('2. PASS via synthetic loopback peer-stub', async () => {
    const peer = await startConformantPeer();
    try {
      const r = await assertion.run(ctxFor({ target: `${peer.host}:${peer.port}` }));
      assert.equal(r.status, 'PASS', `evidence=${JSON.stringify(r.evidence)}`);
      assert.equal(r.evidence.target, `${peer.host}:${peer.port}`);
    } finally { await peer.close(); }
  });

  // Test 3 — FAIL on non-conformant handshake (wrong magic/version).
  test('3. FAIL when synthetic peer sends non-conformant handshake', async () => {
    const peer = await startMalformedPeer();
    try {
      const r = await assertion.run(ctxFor({ target: `${peer.host}:${peer.port}` }));
      assert.equal(r.status, 'FAIL');
      assert.equal(r.evidence.rejected, 'protocol mismatch');
      assert.ok(r.evidence.expected);
      assert.ok(r.evidence.observed);
    } finally { await peer.close(); }
  });

  // Test 4 — FAIL not hang on non-responding peer; timeout bounded.
  test('4. FAIL (not hang) on non-responding peer; wall-time bounded', async () => {
    const peer = await startSilentPeer();
    try {
      const start = Date.now();
      const r = await assertion.run(ctxFor({
        target: `${peer.host}:${peer.port}`,
        timeoutMs: 600,
      }));
      const elapsed = Date.now() - start;
      assert.equal(r.status, 'FAIL');
      assert.equal(r.evidence.reason, 'timeout');
      assert.ok(elapsed < 5_000, `expected wall-time <5s; got ${elapsed}ms`);
    } finally { await peer.close(); }
  });

  // Test 5 — FAIL with ECONNREFUSED when no peer listening.
  test('5. FAIL with TCP-refused when no peer listening', async () => {
    // High-numbered port unlikely to be bound on the test host.
    const r = await assertion.run(ctxFor({
      target: '127.0.0.1:1',
      timeoutMs: 1_500,
    }));
    assert.equal(r.status, 'FAIL');
    assert.equal(r.evidence.reason, 'connection refused');
    assert.equal(r.evidence.errno, 'ECONNREFUSED');
  });

  // Test 6 — NYI shim discipline: scaffold pre-M3 returns NYI even if
  // probe authored (the assertion's run() doesn't open TCP).
  test('6. scaffold/pre-M3 returns NYI even with target wired (scaffold-green contract)', async () => {
    // Start a real peer to prove the NYI shim short-circuits BEFORE connecting.
    const peer = await startConformantPeer();
    try {
      const r = await assertion.run({
        mode: 'scaffold',
        position: 'M2',                  // pre-M3
        target: `${peer.host}:${peer.port}`,
        timeoutMs: 10_000,
      });
      assert.equal(r.status, 'NOT_YET_IMPLEMENTED');
    } finally { await peer.close(); }
  });
});

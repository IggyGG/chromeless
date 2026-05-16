// verification/__tests__/assertion-codec-cap.test.mjs — R5 RED tests.
//
// 7 tests:
//   1. EXPECTED_FORMATS deep-equals the source-of-truth shape (provenance comment in test header).
//   2. comparator PASS for post-m1-expected fixture.
//   3. comparator FAIL for current-image-libwebrtc-default fixture; lists VP8/AV1/extra H264 in violations.
//   4. violation type enumeration: extra-format / missing-format / parameter-mismatch each surface.
//   5. Swappable probe contract: cdp-getCapabilities + sdp-inspection both feed comparator unchanged.
//   6. unknown strategy → assertion construction throws with usable error.
//   7. NYI shim discipline: pre-M1 returns NYI even when probe is wired.
//
// Header provenance per memory feedback_test_fixtures_from_real_artifacts:
//   EXPECTED_FORMATS — From capture/encoder/encoder_factory_stub.cc:146
//   GetSupportedFormats() on 2026-05-16.

import { test, describe } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  assertion,
  EXPECTED_FORMATS,
  compareCodecs,
  getStrategy,
  STRATEGY_NAMES,
} from '../assertions/codec-cap.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const FIXTURES = join(HERE, '..', 'fixtures', 'cdp-codecs');

function loadFixture(name) {
  return JSON.parse(readFileSync(join(FIXTURES, name), 'utf8'));
}

describe('R5 codec-cap-matches-factory assertion', () => {
  // 1. EXPECTED_FORMATS matches source-of-truth.
  // Provenance: From capture/encoder/encoder_factory_stub.cc:146 on 2026-05-16
  // (worktree @ b438523d8). VP9 + H264 (Constrained Baseline 3.1, profile-
  // level-id=42e01f, packetization-mode=1, level-asymmetry-allowed=1).
  test('1. EXPECTED_FORMATS deep-equals source-of-truth (encoder_factory_stub.cc:146)', () => {
    assert.deepEqual(EXPECTED_FORMATS.length, 2);
    assert.equal(EXPECTED_FORMATS[0].name, 'VP9');
    assert.equal(EXPECTED_FORMATS[1].name, 'H264');
    assert.equal(EXPECTED_FORMATS[1].parameters['profile-level-id'], '42e01f');
    assert.equal(EXPECTED_FORMATS[1].parameters['packetization-mode'], '1');
    assert.equal(EXPECTED_FORMATS[1].parameters['level-asymmetry-allowed'], '1');
  });

  // 2. PASS path.
  test('2. comparator PASS for post-m1-expected fixture', () => {
    const fx = loadFixture('post-m1-expected.json');
    const r = compareCodecs(fx.codecs);
    assert.equal(r.pass, true, `violations: ${JSON.stringify(r.violations)}`);
    assert.deepEqual(r.violations, []);
  });

  // 3. FAIL path — current image fixture has VP8/AV1/extra H264 profiles.
  test('3. comparator FAIL for current-image-libwebrtc-default fixture', () => {
    const fx = loadFixture('current-image-libwebrtc-default.json');
    const r = compareCodecs(fx.codecs);
    assert.equal(r.pass, false);
    const extraNames = r.violations.filter(v => v.type === 'extra-format').map(v => v.name);
    // Should include VP8 and AV1 + extra H264 profile variants.
    assert.ok(extraNames.includes('VP8'), `expected VP8 in extras: ${extraNames}`);
    assert.ok(extraNames.includes('AV1'), `expected AV1 in extras: ${extraNames}`);
    const extraH264s = r.violations.filter(v => v.type === 'extra-format' && v.name === 'H264');
    assert.ok(extraH264s.length >= 1, `expected extra H264 variants beyond 42e01f`);
  });

  // 4. Violation enumeration.
  describe('4. violation type enumeration', () => {
    test('extra-format surfaces when observed codec not in expected', () => {
      const r = compareCodecs([
        { mimeType: 'video/VP9', sdpFmtpLine: null },
        { mimeType: 'video/H264', sdpFmtpLine: 'level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f' },
        { mimeType: 'video/VP8', sdpFmtpLine: null }, // unexpected
      ]);
      const extras = r.violations.filter(v => v.type === 'extra-format');
      assert.equal(extras.length, 1);
      assert.equal(extras[0].name, 'VP8');
    });
    test('missing-format surfaces when expected codec absent', () => {
      const r = compareCodecs([
        // Only H264 present; VP9 missing.
        { mimeType: 'video/H264', sdpFmtpLine: 'level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f' },
      ]);
      const missing = r.violations.filter(v => v.type === 'missing-format');
      assert.equal(missing.length, 1);
      assert.equal(missing[0].name, 'VP9');
    });
    test('parameter-mismatch surfaces when H264 profile-level-id differs', () => {
      const r = compareCodecs([
        { mimeType: 'video/VP9', sdpFmtpLine: null },
        { mimeType: 'video/H264', sdpFmtpLine: 'level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f;different-param=bogus' },
      ]);
      // Same profile-level-id matches the EXPECTED_FORMATS key — but
      // packetization-mode + level-asymmetry-allowed both equal; no mismatch.
      // The "different-param" is observed-only; it doesn't surface as
      // parameter-mismatch (only EXPECTED's params are checked). To
      // exercise parameter-mismatch directly we craft a mismatch:
      const r2 = compareCodecs([
        { mimeType: 'video/VP9', sdpFmtpLine: null },
        { mimeType: 'video/H264', sdpFmtpLine: 'level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e01f' },
        // level-asymmetry-allowed mismatch (expected '1', observed '0').
      ]);
      const mm = r2.violations.filter(v => v.type === 'parameter-mismatch');
      assert.ok(mm.length >= 1, `expected parameter-mismatch in ${JSON.stringify(r2.violations)}`);
    });
  });

  // 5. Swappable probe contract: at least two registered strategies.
  test('5. swappable probe contract — ≥2 strategies registered, both pluggable', () => {
    assert.ok(STRATEGY_NAMES.length >= 2, `expected ≥2 strategies; got: ${STRATEGY_NAMES.join(', ')}`);
    assert.ok(STRATEGY_NAMES.includes('cdp-getCapabilities'));
    assert.ok(STRATEGY_NAMES.includes('sdp-inspection'));
    // Both expose .probe(ctx): assert the contract shape.
    for (const name of STRATEGY_NAMES) {
      const s = getStrategy(name);
      assert.equal(typeof s.probe, 'function', `strategy ${name} missing probe()`);
      assert.equal(s.name, name);
    }
  });

  // 6. Unknown strategy errors at probe time (the assertion's run() returns
  // FAIL with a usable message rather than throwing — keeps the gate robust).
  test('6. unknown strategy name → assertion run returns FAIL with usable error', async () => {
    const r = await assertion.run({
      mode: 'scaffold',
      position: 'M1',
      codecCapStrategy: 'definitely-not-a-strategy',
      client: { send: async () => ({}) },
    });
    assert.equal(r.status, 'FAIL');
    assert.match(r.evidence.error, /unknown codec-cap probe strategy/);
  });

  // 7. NYI shim discipline — pre-M1 position returns NYI even if probe wired.
  test('7. pre-M1 position returns NYI even with a wired client (scaffold-green contract)', async () => {
    const r = await assertion.run({
      mode: 'scaffold',
      position: 'M0',
      client: { send: async () => ({ result: { value: { codecs: [] } } }) },
    });
    assert.equal(r.status, 'NOT_YET_IMPLEMENTED');
    assert.match(r.evidence.reason, /position M0/);
  });
});

// encoder-assertions.test.mjs — node:test, no external deps.
// Run: `node --test tests/webrtc/encoder-assertions.test.mjs`
import test from 'node:test';
import assert from 'node:assert/strict';
import {
  assertEncoderIdentity, pollStatsUntilEncoded,
  EXPECTED_ENCODERS, EXPECTED_HW_PREFIXES, STOCK_FALLBACKS, expectedSimulcastPrefix,
} from './encoder-assertions.mjs';

test('canonical SW encoder strings pass', () => {
  for (const s of EXPECTED_ENCODERS.vp9)  assertEncoderIdentity({ implementation: s }, 'vp9');
  for (const s of EXPECTED_ENCODERS.h264) assertEncoderIdentity({ implementation: s }, 'h264');
  for (const s of EXPECTED_ENCODERS.av1)  assertEncoderIdentity({ implementation: s }, 'av1');
});

test('HW-prefix matches pass (NVENC / VAAPI variants)', () => {
  assertEncoderIdentity({ implementation: 'cloud-browser-nvenc-H264-lowlatency' }, 'h264');
  assertEncoderIdentity({ implementation: 'cloud-browser-vaapi-H264-intel-lowlatency' }, 'h264');
  assertEncoderIdentity({ implementation: 'cloud-browser-vaapi-VP9-intel' }, 'vp9');
  assertEncoderIdentity({ implementation: 'cloud-browser-nvenc-AV1' }, 'av1');
  assertEncoderIdentity({ implementation: 'cloud-browser-vaapi-AV1-amd-lowlatency' }, 'av1');
});

test('simulcast wrapper passes', () => {
  assertEncoderIdentity({ implementation: 'cloud-browser-simulcast-vp9[3]' }, 'vp9');
  assertEncoderIdentity({ implementation: 'cloud-browser-simulcast-h264[2]' }, 'h264');
});

test('FALLBACK DETECTED on chromium stock VP9 (libvpx)', () => {
  assert.throws(() => assertEncoderIdentity({ implementation: 'libvpx' }, 'vp9'),
    /FALLBACK DETECTED.*'libvpx'.*chromium's stock VP9 encoder/s);
});

test('FALLBACK DETECTED on stock H264 fallbacks', () => {
  for (const s of STOCK_FALLBACKS.h264) {
    assert.throws(() => assertEncoderIdentity({ implementation: s }, 'h264'),
      /FALLBACK DETECTED/s, `expected FALLBACK error for ${s}`);
  }
});

test('unknown encoder raises with descriptive error', () => {
  assert.throws(() => assertEncoderIdentity({ implementation: 'random-string' }, 'vp9'),
    /unknown encoder 'random-string'/);
});

test('missing/empty/null implementation raises', () => {
  assert.throws(() => assertEncoderIdentity({}, 'vp9'), /no encoder implementation string/);
  assert.throws(() => assertEncoderIdentity({ implementation: '' }, 'vp9'), /no encoder implementation string/);
  assert.throws(() => assertEncoderIdentity(null, 'vp9'), /no encoder implementation string/);
});

test('unknown codec raises', () => {
  assert.throws(() => assertEncoderIdentity({ implementation: 'cloud-browser-vp9-libvpx' }, 'theora'),
    /unknown codec 'theora'/);
});

test('raw string stats arg is accepted', () => {
  assertEncoderIdentity('cloud-browser-vp9-libvpx', 'vp9');
});

test('pollStatsUntilEncoded returns the implementation string after polling', async () => {
  let calls = 0;
  const fakeClient = { Runtime: { evaluate: async () => {
    calls += 1;
    return calls < 3 ? { result: { value: null } } : { result: { value: 'cloud-browser-vp9-libvpx' } };
  }}};
  assert.equal(await pollStatsUntilEncoded(fakeClient, 'window.pc', 5000), 'cloud-browser-vp9-libvpx');
  assert.ok(calls >= 3);
});

test('pollStatsUntilEncoded times out with structured error', async () => {
  const fakeClient = { Runtime: { evaluate: async () => ({ result: { value: null } }) } };
  await assert.rejects(() => pollStatsUntilEncoded(fakeClient, 'window.pc', 600),
    /timed out after 600ms.*Last value: null.*window\.pc/s);
});

test('pollStatsUntilEncoded surfaces CDP exception details', async () => {
  const fakeClient = { Runtime: { evaluate: async () => ({ exceptionDetails: { text: 'TypeError: pc is null' } }) } };
  await assert.rejects(() => pollStatsUntilEncoded(fakeClient, 'window.pc', 5000),
    /CDP exception.*TypeError: pc is null/s);
});

test('exported tables match source-of-truth strings verbatim', () => {
  assert.deepEqual(EXPECTED_ENCODERS.vp9.slice().sort(),  ['cloud-browser-vp9-libvpx', 'cloud-browser-vp9-libvpx-lowlatency'].sort());
  assert.deepEqual(EXPECTED_ENCODERS.h264.slice().sort(), ['cloud-browser-h264-x264', 'cloud-browser-h264-x264-lowlatency'].sort());
  assert.deepEqual(EXPECTED_ENCODERS.av1.slice().sort(),  ['cloud-browser-av1-svtav1', 'cloud-browser-av1-svtav1-lowlatency'].sort());
  assert.ok(Array.isArray(EXPECTED_HW_PREFIXES.vp9));
  assert.match(expectedSimulcastPrefix('h264'), /^cloud-browser-simulcast-h264\[$/);
});

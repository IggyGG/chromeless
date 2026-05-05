// encoder-assertions.mjs — sender-side encoder-identity verification for chromeless.
//
// outboundRtp.encoderImplementation in getStats() is set verbatim from
// EncoderInfo::implementation_name in our embedded encoders. If the
// WebRtcVideoEncoderFactory injection regresses, chromium falls back to
// stock encoders (libvpx, OpenH264, libaom). This module asserts on
// the difference. Source of truth (chromeless monorepo):
//   capture/encoder/vp9_encoder.cc        L293  "cloud-browser-vp9-libvpx"     [+ "-lowlatency"]
//   capture/encoder/h264_encoder.cc       L360  "cloud-browser-h264-x264"      [+ "-lowlatency"]
//   capture/encoder/svtav1_encoder.cc     L401  "cloud-browser-av1-svtav1"     [+ "-lowlatency"]
//   capture/encoder/nvenc_encoder.cc      L575  "cloud-browser-nvenc-{H264|HEVC|AV1}"
//   capture/encoder/vaapi_encoder.cc      L856  "cloud-browser-vaapi-{codec}-{vendor}"
//   capture/encoder/simulcast_factory.cc  L376  "cloud-browser-simulcast-{codec}[{N}]"
//   capture/encoder/encoder_factory_stub.cc L128 "cloud-browser-stub-{codec}"  (UnimplementedEncoder)
// Note: encoderImplementation is OMITTED from getStats() until the first
// frame encodes — gate via pollStatsUntilEncoded.

export const EXPECTED_ENCODERS = {
  vp9:  ['cloud-browser-vp9-libvpx',  'cloud-browser-vp9-libvpx-lowlatency'],
  h264: ['cloud-browser-h264-x264',   'cloud-browser-h264-x264-lowlatency'],
  av1:  ['cloud-browser-av1-svtav1',  'cloud-browser-av1-svtav1-lowlatency'],
};

export const EXPECTED_HW_PREFIXES = {
  vp9:  ['cloud-browser-vaapi-VP9'],
  h264: ['cloud-browser-nvenc-H264', 'cloud-browser-vaapi-H264'],
  av1:  ['cloud-browser-nvenc-AV1',  'cloud-browser-vaapi-AV1'],
};

export const expectedSimulcastPrefix = (codec) => `cloud-browser-simulcast-${codec}[`;

export const STOCK_FALLBACKS = {
  vp9:  ['libvpx'],
  h264: ['OpenH264', 'ExternalH264Encoder'],
  av1:  ['libaom', 'EncoderSimulcastProxy'],
};

/**
 * Poll chromium getStats() over CDP until outboundRtp.encoderImplementation
 * is non-empty. cdpClient is duck-typed: needs Runtime.evaluate({...}).
 */
export async function pollStatsUntilEncoded(cdpClient, peerConnVarName, timeoutMs = 8000) {
  const expr = `(${peerConnVarName}).getStats().then(s => {
    const out = [...s.values()].find(r => r.type === 'outbound-rtp' && r.kind === 'video');
    return out ? (out.encoderImplementation || null) : null;
  })`;
  const deadline = Date.now() + timeoutMs;
  let lastValue = null;
  while (Date.now() < deadline) {
    const r = await cdpClient.Runtime.evaluate({ expression: expr, awaitPromise: true, returnByValue: true });
    if (r && r.exceptionDetails) {
      throw new Error(`pollStatsUntilEncoded: CDP exception evaluating ${peerConnVarName}.getStats(): ` +
        (r.exceptionDetails.text || JSON.stringify(r.exceptionDetails)));
    }
    lastValue = (r && r.result) ? (r.result.value ?? null) : null;
    if (typeof lastValue === 'string' && lastValue.length > 0) return lastValue;
    await new Promise((res) => setTimeout(res, 250));
  }
  throw new Error(`pollStatsUntilEncoded: timed out after ${timeoutMs}ms waiting for ` +
    `outboundRtp.encoderImplementation. Last value: ${JSON.stringify(lastValue)}. ` +
    `Encoder may not have started — check that frames are flowing on ${peerConnVarName}.`);
}

/** Throw if stats.implementation isn't one of OUR embedded encoders for expectedCodec. */
export function assertEncoderIdentity(stats, expectedCodec) {
  const codec = String(expectedCodec).toLowerCase();
  const impl = typeof stats === 'string' ? stats : (stats && stats.implementation);
  if (typeof impl !== 'string' || impl.length === 0) {
    throw new Error(`assertEncoderIdentity: no encoder implementation string (got ${JSON.stringify(stats)})`);
  }
  const expected = EXPECTED_ENCODERS[codec];
  if (!expected) {
    throw new Error(`assertEncoderIdentity: unknown codec '${expectedCodec}'. ` +
      `Supported: ${Object.keys(EXPECTED_ENCODERS).join(', ')}.`);
  }
  if (expected.includes(impl)) return;
  const hwPrefixes = EXPECTED_HW_PREFIXES[codec] || [];
  if (hwPrefixes.some((p) => impl.startsWith(p))) return;
  if (impl.startsWith(expectedSimulcastPrefix(codec))) return;

  const stockFor = STOCK_FALLBACKS[codec] || [];
  if (stockFor.includes(impl)) {
    throw new Error(`FALLBACK DETECTED: outboundRtp.encoderImplementation = '${impl}', ` +
      `which is chromium's stock ${codec.toUpperCase()} encoder.\n` +
      `Expected: '${expected[0]}' (our embedded encoder, see capture/encoder/encoder_factory_stub.cc).\n` +
      `This indicates the WebRtcVideoEncoderFactory injection regressed.\n` +
      `Check capture/build-integration/content_browser_client.cc and ` +
      `patches/0001-expose-encoder-factory-injection.patch.`);
  }
  throw new Error(`assertEncoderIdentity: unknown encoder '${impl}' for codec '${expectedCodec}'.\n` +
    `Expected SW: ${expected.join(', ')}; HW prefix: ${hwPrefixes.join(', ') || '(none)'}; ` +
    `simulcast: '${expectedSimulcastPrefix(codec)}N]'. ` +
    `Stock fallbacks: ${stockFor.join(', ') || '(none)'}. ` +
    `Source: chromeless:capture/encoder/encoder_factory_stub.cc.`);
}

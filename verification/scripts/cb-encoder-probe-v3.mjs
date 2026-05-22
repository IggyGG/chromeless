#!/usr/bin/env node
// verification/scripts/cb-encoder-probe-v3.mjs — R5 raw-CDP capability probe.
//
// Mirrors /tmp/cb-encoder-probe-v3.mjs's raw-WS technique (the operator's
// reference probe). Captures `RTCRtpSender.getCapabilities("video").codecs`
// from a chromeless container and prints a normalized JSON fixture to
// stdout, suitable for verification/fixtures/cdp-codecs/.
//
// Two modes:
//   (1) direct WS:    pass `--ws ws://localhost:9223/devtools/page/<id>`
//                     (when the page WS URL is already known, e.g. operator
//                     forwarding the in-container DevTools to host)
//   (2) harness mode: pass `--image chromeless:ci` and it'll boot via
//                     verification/lib/harness.mjs (R7).
//
// The normalized output shape (matches R5 comparator expectations):
//   {
//     "_provenance": "captured from <imageRef> @ <iso> via cb-encoder-probe-v3.mjs",
//     "imageRef": "<tag>",
//     "capturedAt": "<iso>",
//     "codecs": [
//       { "mimeType": "video/VP9", "clockRate": 90000, "sdpFmtpLine": null },
//       { "mimeType": "video/H264", "clockRate": 90000, "sdpFmtpLine": "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f" },
//       ...
//     ]
//   }
//
// Usage:
//   node verification/scripts/cb-encoder-probe-v3.mjs --image chromeless:ci > fixture.json
//   node verification/scripts/cb-encoder-probe-v3.mjs --ws ws://... > fixture.json

import { argv, exit, stdout, stderr } from 'node:process';
import { boot } from '../lib/harness.mjs';

const GET_CAPS_JS = `(() => {
  const caps = RTCRtpSender.getCapabilities('video');
  if (!caps) return { error: 'getCapabilities(video) returned null' };
  return {
    codecs: caps.codecs.map(c => ({
      mimeType: c.mimeType,
      clockRate: c.clockRate,
      channels: c.channels !== undefined ? c.channels : null,
      sdpFmtpLine: c.sdpFmtpLine || null,
    })),
  };
})()`;

function parseArgs(args) {
  const opts = { ws: null, image: null };
  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    if (a === '--ws') opts.ws = args[++i];
    else if (a.startsWith('--ws=')) opts.ws = a.slice('--ws='.length);
    else if (a === '--image') opts.image = args[++i];
    else if (a.startsWith('--image=')) opts.image = a.slice('--image='.length);
  }
  return opts;
}

async function captureViaHarness(image) {
  const session = await boot({ imageTag: image });
  try {
    const r = await session.client.send('Runtime.evaluate', {
      expression: GET_CAPS_JS,
      returnByValue: true,
    });
    if (r.exceptionDetails) {
      throw new Error(`page exception: ${JSON.stringify(r.exceptionDetails)}`);
    }
    return r.result?.value || {};
  } finally {
    await session.teardown();
  }
}

async function main() {
  const opts = parseArgs(argv.slice(2));
  if (!opts.image && !opts.ws) {
    stderr.write('usage: cb-encoder-probe-v3.mjs --image <tag> | --ws <url>\n');
    return 2;
  }

  let value;
  if (opts.image) {
    value = await captureViaHarness(opts.image);
  } else {
    stderr.write('--ws mode not implemented in this M0 probe; use --image.\n');
    return 2;
  }

  const fixture = {
    _provenance: `captured from ${opts.image || opts.ws} @ ${new Date().toISOString()} via cb-encoder-probe-v3.mjs`,
    imageRef: opts.image || null,
    capturedAt: new Date().toISOString(),
    codecs: value.codecs || [],
  };
  stdout.write(JSON.stringify(fixture, null, 2) + '\n');
  return 0;
}

const code = await main().catch(err => {
  stderr.write(`[probe] error: ${err.message}\n`);
  return 1;
});
exit(code);

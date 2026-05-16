// verification/assertions/codec-cap.mjs — R5 assertion #3.
//
// Probe: RTCRtpSender.getCapabilities("video").codecs vs the constant
// EXPECTED_FORMATS derived from capture/encoder/encoder_factory_stub.cc:146.
//
// Swappable probe strategy contract (R5 acceptance criterion b):
//
//   ctx.codecCapStrategy = 'cdp-getCapabilities' (default) | 'sdp-inspection'
//
// Two impls both feed the SAME comparator unchanged — when M1 needs to
// pivot the technique (false-green resolution per CV2-14 audit), it's
// a one-line config flip + a new strategy module. The harness/verdict
// layer never moves.
//
// NYI shim discipline (CV2-14 test #7): when the gate position is BEFORE
// M1 (the enabling-module marker for #3), this assertion returns
// NOT_YET_IMPLEMENTED regardless of whether the probe is wired. Preserves
// the "current image → scaffold 0" contract test-enforced, not just
// document-enforced.

import { result } from '../lib/registry.mjs';
import { DEFAULT_SCHEDULE } from '../lib/verdict.mjs';

// ---- EXPECTED_FORMATS — single source of truth ------------------------
//
// From capture/encoder/encoder_factory_stub.cc:146 GetSupportedFormats() on 2026-05-16
// (file inspected in worktree at b438523d8). The factory advertises VP9 and H264
// (Constrained Baseline 3.1, profile-level-id=42e01f, packetization-mode=1,
// level-asymmetry-allowed=1). VP8 / AV1 are absent in Phase 1 — VP8 only when
// explicitly enabled, AV1 only when an NVENC path is available.
export const EXPECTED_FORMATS = Object.freeze([
  Object.freeze({
    name: 'VP9',
  }),
  Object.freeze({
    name: 'H264',
    parameters: Object.freeze({
      'profile-level-id': '42e01f',
      'packetization-mode': '1',
      'level-asymmetry-allowed': '1',
    }),
  }),
]);

// Mime-type → encoder name (codec.name) extraction. The CDP getCapabilities
// shape exposes `mimeType: 'video/VP9'` etc.; encoder_factory_stub uses bare
// 'VP9'/'H264' strings.
function mimeToName(mime) {
  if (!mime || typeof mime !== 'string') return null;
  const i = mime.indexOf('/');
  return i >= 0 ? mime.slice(i + 1).toUpperCase() : mime.toUpperCase();
}

// sdpFmtpLine 'level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f'
// → { 'level-asymmetry-allowed': '1', ... }
function parseSdpFmtp(line) {
  if (!line) return {};
  const out = {};
  for (const part of line.split(';')) {
    const eq = part.indexOf('=');
    if (eq < 0) continue;
    out[part.slice(0, eq).trim().toLowerCase()] = part.slice(eq + 1).trim();
  }
  return out;
}

// Normalize a CDP codec entry to the EXPECTED_FORMATS shape ({name, parameters?}).
function normalizeCdpCodec(c) {
  const name = mimeToName(c.mimeType);
  const params = parseSdpFmtp(c.sdpFmtpLine);
  return Object.keys(params).length ? { name, parameters: params } : { name };
}

// ---- comparator ------------------------------------------------------
//
// Compare an observed normalized format list against EXPECTED_FORMATS.
// Three violation types reported in evidence.violations[]:
//   { type: 'extra-format', name, parameters? }       observed but unexpected
//   { type: 'missing-format', name, parameters? }     expected but absent
//   { type: 'parameter-mismatch', name, expected, observed }
//
// PASS when violations.length === 0.
export function compareCodecs(observedRaw, expected = EXPECTED_FORMATS) {
  const observed = observedRaw.map(normalizeCdpCodec);
  const violations = [];

  // Group observed by name+(profile-level-id when H264).
  const obsByKey = new Map();
  for (const o of observed) {
    const key = o.name + (o.parameters?.['profile-level-id'] ? `|${o.parameters['profile-level-id']}` : '');
    if (!obsByKey.has(key)) obsByKey.set(key, []);
    obsByKey.get(key).push(o);
  }
  const expByKey = new Map();
  for (const e of expected) {
    const key = e.name + (e.parameters?.['profile-level-id'] ? `|${e.parameters['profile-level-id']}` : '');
    if (!expByKey.has(key)) expByKey.set(key, []);
    expByKey.get(key).push(e);
  }

  for (const [key, expEntries] of expByKey) {
    if (!obsByKey.has(key)) {
      for (const e of expEntries) violations.push({ type: 'missing-format', name: e.name, parameters: e.parameters });
      continue;
    }
    // Check parameter equality for the matched key.
    const expEntry = expEntries[0];
    const obsEntry = obsByKey.get(key)[0];
    if (expEntry.parameters) {
      for (const [k, v] of Object.entries(expEntry.parameters)) {
        const obsV = obsEntry.parameters?.[k];
        if (obsV !== v) {
          violations.push({
            type: 'parameter-mismatch',
            name: expEntry.name,
            expected: { [k]: v },
            observed: { [k]: obsV ?? null },
          });
        }
      }
    }
  }
  for (const [key, obsEntries] of obsByKey) {
    if (!expByKey.has(key)) {
      for (const o of obsEntries) violations.push({ type: 'extra-format', name: o.name, parameters: o.parameters });
    }
  }
  return {
    pass: violations.length === 0,
    violations,
    observedNormalized: observed,
    expected,
  };
}

// ---- swappable probe strategies --------------------------------------

const STRATEGIES = new Map();

const cdpGetCapabilitiesStrategy = {
  name: 'cdp-getCapabilities',
  async probe(ctx) {
    // The container-boot harness exposes ctx.client (R7). When unavailable
    // (no docker, no probe attempted), throw — comparator-side tests do not
    // call probe; they call compareCodecs directly with a fixture.
    if (!ctx.client) throw new Error('cdp-getCapabilities strategy requires ctx.client (R7 harness)');
    const r = await ctx.client.send('Runtime.evaluate', {
      expression: `(() => {
        const caps = RTCRtpSender.getCapabilities('video');
        if (!caps) return { error: 'getCapabilities(video) returned null' };
        return { codecs: caps.codecs.map(c => ({
          mimeType: c.mimeType,
          clockRate: c.clockRate,
          channels: c.channels !== undefined ? c.channels : null,
          sdpFmtpLine: c.sdpFmtpLine || null,
        })) };
      })()`,
      returnByValue: true,
    });
    return r.result?.value?.codecs || [];
  },
};
STRATEGIES.set(cdpGetCapabilitiesStrategy.name, cdpGetCapabilitiesStrategy);

// Stub second strategy — proves the swappability contract (test #5).
// M1 fills this in if/when SDP inspection is needed to resolve a false-green.
const sdpInspectionStrategy = {
  name: 'sdp-inspection',
  async probe(_ctx) {
    // Not implemented in M0. Returns an empty list; comparator FAILs cleanly.
    // M1 swap-in will populate with addTransceiver+createOffer+SDP parse.
    return [];
  },
};
STRATEGIES.set(sdpInspectionStrategy.name, sdpInspectionStrategy);

export function getStrategy(name) {
  if (!STRATEGIES.has(name)) {
    throw new Error(`unknown codec-cap probe strategy: ${name} (known: ${[...STRATEGIES.keys()].join(', ')})`);
  }
  return STRATEGIES.get(name);
}

export const STRATEGY_NAMES = Object.freeze([...STRATEGIES.keys()]);

// ---- assertion -------------------------------------------------------

function modulePositionIndex(pos, schedule = DEFAULT_SCHEDULE) {
  const i = schedule.modules.indexOf(pos);
  return i < 0 ? 0 : i;
}

export const assertion = {
  id: 'codec-cap-matches-factory',
  number: 3,
  title: 'codec capability matches encoder-factory',
  async run(ctx) {
    // NYI shim discipline: pre-M1 (where the encoder factory hasn't landed),
    // return NYI regardless of probe state. This preserves the
    // current-image → scaffold-PASS contract (test #7).
    const schedule = ctx.schedule || DEFAULT_SCHEDULE;
    const position = ctx.position || 'M0';
    const enablingMarker = schedule.assertions['codec-cap-matches-factory']?.enablingModule || 'M1';
    if (modulePositionIndex(position, schedule) < modulePositionIndex(enablingMarker, schedule)) {
      return result.notYetImplemented({
        reason: `position ${position} < enabling ${enablingMarker}; probe shimmed`,
      });
    }

    const strategyName = ctx.codecCapStrategy || 'cdp-getCapabilities';
    let strategy;
    try { strategy = getStrategy(strategyName); }
    catch (err) { return result.fail({ strategy: strategyName, error: err.message }); }

    let observedCodecs;
    try {
      observedCodecs = await strategy.probe(ctx);
    } catch (err) {
      return result.fail({ strategy: strategyName, probeError: err.message });
    }

    const cmp = compareCodecs(observedCodecs);
    return (cmp.pass ? result.pass : result.fail)({
      strategy: strategyName,
      violations: cmp.violations,
      observedNormalized: cmp.observedNormalized,
      expected: cmp.expected,
    });
  },
};

// conformance/suites/cdp-surface.mjs — is the DevTools surface the one this
// project documents?
//
// SPEC anchors:
//   CLAUDE.md "CDP clients must pass `local: true`" — GET /json/protocol
//     CHECK-FATALs the worker, so this suite NEVER requests that path. The
//     check that would be most natural to write here is the one that
//     crashes the thing under test.
//   capture/build-integration/cb_devtools_agent.cc — the Cb.* hand-dispatch.
//
// Everything here is plain HTTP against the DevTools endpoint. No CDP
// WebSocket session is opened: the suite is read-only by construction, and
// a conformance run must not perturb a deployment someone is using.

import { result } from '../lib/check.mjs';

const CLAUDE = 'CLAUDE.md (Traps that have cost real time)';

async function getJson(base, path, timeoutMs) {
  const url = `${String(base).replace(/\/+$/, '')}${path}`;
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), timeoutMs);
  try {
    const res = await fetch(url, { signal: ctl.signal });
    const text = await res.text();
    let json = null;
    try { json = JSON.parse(text); } catch { /* reported by the caller */ }
    return { ok: res.ok, status: res.status, json, text, url };
  } finally {
    clearTimeout(timer);
  }
}

const devtoolsReachable = {
  id: 'devtools-reachable',
  suite: 'cdp-surface',
  title: '/json/version answers and identifies a browser',
  spec: 'tests/smoke/container-boot.sh step 4',
  required: true,
  async run(ctx) {
    let r;
    try {
      r = await getJson(ctx.target, '/json/version', ctx.timeoutMs);
    } catch (err) {
      return result.fail({
        expected: 'HTTP 200 with a JSON body',
        observed: `request failed: ${err.message}`,
        hint: 'DevTools binds 127.0.0.1 ONLY as of Chromium 147 — --remote-debugging-address=0.0.0.0 is ignored. From outside the container you need a port-forward or to run inside its network namespace; tests/smoke/container-boot.sh uses `docker exec` for exactly this reason.',
      });
    }
    if (!r.ok) {
      return result.fail({ expected: 'HTTP 200', observed: `HTTP ${r.status}`, hint: `GET ${r.url}` });
    }
    if (!r.json || typeof r.json !== 'object') {
      return result.fail({
        expected: 'a JSON object',
        observed: `unparseable body: ${r.text.slice(0, 120)}`,
      });
    }
    if (!('Browser' in r.json)) {
      return result.fail({
        expected: 'a `Browser` key',
        observed: `keys: ${Object.keys(r.json).join(', ')}`,
        hint: 'this is what the smoke test asserts; a response without it is not a DevTools endpoint',
      });
    }
    return result.pass({
      browser: r.json.Browser,
      protocolVersion: r.json['Protocol-Version'],
      v8: r.json['V8-Version'],
    });
  },
};

const pageTargetExists = {
  id: 'page-target-exists',
  suite: 'cdp-surface',
  title: '/json lists at least one page target with a debugger URL',
  spec: 'tests/smoke/container-boot.sh step 5',
  required: true,
  async run(ctx) {
    let r;
    try {
      r = await getJson(ctx.target, '/json', ctx.timeoutMs);
    } catch (err) {
      return result.fail({ expected: 'HTTP 200 with a JSON array', observed: `request failed: ${err.message}` });
    }
    if (!Array.isArray(r.json)) {
      return result.fail({
        expected: 'a JSON array of targets',
        observed: `got ${r.json === null ? 'unparseable body' : typeof r.json}`,
      });
    }
    const pages = r.json.filter(t => t?.type === 'page' && t?.webSocketDebuggerUrl);
    if (pages.length === 0) {
      return result.fail({
        expected: 'at least one target with type=page and a webSocketDebuggerUrl',
        observed: `${r.json.length} target(s), none usable: ${r.json.map(t => t?.type).join(', ') || '(none)'}`,
        hint: 'the browser is up but has no page — check that the launcher opened one (launch-chromeless.sh) rather than starting headless with no tab',
      });
    }
    return result.pass({ pageTargets: pages.length, firstUrl: pages[0].url });
  },
};

// The trap this suite exists to encode. Requesting /json/protocol is what a
// naive conformance kit would do to enumerate Cb.*; on this worker it
// CHECK-FATALs the browser process. So the check is that the endpoint stays
// AVOIDED — we assert the documented hazard rather than walking into it.
const protocolEndpointHazard = {
  id: 'protocol-endpoint-hazard',
  suite: 'cdp-surface',
  title: 'GET /json/protocol is documented as fatal and is not probed',
  spec: CLAUDE,
  required: false,
  async run() {
    return result.skip(
      'not probed on purpose: GET /json/protocol CHECK-FATALs the worker (SIGABRT). ' +
      'Nine test files in this repo carry `local: true` on their chrome-remote-interface ' +
      'connect for the same reason. If your CDP client crashes the browser on connect, ' +
      'this is why — pass local:true so it skips protocol discovery.',
      { endpoint: '/json/protocol', action: 'deliberately-not-requested' },
    );
  },
};

export const checks = [devtoolsReachable, pageTargetExists, protocolEndpointHazard];

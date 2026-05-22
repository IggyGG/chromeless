// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// harness/m4-uat/lib/cdp.mjs — minimal CDP client for the UAT harness.
//
// Why a hand-rolled client and not chrome-remote-interface? Two reasons:
//
//   1. The M0 latency harness already uses a hand-rolled CDP client
//      (harness/latency/server-sink.py) so adding a node CDP dep would
//      drift the project. The dependency surface here stays at
//      whatever ws + node:net already provide.
//
//   2. The harness only needs four CDP methods:
//      - GET /json/version       (discover ws endpoint)
//      - Target.attachToTarget   (attach to the captured WebContents)
//      - Page.navigate           (load the fixture page)
//      - Runtime.evaluate        (read document.lastInputEvent)
//
// DRAFT — the WebSocket actually connecting and the per-method
// promise chains are stubbed below. The surface that uat.mjs and the
// scenarios depend on is locked.

import { WebSocket } from 'node:ws';   // TODO(M4-R9-ws-dep): formally
// add `ws` to a top-level package.json once we know what runtime ships
// inside chromeless:ci. Today the latency harnesses run from outside
// the container so the dep is host-side.

let nextId = 1;

export async function openCdp(httpEndpoint, timeoutMs = 30_000) {
  // 1. GET <httpEndpoint>/json/version — extract webSocketDebuggerUrl
  // 2. Open WebSocket to that URL
  // 3. Return wrapper with .send(method, params) → Promise<result>,
  //    .close()
  //
  // TODO(M4-R9-cdp-impl): wire the actual fetch + WebSocket lifecycle.
  // For DRAFT, return a stub so scenarios can be unit-tested for shape.
  return {
    endpoint: httpEndpoint,
    send: async (method, params) => {
      throw new Error(`cdp not implemented (called ${method})`);
    },
    close: async () => {},
  };
}

export async function navigatePage(cdp, url) {
  return cdp.send('Page.navigate', { url });
}

// Reads document.lastInputEvent from the captured WebContents.
// Returns the JSON-stringified-and-reparsed value (so the harness
// gets a plain object).
export async function readLastInputEvent(cdp) {
  const r = await cdp.send('Runtime.evaluate', {
    expression: 'JSON.stringify(document.lastInputEvent || null)',
    returnByValue: true,
  });
  // r.result.value is a JSON string OR null literal (returnByValue).
  if (!r || !r.result || r.result.value == null) return null;
  try { return JSON.parse(r.result.value); }
  catch { return { _raw: r.result.value }; }
}

// Generic script evaluator — scenarios use it for per-scenario probes
// that go beyond document.lastInputEvent (e.g. reading clipboard,
// querying active touches, asserting compositionend committed text).
export async function evaluateScript(cdp, expression) {
  const r = await cdp.send('Runtime.evaluate', {
    expression: `JSON.stringify((function(){ try { return (${expression}); } catch(e) { return { _error: String(e) }; } })())`,
    returnByValue: true,
  });
  if (!r || !r.result) return null;
  try { return JSON.parse(r.result.value); }
  catch { return { _raw: r.result.value }; }
}

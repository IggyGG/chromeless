// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// scenarios/mouse.mjs — M4 R3 surface (mouse + wheel).
//
// Sends representative `mouse_move`, `mouse_button` (down/up), and
// `mouse_wheel` envelopes; reads document.lastInputEvent after each
// and asserts the DOM saw the corresponding event with the expected
// fields.

import { env } from '../lib/envelope.mjs';
import { result, Status } from '../lib/registry.mjs';

const TARGET_X = 320;
const TARGET_Y = 240;

async function flush(ctx) {
  // RAF + small settling delay; the dispatcher's UI-thread PostTask
  // is not synchronous with the DataChannel receive. 50ms is the
  // round number that's worked for the input-bridge harness and
  // we keep the same budget here. TODO(M4-R9-flush-budget): if
  // flake shows up, replace with a `document.lastInputEvent` poll
  // with explicit await.
  await new Promise(r => setTimeout(r, 50));
  await ctx.evaluateScript(ctx.cdp, 'document.body.offsetHeight');
}

function shapeMatches(observed, expected) {
  // Compare a subset of fields — observed may carry timestamp /
  // target metadata the harness doesn't care about. Only the keys
  // present in `expected` are required to match.
  if (!observed) return false;
  for (const k of Object.keys(expected)) {
    if (observed[k] !== expected[k]) return false;
  }
  return true;
}

export const scenario = {
  id: 'mouse',
  r9Surface: 'M4 R3 (mouse + wheel)',
  async run(ctx) {
    if (!ctx.dc) {
      return result.skipped('datachannel-not-open', {
        dcOpenError: ctx.dcOpenError ? ctx.dcOpenError.message : null,
      });
    }

    const sent = [];
    const observed = [];

    // 1. mouse_move
    const move = env.mouseMove({ x: TARGET_X, y: TARGET_Y });
    sent.push(move);
    await ctx.dc.send(move);
    await flush(ctx);
    let ev = await ctx.readLastInputEvent(ctx.cdp);
    observed.push(ev);
    if (!shapeMatches(ev, { type: 'mousemove', clientX: TARGET_X, clientY: TARGET_Y })) {
      return result.fail({ stage: 'mouse_move', sent, observed });
    }

    // 2. mouse_button down + up (left-click)
    for (const action of ['down', 'up']) {
      const btn = env.mouseButton({ button: 0, action, x: TARGET_X, y: TARGET_Y });
      sent.push(btn);
      await ctx.dc.send(btn);
      await flush(ctx);
      ev = await ctx.readLastInputEvent(ctx.cdp);
      observed.push(ev);
      const expectedType = action === 'down' ? 'mousedown' : 'mouseup';
      if (!shapeMatches(ev, { type: expectedType, button: 0,
                              clientX: TARGET_X, clientY: TARGET_Y })) {
        return result.fail({ stage: `mouse_button-${action}`, sent, observed });
      }
    }

    // 3. mouse_wheel — scroll down 120 (one tick, in pixel mode).
    const wheel = env.mouseWheel({ dx: 0, dy: -120, mode: 0,
                                   delta_mode: 'pixel', phase: 'start',
                                   momentum: false, x: TARGET_X, y: TARGET_Y });
    sent.push(wheel);
    await ctx.dc.send(wheel);
    await flush(ctx);
    ev = await ctx.readLastInputEvent(ctx.cdp);
    observed.push(ev);
    if (!shapeMatches(ev, { type: 'wheel', deltaY: -120, deltaMode: 0 })) {
      return result.fail({ stage: 'mouse_wheel', sent, observed });
    }

    return result.pass({ sent, observed });
  },
};

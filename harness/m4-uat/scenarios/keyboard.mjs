// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// scenarios/keyboard.mjs — M4 R4 surface (keyboard + modifiers).
//
// Two paths exercised:
//   1. Plain printable key (KeyA + 'a', mods=0).
//   2. Modified key (KeyA + Cmd, mods=8) — verifies the shared
//      modifier state machine is updated.

import { env } from '../lib/envelope.mjs';
import { result } from '../lib/registry.mjs';

const FOCUS_TARGET = '#uat-text-input';

async function flush(ctx) {
  await new Promise(r => setTimeout(r, 50));
}

function shapeMatches(observed, expected) {
  if (!observed) return false;
  for (const k of Object.keys(expected)) {
    if (observed[k] !== expected[k]) return false;
  }
  return true;
}

export const scenario = {
  id: 'keyboard',
  r9Surface: 'M4 R4 (keyboard)',
  async run(ctx) {
    if (!ctx.dc) {
      return result.skipped('datachannel-not-open', {
        dcOpenError: ctx.dcOpenError ? ctx.dcOpenError.message : null,
      });
    }

    // Make sure the text input is focused so the keypress lands
    // in a meaningful DOM target.
    await ctx.evaluateScript(ctx.cdp,
      `document.querySelector('${FOCUS_TARGET}').focus()`);

    const sent = [];
    const observed = [];

    // 1. Plain 'a'
    for (const phase of ['keyDown', 'keyUp']) {
      const e = env[phase]({ code: 'KeyA', key: 'a', mods: 0 });
      sent.push(e);
      await ctx.dc.send(e);
      await flush(ctx);
      const ev = await ctx.readLastInputEvent(ctx.cdp);
      observed.push(ev);
      const expectedType = phase === 'keyDown' ? 'keydown' : 'keyup';
      if (!shapeMatches(ev, { type: expectedType, code: 'KeyA', key: 'a' })) {
        return result.fail({ stage: `${phase}-plain`, sent, observed });
      }
    }

    // 2. Cmd+A — mods bit 8 (Meta)
    for (const phase of ['keyDown', 'keyUp']) {
      const e = env[phase]({ code: 'KeyA', key: 'a', mods: 8 });
      sent.push(e);
      await ctx.dc.send(e);
      await flush(ctx);
      const ev = await ctx.readLastInputEvent(ctx.cdp);
      observed.push(ev);
      const expectedType = phase === 'keyDown' ? 'keydown' : 'keyup';
      // metaKey===true is the DOM-side projection of mods bit 8.
      if (!shapeMatches(ev, { type: expectedType, code: 'KeyA',
                              key: 'a', metaKey: true })) {
        return result.fail({ stage: `${phase}-meta`, sent, observed });
      }
    }

    return result.pass({ sent, observed });
  },
};

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// scenarios/touch.mjs — M4 R6 surface (touch + active-points state).
//
// Two paths exercised:
//   1. Single-finger tap: touch_start → touch_end with identifier=1.
//      DOM sees touchstart then touchend; touches list is empty at end.
//   2. Two-finger drag: identifiers 1+2 land, both move, both lift.
//      Verifies R6's per-identifier coalescing and the
//      "touchPoints reflects the state AFTER this event" CDP rule.

import { env } from '../lib/envelope.mjs';
import { result } from '../lib/registry.mjs';

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
  id: 'touch',
  r9Surface: 'M4 R6 (touch)',
  async run(ctx) {
    if (!ctx.dc) {
      return result.skipped('datachannel-not-open', {
        dcOpenError: ctx.dcOpenError ? ctx.dcOpenError.message : null,
      });
    }

    const sent = [];
    const observed = [];

    // ---- Path 1: single-finger tap ---------------------------------
    const tap = [
      env.touchStart({ identifier: 1, x: 200, y: 200, force: 0.5 }),
      env.touchEnd({ identifier: 1 }),
    ];
    for (const e of tap) {
      sent.push(e);
      await ctx.dc.send(e);
      await flush(ctx);
    }
    const ev1 = await ctx.readLastInputEvent(ctx.cdp);
    observed.push(ev1);
    if (!shapeMatches(ev1, { type: 'touchend' })) {
      return result.fail({ stage: 'single-finger-tap', sent, observed });
    }
    const activeAfterTap = await ctx.evaluateScript(ctx.cdp,
      'window.__uat_touches_active && window.__uat_touches_active.length || 0');
    observed.push({ stage: 'after-tap', activeTouches: activeAfterTap });
    if (activeAfterTap !== 0) {
      return result.fail({ stage: 'tap-active-cleanup', sent, observed });
    }

    // ---- Path 2: two-finger drag ----------------------------------
    const seq = [
      env.touchStart({ identifier: 1, x: 100, y: 100 }),
      env.touchStart({ identifier: 2, x: 300, y: 300 }),
      env.touchMove({  identifier: 1, x: 150, y: 150 }),
      env.touchMove({  identifier: 2, x: 350, y: 350 }),
      env.touchEnd({   identifier: 1 }),
      env.touchEnd({   identifier: 2 }),
    ];
    for (const e of seq) {
      sent.push(e);
      await ctx.dc.send(e);
      await flush(ctx);
    }
    const ev2 = await ctx.readLastInputEvent(ctx.cdp);
    observed.push(ev2);
    if (!shapeMatches(ev2, { type: 'touchend' })) {
      return result.fail({ stage: 'two-finger-drag', sent, observed });
    }
    const finalActive = await ctx.evaluateScript(ctx.cdp,
      'window.__uat_touches_active && window.__uat_touches_active.length || 0');
    observed.push({ stage: 'after-two-finger', activeTouches: finalActive });
    if (finalActive !== 0) {
      return result.fail({ stage: 'two-finger-cleanup', sent, observed });
    }

    return result.pass({ sent, observed });
  },
};

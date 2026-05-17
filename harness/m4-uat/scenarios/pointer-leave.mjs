// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// scenarios/pointer-leave.mjs — M4 R10 surface (last-pointer + leave).
//
// Three steps:
//   1. Send a mouse_move at (600, 400) and read R10's last-pointer
//      snapshot via the page-side test seam; verify the snapshot
//      reports (600, 400) with in_widget=true.
//   2. Send mouse_leave. Read the snapshot again; verify
//      in_widget=false but coordinates preserved (R10's
//      "freeze, don't snap" rule).
//   3. Send mouse_move at (700, 500); verify the snapshot returns
//      to in_widget=true with the new coordinates.
//
// The page-side test seam exposes the snapshot at
// `window.__uat_last_pointer` and is populated by the C++ test
// marker in capture/build-integration/cb_input_dispatch_test_marker.h
// (drafted alongside this harness). Until that marker compiles in
// the chromeless:ci image, the snapshot will be undefined and the
// scenario reports NYI.

import { env } from '../lib/envelope.mjs';
import { result, Status } from '../lib/registry.mjs';

async function flush(ctx) {
  await new Promise(r => setTimeout(r, 50));
}

export const scenario = {
  id: 'pointer-leave',
  r9Surface: 'M4 R10 (last-pointer + leave)',
  async run(ctx) {
    if (!ctx.dc) {
      return result.skipped('datachannel-not-open', {
        dcOpenError: ctx.dcOpenError ? ctx.dcOpenError.message : null,
      });
    }

    const sent = [];
    const observed = [];

    // 1. mouse_move + snapshot
    const m1 = env.mouseMove({ x: 600, y: 400 });
    sent.push(m1);
    await ctx.dc.send(m1);
    await flush(ctx);
    let snap = await ctx.evaluateScript(ctx.cdp, 'window.__uat_last_pointer');
    observed.push({ stage: 'after-move-1', snap });
    if (!snap) {
      // The test marker isn't installed in this image — common in
      // the integration/native-peer pre-build state.
      return result.notYetImplemented({
        reason: 'no __uat_last_pointer (C++ test marker not built)',
        sent, observed,
      });
    }
    if (snap.x !== 600 || snap.y !== 400 || snap.in_widget !== true) {
      return result.fail({ stage: 'initial-snapshot', sent, observed });
    }

    // 2. mouse_leave; verify coords preserved + in_widget=false
    const leave = env.mouseLeave();
    sent.push(leave);
    await ctx.dc.send(leave);
    await flush(ctx);
    snap = await ctx.evaluateScript(ctx.cdp, 'window.__uat_last_pointer');
    observed.push({ stage: 'after-leave', snap });
    if (!snap || snap.x !== 600 || snap.y !== 400 || snap.in_widget !== false) {
      return result.fail({ stage: 'leave-freeze', sent, observed });
    }

    // 3. mouse_move; verify in_widget flips back to true and coords update
    const m2 = env.mouseMove({ x: 700, y: 500 });
    sent.push(m2);
    await ctx.dc.send(m2);
    await flush(ctx);
    snap = await ctx.evaluateScript(ctx.cdp, 'window.__uat_last_pointer');
    observed.push({ stage: 'after-move-2', snap });
    if (!snap || snap.x !== 700 || snap.y !== 500 || snap.in_widget !== true) {
      return result.fail({ stage: 'reentry-snapshot', sent, observed });
    }

    return result.pass({ sent, observed });
  },
};

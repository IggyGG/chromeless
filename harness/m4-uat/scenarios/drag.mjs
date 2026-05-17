// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// scenarios/drag.mjs — M4 R7 surface (drag + drop, principal risk).
//
// Two paths exercised:
//   1. Happy path: drag_start → drag_over → drop → drag_end(success=true).
//      Verifies the drop target receives the text/plain payload and a
//      drop DOM event arrives.
//   2. Cancel path: drag_start → drag_over → drag_end(success=false)
//      without a drop. Verifies that R7's per-drag state holder
//      releases the drag (no stuck-drag — verifiable via the page-
//      side __uat_drag_active flag).

import { env } from '../lib/envelope.mjs';
import { result } from '../lib/registry.mjs';

async function flush(ctx) {
  await new Promise(r => setTimeout(r, 50));
}

export const scenario = {
  id: 'drag',
  r9Surface: 'M4 R7 (drag+drop)',
  async run(ctx) {
    if (!ctx.dc) {
      return result.skipped('datachannel-not-open', {
        dcOpenError: ctx.dcOpenError ? ctx.dcOpenError.message : null,
      });
    }

    const sent = [];
    const observed = [];

    const items = [
      { kind: 'string', type: 'text/plain', data: 'hello world' },
      { kind: 'string', type: 'text/uri-list', data: 'https://example.com/' },
    ];

    // ---- Path 1: drag + drop --------------------------------------
    const happyPath = [
      env.dragStart({ x: 200, y: 200, types: ['text/plain', 'text/uri-list'], items }),
      env.dragOver({ x: 220, y: 220 }),
      env.dragOver({ x: 240, y: 240 }),
      env.drop({ x: 250, y: 250, types: ['text/plain', 'text/uri-list'], items }),
      env.dragEnd({ success: true }),
    ];
    for (const e of happyPath) {
      sent.push(e);
      await ctx.dc.send(e);
      await flush(ctx);
    }
    const dropTextRead = await ctx.evaluateScript(ctx.cdp,
      'window.__uat_last_drop_text || null');
    observed.push({ stage: 'drop', text: dropTextRead });
    if (dropTextRead !== 'hello world') {
      return result.fail({ stage: 'drop-payload', sent, observed });
    }
    const activeAfter = await ctx.evaluateScript(ctx.cdp,
      'window.__uat_drag_active');
    observed.push({ stage: 'after-happy', active: activeAfter });
    if (activeAfter) {
      return result.fail({ stage: 'drag-not-released', sent, observed });
    }

    // ---- Path 2: cancel without drop ------------------------------
    await ctx.evaluateScript(ctx.cdp,
      'window.__uat_last_drop_text = null; window.__uat_drag_active = false;');
    const cancelPath = [
      env.dragStart({ x: 100, y: 100, types: ['text/plain'],
                      items: [{ kind: 'string', type: 'text/plain', data: 'aborted' }] }),
      env.dragOver({ x: 120, y: 120 }),
      env.dragEnd({ success: false }),
    ];
    for (const e of cancelPath) {
      sent.push(e);
      await ctx.dc.send(e);
      await flush(ctx);
    }
    const cancelText = await ctx.evaluateScript(ctx.cdp,
      'window.__uat_last_drop_text || null');
    observed.push({ stage: 'after-cancel', text: cancelText });
    if (cancelText !== null) {
      return result.fail({ stage: 'cancel-leaked-drop', sent, observed });
    }
    const activeAfterCancel = await ctx.evaluateScript(ctx.cdp,
      'window.__uat_drag_active');
    observed.push({ stage: 'cancel-active', active: activeAfterCancel });
    if (activeAfterCancel) {
      return result.fail({ stage: 'cancel-did-not-release', sent, observed });
    }

    return result.pass({ sent, observed });
  },
};

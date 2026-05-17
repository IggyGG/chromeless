// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// scenarios/ime.mjs — M4 R5 surface (IME composition).
//
// Three paths exercised:
//   1. composition_start → composition_update → composition_end
//      with committed text 你好. Verifies the DOM sees a
//      compositionend with `data: "你好"` and the focused input
//      receives the committed string.
//   2. composition_cancel after a partial composition. Verifies
//      the input's value is empty (no commit happened).
//   3. Suppression rule: a key_down sent while composition is in
//      flight MUST NOT produce a DOM keydown. R5's suppression
//      logic lives on the native side; the harness asserts it
//      from the DOM by counting keydowns before/after.

import { env } from '../lib/envelope.mjs';
import { result } from '../lib/registry.mjs';

const INPUT_SELECTOR = '#uat-text-input';

async function flush(ctx) {
  await new Promise(r => setTimeout(r, 60));
}

export const scenario = {
  id: 'ime',
  r9Surface: 'M4 R5 (IME)',
  async run(ctx) {
    if (!ctx.dc) {
      return result.skipped('datachannel-not-open', {
        dcOpenError: ctx.dcOpenError ? ctx.dcOpenError.message : null,
      });
    }

    await ctx.evaluateScript(ctx.cdp,
      `document.querySelector('${INPUT_SELECTOR}').focus();` +
      `document.querySelector('${INPUT_SELECTOR}').value = '';`);

    const sent = [];
    const observed = [];

    // ---- Path 1: full composition committing 你好 -----------------
    const seq1 = [
      env.compositionStart({ data: '', rect: { x: 100, y: 100, w: 12, h: 18 } }),
      env.compositionUpdate({ data: 'ni hao', selection_start: 6, selection_end: 6,
                              candidate_list: ['你好', '拟好'] }),
      env.compositionEnd({ data: '你好' }),
    ];
    for (const e of seq1) {
      sent.push(e);
      await ctx.dc.send(e);
      await flush(ctx);
    }
    const inputValue = await ctx.evaluateScript(ctx.cdp,
      `document.querySelector('${INPUT_SELECTOR}').value`);
    observed.push({ stage: 'commit', value: inputValue });
    if (inputValue !== '你好') {
      return result.fail({ stage: 'composition-commit', sent, observed });
    }
    const lastEv = await ctx.readLastInputEvent(ctx.cdp);
    observed.push(lastEv);
    if (!lastEv || lastEv.type !== 'compositionend' || lastEv.data !== '你好') {
      return result.fail({ stage: 'compositionend-event', sent, observed });
    }

    // ---- Path 2: composition_cancel after partial update ---------
    await ctx.evaluateScript(ctx.cdp,
      `document.querySelector('${INPUT_SELECTOR}').focus();` +
      `document.querySelector('${INPUT_SELECTOR}').value = '';`);
    const seq2 = [
      env.compositionStart({ data: '' }),
      env.compositionUpdate({ data: 'half' }),
      env.compositionCancel(),
    ];
    for (const e of seq2) {
      sent.push(e);
      await ctx.dc.send(e);
      await flush(ctx);
    }
    const afterCancel = await ctx.evaluateScript(ctx.cdp,
      `document.querySelector('${INPUT_SELECTOR}').value`);
    observed.push({ stage: 'cancel', value: afterCancel });
    if (afterCancel !== '') {
      return result.fail({ stage: 'composition-cancel', sent, observed });
    }

    // ---- Path 3: key_down during composition is suppressed -------
    // Reset state, start a composition, then send a key_down for KeyA.
    // The DOM keydown listener counts events; the count MUST be 0
    // for the suppression window.
    await ctx.evaluateScript(ctx.cdp,
      `window.__uat_keydown_count = 0;` +
      `document.querySelector('${INPUT_SELECTOR}').focus();` +
      `document.querySelector('${INPUT_SELECTOR}').value = '';`);
    await ctx.dc.send(env.compositionStart({ data: '' })); sent.push(seq2[0]);
    await flush(ctx);
    const keyDuringComp = env.keyDown({ code: 'KeyA', key: 'a', mods: 0 });
    sent.push(keyDuringComp);
    await ctx.dc.send(keyDuringComp);
    await flush(ctx);
    const kc = await ctx.evaluateScript(ctx.cdp, 'window.__uat_keydown_count');
    observed.push({ stage: 'suppression', keydownCount: kc });
    if (kc !== 0) {
      return result.fail({ stage: 'key-suppression-during-composition',
                           sent, observed, suppressionLeak: kc });
    }
    // Cancel to leave the input in a known state for later scenarios.
    await ctx.dc.send(env.compositionCancel()); sent.push(seq2[2]);
    await flush(ctx);

    return result.pass({ sent, observed });
  },
};

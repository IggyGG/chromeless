// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// scenarios/clipboard.mjs — M4 R8 surface (clipboard_copy_request).
//
// R8 turns a `clipboard_copy_request` envelope into a synthesised
// Ctrl+C (Cmd+C on macOS, but the cloud chromium runs Linux so it's
// always Ctrl+C). The harness verifies:
//   1. A `copy` DOM event arrives on document after the request.
//   2. The selected text in the page makes it to navigator.clipboard
//      (the page's copy handler writes it; we can read it back via
//      Clipboard API IFF the harness page has been granted
//      clipboard-read permission).
//
// TODO(M4-R9-clipboard-permission): navigator.clipboard.readText
// requires a transient user activation or an automation permission
// grant on the CDP side. For DRAFT we read window.__uat_last_copy_text
// which the page-side `copy` handler populates.

import { env } from '../lib/envelope.mjs';
import { result } from '../lib/registry.mjs';

async function flush(ctx) {
  await new Promise(r => setTimeout(r, 60));
}

export const scenario = {
  id: 'clipboard',
  r9Surface: 'M4 R8 (clipboard_copy_request)',
  async run(ctx) {
    if (!ctx.dc) {
      return result.skipped('datachannel-not-open', {
        dcOpenError: ctx.dcOpenError ? ctx.dcOpenError.message : null,
      });
    }

    const sent = [];
    const observed = [];

    // 1. Select the well-known fixture text in the page.
    await ctx.evaluateScript(ctx.cdp,
      `const r = document.createRange();` +
      `r.selectNodeContents(document.querySelector('#uat-copy-source'));` +
      `const sel = window.getSelection();` +
      `sel.removeAllRanges(); sel.addRange(r);` +
      `window.__uat_last_copy_text = null;`);

    // 2. Send the copy_request envelope.
    const e = env.clipboardCopyRequest();
    sent.push(e);
    await ctx.dc.send(e);
    await flush(ctx);

    // 3. Confirm: the page-side `copy` listener fired, populated
    //    window.__uat_last_copy_text, AND document.lastInputEvent
    //    reflects the synthesised Ctrl+C.
    const copiedText = await ctx.evaluateScript(ctx.cdp,
      'window.__uat_last_copy_text');
    observed.push({ stage: 'copy', text: copiedText });
    if (typeof copiedText !== 'string' || !copiedText.includes('UAT-CLIPBOARD-CANARY')) {
      return result.fail({ stage: 'copy-text-missing', sent, observed });
    }

    const lastEv = await ctx.readLastInputEvent(ctx.cdp);
    observed.push(lastEv);
    // R8 synthesises Ctrl+C; either the keydown for KeyC or the
    // copy event itself is fine evidence. We accept both.
    const okEv = lastEv && (
      (lastEv.type === 'keydown' && lastEv.code === 'KeyC' && lastEv.ctrlKey === true)
      || lastEv.type === 'copy'
    );
    if (!okEv) {
      return result.fail({ stage: 'copy-dom-event', sent, observed });
    }

    return result.pass({ sent, observed });
  },
};

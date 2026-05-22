// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// harness/m4-uat/lib/datachannel.mjs — open the `input` DataChannel
// against the native peer.
//
// This module is the harness-side counterpart of:
//   - M3 R2 signaling client (cb_signaling_client.{h,cc})
//   - M3 R4 offerer handshake driver
//   - M3 R5 DataChannel host + consumer-bind API
//   - M4 R1 input DC sink (cb_input_dispatch.{h,cc})
//
// The harness plays the *client* role: it initiates the offer, the
// native peer answers, and the harness drives the `input` channel.
// In production the renderer process does this from inside the page
// the user opened; the harness does it from outside the container so
// the chain under test is the actual native code path the renderer
// would exercise.
//
// DRAFT — the actual WebRTC dance below is stubbed. Two viable
// implementations:
//
//   A. Embed `wrtc` (node WebRTC) — full browser-shaped API.
//      Pulls in ~30MB of binary deps; works on Linux x86_64 only.
//
//   B. Drive the harness from a *second* Chromium instance also
//      running inside the container (`harness-driver` user-data-dir),
//      eval `new RTCPeerConnection(...)` on a blank page, marshal
//      offers / answers / ICE candidates over CDP. Heavier per
//      scenario but no native deps; reuses the existing CDP plumbing.
//
// TODO(M4-R9-webrtc-impl): pick A or B once the M3 signaling URL
// shape stabilises. Leaning B because (a) the M0 harness already
// runs CDP-side and (b) it tests the rendererpath. For DRAFT the
// stub below preserves the surface uat.mjs depends on.

export async function openNativeDataChannel({ cdp, timeoutMs = 30_000,
                                              signalingUrl = null,
                                              iceServers = [] } = {}) {
  // The signalingUrl defaults to whatever the container exposes on
  // the M3 R2 signaling port. Today that port is not yet stable;
  // when M3 R5's consumer-bind API exposes it on /json/version-style
  // discovery, plumb it through here.
  //
  // Returns an object with:
  //   .send(envelope)    — JSON.stringify + RTCDataChannel.send
  //   .close()           — close the DC then the PC
  //   .readyState        — 'open' | 'connecting' | 'closing' | 'closed'

  throw new Error(
    'openNativeDataChannel: NOT YET WIRED — see TODO(M4-R9-webrtc-impl). '
    + 'For now uat.mjs catches this and either SKIPs scenarios (scaffold) '
    + 'or FAILs early (strict).'
  );
}

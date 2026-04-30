# SDP munging

Why and how `cloud-browser-webrtc` rewrites SDP between
`createOffer` and `setLocalDescription`. Owner: webrtc-dev.

## TL;DR

- We **always** call `prioritizeCodec(offer.sdp, "VP9")` before sending
  the offer. VP9 is our v1 software-encode target — it gives noticeably
  better quality-per-bit than VP8 at our latency budget, and Chromium,
  Firefox, and Safari (>= 14) all accept it.
- We **conditionally** call `forceH264Profile(offer.sdp, "42e01f")` when
  H.264 is the negotiated path (currently as a fallback only). The
  default `42e01f` (Constrained Baseline 3.1) disallows B-frames, which
  are the single biggest source of 100ms+ encoder latency.
- We **rarely** call `stripExtensions(offer.sdp, [...])` — kept as a
  surgical tool for when a specific RTP header extension causes a bug.
  Default config strips nothing.

## Why munge SDP at all?

The proper API is
`RTCRtpTransceiver.setCodecPreferences(codecs)`. It works for ordering
and is preferred when sufficient. We still munge text because:

1. **fmtp parameters.** `setCodecPreferences` re-orders the codec
   list; it does not rewrite an `fmtp` line. To force an H.264
   profile-level-id we have to edit text.
2. **Cross-browser robustness.** Some Safari versions ignore
   `setCodecPreferences` for H.264 profiles. Some Firefox versions
   accept it but emit different `fmtp` defaults than Chromium.
3. **Symmetry with answerer.** The answerer in our pipeline is the
   cloud-side streamer (T23). It either accepts whatever the offerer
   advertised or downgrades. By restricting the offer up front, we get
   predictable answers.

## Functions

All in `client/src/sdp.ts`. Pure functions, no side effects, fully
unit-tested against real Chromium 147 SDP plus representative
Firefox/Safari fixtures.

### `prioritizeCodec(sdp, codec) → sdp`

Re-orders the m=video payload-type list so PTs whose `a=rtpmap` matches
`codec` (case-insensitive) come first. Untouched if the codec is absent.
Handles both BUNDLE'd and non-BUNDLE'd SDP. Audio sections are not
touched.

### `forceH264Profile(sdp, profileLevelId = "42e01f") → sdp`

For every H.264 payload type in m=video, rewrites its `a=fmtp` line so
the params are exactly:

```
level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=<value>
```

Other unrelated `fmtp` lines (VP9 `profile-id=*`, audio `minptime` etc.)
are preserved verbatim.

Recommended values:

| value      | profile               | notes |
|------------|-----------------------|-------|
| `42e01f`   | Constrained Baseline 3.1 | **default**; no B-frames; broadest support |
| `42001f`   | Baseline 3.1            | similar but not "Constrained"; some encoders enable B-frames |
| `4d001f`   | Main 3.1                | better compression but encoder must be told no-B-frames separately |
| `640c1f`   | High 3.1                | not recommended for low-latency pipelines |

If you change the default, also change the encoder configuration on the
streamer side (T19/T35) so it actually emits a stream matching the
profile we negotiated. Mismatch produces a working ICE+DTLS connection
that decodes to garbage.

### `stripExtensions(sdp, extUris) → sdp`

Removes `a=extmap:<n> [...] <uri>` lines whose URI matches any string in
`extUris`. Useful for working around peer-specific bugs. Targets we have
considered:

- `urn:3gpp:video-orientation` — safe to strip for screen capture, where
  rotation is meaningless.
- `http://www.webrtc.org/experiments/rtp-hdrext/playout-delay` — the
  W3C draft for jitter-buffer hint; some answerers misparse it.

`a=extmap-allow-mixed` is intentionally left in place; removing it
requires deeper renegotiation logic than we want to take on at the
text level.

## When munging applies (offerer vs answerer)

In v1 the **client** is the offerer; the streamer answers. We munge on
the offer, before `setLocalDescription`. This is consistent with most
WebRTC tooling.

Per `docs/capture/path-of-least-resistance.md` (T15) and T23, that
roles flip: the **streamer** holds media and offers, the client
answers (T34 ships this flip). At that point the same munging functions
apply, but on the answer SDP — they are pure transforms and don't care
which side they run on. T34 will move the call sites accordingly.

## Test fixtures

- `client/src/__fixtures__/chromium-offer.sdp` — generated from real
  headless Chromium 147 with two recvonly transceivers (video + audio).
  Re-generate by running:
  ```ts
  const pc = new RTCPeerConnection();
  pc.addTransceiver("video", { direction: "recvonly" });
  pc.addTransceiver("audio", { direction: "recvonly" });
  console.log((await pc.createOffer()).sdp);
  ```
- `client/src/__fixtures__/firefox-offer.sdp` — representative
  shape (Firefox 99-style), VP8/VP9/H.264 with `extmap:6/recvonly`
  direction modifier.
- `client/src/__fixtures__/safari-offer.sdp` — representative
  shape, leads with H.264 (multiple profile-level-ids and
  packetization-modes), VP8 fallback.

When we add new browser targets (e.g., AV1-capable Chrome stable),
capture a fresh fixture and add a parametrised test rather than
mutating the existing fixture in place.

## Operational notes

- Munging happens **client-side** for v1. Keeping it client-side means
  the signaling server stays opaque to media. If we ever want SFU-style
  intermediation, the same functions can run server-side; they have no
  DOM dependencies.
- All three functions are O(n) over SDP length. SDP is small; no
  measurable cost.
- If a transform produces invalid SDP, `setLocalDescription` will throw
  synchronously. The client surfaces this through the existing error
  log path (`onError` in `main.ts`).

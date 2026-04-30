# Decoder verification matrix (client-side codec support)

**Status:** research only. Companion to `docs/research/av1-encoders.md`
(T43) — encoder side covered there, decoder side covered here.
**Owner:** chromium-dev.
**Cross-references:** `capture/encoder/encoder_factory.h` (T19),
`client/src/codec-negotiate.ts` (T54),
`client/src/sdp.ts` (T30, prioritizeCodec),
`docs/research/av1-encoders.md` (T43).

This document is the fact base for codec **negotiation**. Our
encoder factory now offers VP9 + H.264 (BL/Main) + HEVC + AV1 — but
"we can encode it" doesn't mean "the user can decode it." Without
client-side data, T54's `VIDEO_CODEC_PREFERENCE` is a guess; with
this matrix it's a policy.

## 1. Per-codec, per-client support matrix

For each cell:
- **HW** — Y if a hardware decoder exists in the typical 2025-era
  device. N otherwise.
- **SW** — Y if the browser ships a software decoder fallback. N if
  the browser refuses the codec entirely.
- The cell shows `HW/SW` plus a short caveat where one matters.

Codec rows below are the ones we actually negotiate today; HEVC is
included for forward-planning given Chrome M136+'s WebRTC HEVC
support
([blink-dev intent-to-ship](https://groups.google.com/a/chromium.org/g/blink-dev/c/3h8lL8a377c/m/_vnatJetAQAJ)).

### Chrome desktop (Win / Mac / Linux), M120+

| Codec      | HW | SW | Notes                                                                           |
|------------|----|----|----------------------------------------------------------------------------------|
| VP9        | Y  | Y  | Native priority for Google's WebRTC stack. Best-tested path.                    |
| H.264 BL3.1 | Y  | Y  | Universal. HW decoder on every Intel iGPU since Sandy Bridge / AMD VCN1+ / NVDEC.|
| H.264 Main3.1 | Y | Y  | Same HW path as BL; some older HW falls back to SW for B-frames.                |
| HEVC Main  | Y* | Y  | M136+ WebRTC HEVC supported natively without flags. *HW limited to systems with HEVC license / iGPU support. |
| AV1 8-bit  | Y* | Y  | M120+ HW decode where the GPU has it (Intel Tiger Lake+, NVIDIA RTX 30+, AMD RX 6000+). SW fallback via libdav1d everywhere else. |

### Chrome mobile (Android, M120+)

| Codec      | HW | SW | Notes                                                                           |
|------------|----|----|----------------------------------------------------------------------------------|
| VP9        | Y  | Y  | HW on Snapdragon 845+, Tensor G1+, MediaTek Dimensity 1000+. SW elsewhere.      |
| H.264 BL3.1 | Y  | Y  | Universal mobile HW decode.                                                     |
| H.264 Main3.1 | Y | Y  | Same.                                                                            |
| HEVC Main  | Y* | Y  | HW on most Snapdragon 6xx+ / Tensor / Dimensity. *Vendor-licensed — not in AOSP for all OEMs. |
| AV1 8-bit  | Y* | Y  | HW on Pixel 6+ (Tensor G1), Snapdragon 8 Gen 2+ ([Qualcomm announcement](https://www.androidpolice.com/qualcomms-next-flagship-chip-could-support-native-av1-decoding/)), Dimensity 9000+. SW fallback via dav1d on Android 12+ ([9to5google](https://9to5google.com/2024/04/19/android-av1-software-decoder/)) — works but battery-hungry on weak chips. |

### Chrome iOS

iOS Chrome is **WebKit under the hood** by App Store policy; treat
identically to Safari iOS. We list it once below and skip the
duplicate row.

### Safari desktop (macOS), 17.4+

| Codec      | HW | SW | Notes                                                                           |
|------------|----|----|----------------------------------------------------------------------------------|
| VP9        | Y* | N  | Decode shipped in Safari 17.0; HW only — *Apple Silicon M3+ only* ([WebRTC Safari guide](https://www.videosdk.live/developer-hub/webrtc/webrtc-safari)). Older Macs (Intel + M1/M2): no decode at all. |
| H.264 BL3.1 | Y  | Y  | Universal — Safari's preferred codec, fully HW-accelerated since the dawn of time. |
| H.264 Main3.1 | Y | Y  | Same.                                                                            |
| HEVC Main  | Y  | Y  | Safari has shipped WebRTC HEVC since 14; HW everywhere on Apple Silicon and Intel Macs with VideoToolbox. |
| AV1 8-bit  | Y* | **N** | M3+ Macs only ([Bitmovin](https://bitmovin.com/blog/apple-av1-support/)). Apple has *no system-wide software AV1 decoder*. M1/M2/Intel Mac users on Safari: AV1 cannot play. |

### Safari mobile (iOS, iPadOS), 17.4+

| Codec      | HW | SW | Notes                                                                           |
|------------|----|----|----------------------------------------------------------------------------------|
| VP9        | Y* | N  | iPhone 15 Pro / iPad Pro M4+ only; HW decode only, no SW fallback.              |
| H.264 BL3.1 | Y  | Y  | Universal — every iPhone since the iPhone 3GS.                                  |
| H.264 Main3.1 | Y | Y  | Same.                                                                            |
| HEVC Main  | Y  | Y  | HW everywhere since A9 (iPhone 6s, 2015).                                       |
| AV1 8-bit  | Y* | **N** | A17 Pro / iPhone 15 Pro+, iPad Pro M4+ ([HitPaw on iPhone AV1](https://www.hitpaw.com/iphone-tips/iphone-av1.html)). All other iOS/iPadOS devices: AV1 cannot play. **Critical:** the SW fallback simply doesn't exist on Apple platforms.|

### Firefox desktop (Win / Mac / Linux), 130+

| Codec      | HW | SW | Notes                                                                           |
|------------|----|----|----------------------------------------------------------------------------------|
| VP9        | Y  | Y  | Native; Firefox has shipped VP9 decode since 28.                                 |
| H.264 BL3.1 | Y  | Y  | Universal.                                                                       |
| H.264 Main3.1 | Y | Y  | Universal.                                                                       |
| HEVC Main  | Y* | Y* | Firefox 134 added Windows HW HEVC ([Neowin](https://www.neowin.net/news/firefox-134-finally-gets-windows-hardware-h265-support-improves-popup-blocking-and-more/)); Firefox 137 added Linux VA-API HEVC ([Phoronix](https://www.phoronix.com/news/Firefox-137-VA-API-HEVC)). **WebRTC HEVC is separate** and as of early 2025 was still under review by Mozilla — *do not assume Firefox can decode HEVC over WebRTC even if it plays HEVC `<video>`.* |
| AV1 8-bit  | Y* | Y  | HW where GPU supports it (Intel TGL+, NVIDIA RTX 30+, AMD RX 6000+); SW via libdav1d everywhere else. |

### Firefox mobile (Android), 130+

| Codec      | HW | SW | Notes                                                                           |
|------------|----|----|----------------------------------------------------------------------------------|
| VP9        | Y  | Y  | Same hardware envelope as Chrome Android.                                       |
| H.264 BL3.1 | Y  | Y  | Universal.                                                                       |
| H.264 Main3.1 | Y | Y  | Universal.                                                                       |
| HEVC Main  | N* | N* | Firefox Android does **not** ship HEVC for codec-licensing reasons.             |
| AV1 8-bit  | Y* | Y  | HW where chip supports it; SW dav1d on Android 12+.                             |

Firefox iOS is WebKit; treat as Safari iOS.

### Edge (Chromium-based), Win / Mac

Edge ≡ Chrome desktop for our purposes. Same matrix; same negotiation
policy. Called out here because some negotiation logic is tempted to
key off the "Edge" UA token and pick a different path — **don't.**
Edge inherits Chrome's decoder set and Chrome's bugs.

## 2. Empirical verification methodology

How we built the matrix above:

1. **Browser-vendor public docs.** caniuse.com tables for `av1`,
   `vp9`, `hevc`; MDN's [WebRTC codecs guide](https://developer.mozilla.org/en-US/docs/Web/Media/Guides/Formats/WebRTC_codecs);
   the [WebRTC browser-support roundup](https://antmedia.io/webrtc-browser-support/)
   for cross-checking the cells. Where browsers list "decode
   supported" but only on certain hardware, that note carries into
   the matrix.
2. **Vendor blog posts and chromestatus / blink-dev intents.**
   Authoritative for "as of which version" data — e.g., the
   [Chrome HEVC WebRTC blink-dev intent](https://groups.google.com/a/chromium.org/g/blink-dev/c/3h8lL8a377c/m/_vnatJetAQAJ),
   the [Apple AV1 hardware roundup](https://bitmovin.com/blog/apple-av1-support/).
3. **`RTCRtpReceiver.getCapabilities("video")`** — the runtime
   ground truth. The static method returns the decoder codec list
   the current browser advertises ([MDN](https://developer.mozilla.org/en-US/docs/Web/API/RTCRtpReceiver/getCapabilities_static))
   on the current hardware. **This is the only data source we
   trust at session time** — see §4. The matrix is for *planning*
   defaults; runtime probing is for the actual decision.
4. **Empirical session data.** Once we have telemetry (Phase 3+),
   the per-User-Agent decode-success rates feed back into the
   policy. Until then, we treat every cell with a star (`*`) as
   "verify on representative client device before relying on it."

A note on `getCapabilities()`'s caveats. The MDN note is load-
bearing: *"the returned set of capabilities is the most optimistic
possible list. It is entirely possible that certain combinations of
options may fail to work when you actually try to use them."* We
treat the list as a strict ceiling on what to offer; we do not treat
"listed" as "guaranteed to play smoothly."

## 3. Negotiation policy implications

Given the matrix, the recommended **per-User-Agent codec preference
list** (top of the list = preferred) is:

| Client class                                | Preference order                  |
|---------------------------------------------|-----------------------------------|
| Chrome desktop (any OS), Edge desktop       | VP9 → AV1 → H.264 BL3.1           |
| Chrome Android (any chip)                   | H.264 BL3.1 → VP9 → AV1           |
| Safari macOS, M3+                           | VP9 → H.264 BL3.1 (skip AV1 unless we know hardware) |
| Safari macOS, pre-M3                        | H.264 BL3.1 → HEVC                |
| Safari iOS, iPhone 15 Pro+ / iPad Pro M4+   | HEVC → H.264 BL3.1 (skip VP9/AV1; HW-only and unreliable to detect) |
| Safari iOS / iPadOS, all others             | H.264 BL3.1 → HEVC                |
| Firefox desktop                             | VP9 → AV1 → H.264 BL3.1           |
| Firefox Android                             | H.264 BL3.1 → VP9 → AV1           |

Why mobile prefers H.264 even when it has HW for VP9/AV1:

- **Battery.** H.264 HW decoders are decades old, heavily optimized,
  and use less power per frame than newer codec HW blocks on the
  same chip.
- **Frame-pacing.** H.264 HW decoders have the most predictable
  drain rate; VP9/AV1 HW decoders on weaker chips show variable
  per-frame latency that the browser can mask but the user feels.
- **Failure mode.** When H.264 HW decode fails, every browser has a
  competent SW fallback (libavcodec/openh264). When VP9 fails on
  Safari iOS the user gets a black box — the SW path doesn't exist.

This translates directly into T54's `VIDEO_CODEC_PREFERENCE`. The
existing single global constant
(`["VP9", "AV1", "H264", "VP8"]` in `client/main.ts`) becomes a
**function of the negotiated client class**:

```ts
// Sketch for client/src/codec-negotiate.ts
function preferredCodecs(ua: NavigatorUA): readonly string[] {
  if (ua.platform === "iOS" || (ua.brand === "Safari" && !isM3OrLater(ua))) {
    return ["H264", "HEVC"];        // legacy Apple; skip VP9/AV1.
  }
  if (ua.brand === "Safari") {
    return ["VP9", "H264"];         // M3+ Mac: VP9 OK, AV1 only on M3+ which we can't probe via UA.
  }
  if (ua.mobile) {
    return ["H264", "VP9", "AV1"]; // mobile: H.264 first for battery.
  }
  return ["VP9", "AV1", "H264"];    // Chrome / Firefox / Edge desktop.
}
```

The User-Agent classes above are intentionally coarse. UA Client
Hints (`navigator.userAgentData`) is the right API in 2025 — it
gives us `brands`, `mobile`, `platform`, and `platformVersion`
without the legacy UA string parsing nightmare. UA-CH is supported
in Chrome / Edge / Opera; absent in Safari and Firefox, so we keep a
fallback path that parses `navigator.userAgent`.

## 4. Decoder probing at runtime (Phase 2+ stretch)

UA-based heuristics are wrong somewhere — Safari versions roll, M3
detection is imperfect, niche browsers exist. The proper fix is
**probing the actual client decoder set**.

### How

`RTCRtpReceiver.getCapabilities("video")` is supported across Chrome,
Firefox, and Safari (per [the cross-browser
codec-preferences post](https://blog.mozilla.org/webrtc/cross-browser-support-for-choosing-webrtc-codecs/)).
The client side calls it before constructing the RTCPeerConnection,
serializes the result, and ships it to the streamer over the
control channel. The streamer uses it to:

1. Filter `GetSupportedFormats()` so we only advertise codecs the
   client actually claims to decode.
2. Pick the **encoder** behind the SdpVideoFormat the client most
   prefers (after our own preference order applies — we still
   prefer VP9 first if both sides can do it).

### Wire shape

A new `client_decoder_caps` envelope on the existing data channel
infrastructure (T20 `input` channel reused, OR a new `caps` channel
opened by the client at session start):

```jsonc
{
  "type": "client_decoder_caps",
  "from": "client",
  "data": {
    "v": 1,
    // Verbatim from RTCRtpReceiver.getCapabilities("video").codecs
    // — we don't reformat. SdpVideoFormat-shaped on the streamer
    // side so encoder_factory_stub.cc can match against
    // GetSupportedFormats().
    "codecs": [
      { "mimeType": "video/VP9", "clockRate": 90000,
        "sdpFmtpLine": "profile-id=0" },
      { "mimeType": "video/H264", "clockRate": 90000,
        "sdpFmtpLine": "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f" },
      ...
    ],
    // Optional UA-CH summary so the streamer can log per-class
    // negotiation outcomes for future policy tuning.
    "ua": {
      "brands": [{"brand":"Chromium","version":"125"}],
      "mobile": false,
      "platform": "Linux"
    }
  }
}
```

Streamer-side handler (`capture/streamer-page/streamer.js` —
follow-up to T54 once we ship this):

1. Wait for `client_decoder_caps` before `createOffer`.
2. Build the offered SDP from the **intersection** of our
   `GetSupportedFormats()` and the client's advertised list.
3. Apply our preference order over that intersection.
4. If the channel never delivers caps within ~500 ms, fall back to
   the UA-class default from §3.

### Why Phase 2+

- **Wire churn.** This adds a channel + a sequencing constraint
  (caps before offer) to the v1 negotiation. Worth doing when
  there's evidence the heuristics misfire, not before.
- **Doesn't help the bootstrap case.** The first SDP offer the
  user's browser sees still goes through the streamer's defaults;
  subsequent renegotiation is what benefits from the cap exchange.
  We need the renegotiation flow (T37 reconnect path is a partial
  fit) before this pays off.
- **Telemetry-first.** Wire the per-class negotiation-outcome
  metric (already a stub in T42 stats sampler) into the cb-metrics
  sidecar; let real data tell us which UA classes are negotiating
  poorly, then ship probing for *those* classes specifically.

## 5. Cross-references

- `docs/research/av1-encoders.md` (T43) — encoder-side
  prerequisites; matrix above is symmetric.
- `client/src/codec-negotiate.ts` (T54) — current
  multi-codec-fallback implementation. Needs the per-class
  preference list from §3.
- `client/src/sdp.ts` (T30) — `prioritizeCodec()` is what actually
  reorders the answer SDP. The negotiate module decides *what*;
  this module decides *how to spell it.*
- `capture/encoder/encoder_factory.h` (T19) +
  `encoder_factory_stub.cc` —
  `GetSupportedFormats()` is the offer side; once the runtime
  probing in §4 ships, this method's output gets gated by the
  client's advertised codecs.

## Sources

- [MDN — RTCRtpReceiver.getCapabilities()](https://developer.mozilla.org/en-US/docs/Web/API/RTCRtpReceiver/getCapabilities_static)
- [MDN — WebRTC codecs guide](https://developer.mozilla.org/en-US/docs/Web/Media/Guides/Formats/WebRTC_codecs)
- [Mozilla — cross-browser support for choosing WebRTC codecs](https://blog.mozilla.org/webrtc/cross-browser-support-for-choosing-webrtc-codecs/)
- [caniuse — AV1](https://caniuse.com/av1), [HEVC](https://caniuse.com/hevc)
- [Antmedia — WebRTC Browser Support 2026](https://antmedia.io/webrtc-browser-support/)
- [VideoSDK — WebRTC Safari 2025 guide](https://www.videosdk.live/developer-hub/webrtc/webrtc-safari)
- [Bitmovin — Apple AV1 Support](https://bitmovin.com/blog/apple-av1-support/)
- [Coconut — AV1 Supported Devices](https://www.coconut.co/articles/av1-supported-devices-complete-list-updates)
- [9to5google — Android software AV1 decoder](https://9to5google.com/2024/04/19/android-av1-software-decoder/)
- [Android Police — Snapdragon AV1 native decode](https://www.androidpolice.com/qualcomms-next-flagship-chip-could-support-native-av1-decoding/)
- [HitPaw — iPhone AV1 support](https://www.hitpaw.com/iphone-tips/iphone-av1.html)
- [Phoronix — Firefox 137 VA-API HEVC](https://www.phoronix.com/news/Firefox-137-VA-API-HEVC)
- [Neowin — Firefox 134 Windows HW H.265](https://www.neowin.net/news/firefox-134-finally-gets-windows-hardware-h265-support-improves-popup-blocking-and-more/)
- [Chromium blink-dev — H.265 in WebRTC](https://groups.google.com/a/chromium.org/g/blink-dev/c/3h8lL8a377c/m/_vnatJetAQAJ)

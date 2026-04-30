# AV1 encoder survey (Phase 4 prep)

**Status:** research only. Phase 4 candidate per `PROJECT_BRIEF.md`.
**Owner:** chromium-dev.
**Cross-references:** `capture/encoder/encoder_factory.h` (T19),
`docs/internal/vp9-tuning-rationale.md` (T35),
`docs/prior-art/selkies.md` (T3 — they list `nvav1enc` in their
encoder set).

This document captures the AV1 encoder landscape as of early 2026.
It is not an implementation plan; the goal is to give us a fact base
so when we open a Phase 4 task we don't waste a week re-running the
same searches.

## 1. Why AV1 for a cloud browser

AV1 is the strongest argument for changing codecs we have. Two reasons.

**Bandwidth at our content type.** Our wire is a Chromium tab — text,
UI chrome, sub-rectangles changing under intra-refresh, occasional
embedded video. Generic codec comparisons claim ~30–50% bitrate
savings over VP9 and ~50% over H.264 at equal quality
([CDNetworks](https://www.cdnetworks.com/blog/media-delivery/compression-standards-explained/),
[Antmedia](https://antmedia.io/video-codecs-streaming-guide/)). For
**screen content specifically**, libaom's screen-content tune
(turned on by `--tune=screen` in libaom 3.7+) reports 10–19% BD-rate
gains over the default tune at speeds 6–10
([Phoronix on libaom 3.7](https://www.phoronix.com/news/Google-libaom-3.7-AV1)).
NVIDIA reports 40% bitrate saving for NVENC AV1 over H.264 at
1080p60 on generic content
([NVIDIA dev blog](https://developer.nvidia.com/blog/improving-video-quality-and-performance-with-av1-and-nvidia-ada-lovelace-architecture/)).
Visionular's WebRTC-specific benches put AV1 in the same ballpark
([Visionular](https://visionular.ai/av1-for-webrtc/)).

**Bandwidth-constrained clients.** v1 ships VP9 (T35) and H.264
(T36). For users on cell or saturated WiFi, AV1's per-bit advantage
is the largest single quality lever we have without adding more
hardware. This is the same story Selkies' inclusion of `nvav1enc`
hints at, even if their pipeline is structurally different.

What AV1 is **not** is free. Software encoding is 5–10× slower than
VP9 ([Antmedia](https://antmedia.io/video-codecs-streaming-guide/)).
Screen-content benefits depend on a working tune and adequate
hardware. Decoder availability is partial outside Chrome / Firefox
desktop (see §4).

## 2. Hardware encoder landscape (2026)

### NVIDIA NVENC AV1 (Ada Lovelace and newer)

- **Hardware:** RTX 40-series (Ada Lovelace), Hopper (H100), and
  newer. RTX 30-series (Ampere) and earlier do **not** have an AV1
  encoder. Cloud-hosted GPUs split similarly: A10/A100 → no; L4/L40,
  H100 → yes.
- **Latency:** Two relevant tunes —
  `NV_ENC_TUNING_INFO_LOW_LATENCY` (no B-frames, lookahead disabled)
  and `NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY` (strict in-order
  pipeline, minimal queuing)
  ([NVIDIA dev blog](https://developer.nvidia.com/blog/improving-video-quality-and-performance-with-av1-and-nvidia-ada-lovelace-architecture/)).
  Ultra-low-latency is the right starting point for our budget.
- **Quality:** NVENC AV1 reaches 8K 10-bit 60 fps in fixed-function
  hardware ([NVIDIA dev blog](https://developer.nvidia.com/blog/improving-video-quality-and-performance-with-av1-and-nvidia-ada-lovelace-architecture/)),
  ~40% bitrate savings over H.264. AV1 + intra-refresh works.
- **CBR:** Supported across NVENC implementations
  ([NVENC App Note](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-application-note/index.html)).
- **libwebrtc integration:** No upstream NVENC encoder factory.
  Selkies hooks in via GStreamer's `nvav1enc`; we'd need a libwebrtc
  `VideoEncoder` wrapper around the NVENC SDK directly. Not free,
  but well-trodden — NVIDIA ships sample code that maps cleanly
  onto libwebrtc's interface.

### Intel QSV AV1 (Arc / Xe)

- **Hardware:** Arc A-series (Alchemist), Battlemage discrete GPUs,
  and integrated Xe-HPG (Meteor Lake / Arrow Lake).
- **Latency / CBR:** QSV exposes the same low-delay knobs as Intel's
  other codecs; SVT-AV1's CBR rate-control conventions transfer
  cleanly to the hardware path
  ([SVT-AV1 rate-control appendix](https://github.com/BlueSwordM/SVT-AV1/blob/master/Docs/Appendix-Rate-Control.md)).
- **libwebrtc integration:** Accessible via FFmpeg's `av1_qsv` or
  Intel oneVPL. Requires a libwebrtc adapter — same shape as the
  NVENC case.
- **Cloud presence:** Less common than NVIDIA on managed cloud
  instance types. Treat as a "nice fallback" rather than a primary
  Phase 4 target.

### AMD AMF AV1 (RDNA 3+)

- **Hardware:** RX 7000-series (RDNA 3) introduced VCN 4.0 with AV1
  encode; RX 9000 (RDNA 4) continues it
  ([AMF wiki — AV1 Encoder](https://github.com/GPUOpen-LibrariesAndSDKs/AMF/wiki/AV1-Encoder)).
- **Rate control:** AMF AV1 inherits the AVC/HEVC rate-control
  surface — CBR is supported, FFmpeg's `av1_amf -rc cbr` is the
  canonical knob ([AMF Recommended FFmpeg Settings](https://github.com/GPUOpen-LibrariesAndSDKs/AMF/wiki/Recommended-FFmpeg-Encoder-Settings)).
- **Latency:** AMD's published quality vs latency tradeoffs lag
  NVENC AV1; in benches AMD HW encoders historically trail Nvidia
  and Intel ([Tom's Hardware](https://www.tomshardware.com/news/amd-intel-nvidia-video-encoding-performance-quality-tested)).
  Acceptable, not class-leading.
- **libwebrtc integration:** Same story — wrap AMF in a libwebrtc
  encoder. No upstream support.

### Apple Silicon

- **M3 / M4 (standard):** Hardware AV1 **decode only**. No encoder
  ([Bitmovin](https://bitmovin.com/blog/apple-av1-support/)).
- **M4 Ultra, M5 Pro/Max:** Hardware AV1 encode added
  ([VideoConverter Factory roundup](https://www.videoconverterfactory.com/multimedia-solution/apple-av1.html)).
- **Implications:** Our targets are Linux servers, not Macs, so
  Apple-side encoding is mostly relevant for client-side decode
  capability (covered in §4) — there is no realistic scenario where
  we encode on Apple hardware for this product. M5-class encode is
  noted only because it changes the **decoder** baseline downstream.

### Cloud GPU pricing footnote

For the AWS / GCP shapes we'd realistically deploy on:
- L4 (Ada Lovelace): widely available, AV1 encode-capable.
- L40, L40S: AV1-capable.
- A10, A10G: **no AV1 encode**. (Ampere generation.)
- H100: AV1-capable.
- T4: no AV1 encode. (Turing.)

Phase 4 hardware planning should pin a specific instance type
**before** any AV1 work — `g6.xlarge`-class L4 is the cheapest
AV1-capable shape on AWS today. (re-validate against current pricing
at planning time.)

## 3. Software encoders

### libaom-av1

- **Quality:** the reference encoder. Highest quality per bit at the
  slow end of the speed dial, the slowest practical realtime
  encoder at the fast end.
- **Realtime mode:** `--usage=realtime` enables `--cpu-used` 5–10 on
  recent libaom (FFmpeg caps at 8 in its wrapper)
  ([Phoronix on libaom 3.7](https://www.phoronix.com/news/Google-libaom-3.7-AV1)).
  Even at `--cpu-used=8` with `--tune=screen`, libaom does **not**
  hit our latency budget at 1080p on commodity cloud CPUs without
  a lot of tuning — typically 1.5–3× slower than SVT-AV1 at
  comparable quality.
- **Verdict:** **Don't pick libaom for our software path.** It's the
  reference, not the realtime workhorse.

### SVT-AV1

- **Speed:** Intel/Netflix's encoder; SVT-AV1 3.0 (Feb 2025) further
  closes the gap to NVENC at low presets
  ([Phoronix on SVT-AV1 3.0](https://www.phoronix.com/news/SVT-AV1-3.0-Released)).
  Materially faster than libaom in realtime mode
  ([Streaming Learning Center](https://streaminglearningcenter.com/blogs/good-news-av1-encoding-times-drop-to-near-reasonable-levels.html)).
- **Latency:** SVT-AV1's prediction structure is "optimized to
  produce low delay" with explicit CBR rate-control + low-delay
  options ([SVT-AV1 RC docs](https://github.com/BlueSwordM/SVT-AV1/blob/master/Docs/Appendix-Rate-Control.md)).
  v1.6.0 specifically improved low-delay tradeoffs for screen
  content.
- **Screen content:** SVT-AV1 has a screen-content path that
  overlaps libaom's `--tune=screen`; defaults are reasonable
  per the upstream tuning notes.
- **Verdict:** **The right software AV1 encoder for us if/when we
  go.** Same factory shape as VP9 / H.264, just a different
  internal wrapper.

### rav1e

- Rust encoder, focused on safety / correctness. Faster than libaom
  but slower than SVT-AV1 at equivalent quality. Less polished
  realtime story.
- **Verdict:** No reason to pick over SVT-AV1 for our use case.

## 4. WebRTC integration story

### libwebrtc state

- **AV1 has been in libwebrtc since April 2020**
  ([webrtcHacks on AV1 in Meet](https://webrtchacks.com/the-hidden-av1-gift-in-google-meet/)).
- **Encoder factory:** AV1 plugs into the same `VideoEncoderFactory`
  surface T19 already exposes. The work to add an AV1 backend is:
  1. New `VideoEncoder` subclass (the wrapper around SVT-AV1 / NVENC
     / etc.).
  2. Extending the RTP packetizer to support the AV1 RTP payload
     format (RFC 9329) — libwebrtc already has this.
  3. SDP fmtp + `EncodedImageCallback` plumbing.
  ([Meetecho on AV1 / H.265 in Janus](https://www.meetecho.com/blog/av1-h265-janus/))
- **HW codec support:** Improved AV1 hardware codec support landed
  in Chrome M120 ([Chrome Status feature 6206321818861568](https://chromestatus.com/feature/6206321818861568)).
  This is the client-side decoder path; it tells us our pinned
  Chromium release (T17 / docs/build/chromium-from-source.md) almost
  certainly already has the decoder side.

### SDP naming gotcha

Older libwebrtc versions advertise / accept AV1 under the name
`AV1X`; newer versions use `AV1`. Some endpoints negotiate one and
not the other. Track this when we wire AV1 into our factory —
`encoder_factory_stub.cc` should probably advertise both for a
transition window
([Janus Gateway issue #2844](https://github.com/meetecho/janus-gateway/issues/2844)).

### Encoder factory implications for T19

The contract holds; we're adding a new format string to
`GetSupportedFormats()` and a new branch in `CreateVideoEncoder`.
SVC modes (`L1T2`, etc.) are negotiated **outside** SDP — libwebrtc
asks us via `QueryCodecSupport(format, scalability_mode)`
([W3C webrtc-svc](https://w3c.github.io/webrtc-svc/)) — so AV1 is
where SVC actually starts mattering for us. Plan to revisit
`QueryCodecSupport` at the same time we add AV1.

### Browser decoder support

| Browser  | AV1 in WebRTC | Notes |
|----------|---------------|-------|
| Chrome / Edge ≥ 90    | Yes            | Hardware-accelerated since M120 on AV1-decode-capable hardware. |
| Firefox ≥ 116         | Yes            | DRM AV1 since Firefox 125. |
| Safari ≥ 17.4         | Hardware-gated | Only decodes AV1 on M3+ Macs / iPhone 15 Pro / iPhone 16+ ([Bitmovin](https://bitmovin.com/blog/apple-av1-support/)). |
| Safari on older iOS / Intel Macs | **No** | Ships H.264 only at the WebRTC layer. |
| Mobile Chrome on Android | Mostly yes     | Phone SoC dependent; Pixel 6+, Snapdragon 8 Gen 1+, etc. |

The Safari gap is the load-bearing constraint: a non-trivial fraction
of our users will be on Macs / iPhones without a hardware AV1
decoder. **If we ship AV1 we must ship VP9 + H.264 fallbacks for
exactly the same reason H.264 was added in T36.** No throwing the
old encoders out.

Sources for the support claim:
[caniuse — av1](https://caniuse.com/av1),
[MDN — Web video codec guide](https://developer.mozilla.org/en-US/docs/Web/Media/Guides/Formats/Video_codecs),
[antmedia — WebRTC browser support 2026](https://antmedia.io/webrtc-browser-support/).

## 5. Recommendation

For Phase 4, in this order:

1. **NVENC AV1 (Ada / Hopper / newer).** Highest quality at our
   latency budget on the cloud GPUs we'd actually rent. Plug in via
   a new `Av1NvencEncoder` behind T19's factory; reuse the
   `Vp9Encoder` shape. Gate availability on a runtime probe — if the
   `nvenc-av1` capability isn't present, we silently fall through.
2. **SVT-AV1 software.** Ship as a parallel software encoder so
   non-AV1 cloud instances still get AV1 if customer demand
   warrants. Use SVT-AV1 in CBR low-delay mode with its
   screen-content tune. Same `H264Encoder` / `Vp9Encoder` factory
   shape.
3. **AMD AMF AV1.** Add only if we have a strong reason to deploy on
   RX 7000-series GPUs (most cloud providers don't). Lowest
   priority.
4. **libaom realtime.** Skip. No reason to ship two software
   encoders, and SVT-AV1 dominates the realtime quadrant.

**Fallback policy.** Every session re-negotiates SDP. Our factory's
`GetSupportedFormats()` reports formats in preference order; for v2
the order becomes `AV1, VP9, H264, VP8?`. Clients without AV1 fall
back to VP9 (Chrome / Firefox / older Safari) or H.264 (every
remaining client). This is exactly the shape T36's H.264 was added
for; AV1 just slots in front.

**SVC.** Revisit `QueryCodecSupport(format, scalability_mode)` at
the same time. AV1 is the natural place to introduce a single
spatial layer + 2 temporal layers (`L1T2`) for cheap simulcast-shaped
adaptation. (re-validate after a benchmark on representative content.)

**What we still don't know — re-validate during Phase 4 kickoff.**

- Realistic NVENC AV1 latency at 1080p / 1440p / 4K on the chosen
  cloud GPU. Bench against our harness (T10/T11) before any
  factory-side work.
- Whether `AV1` vs `AV1X` naming has stabilized in the pinned
  Chromium branch's libwebrtc; the answer changes the SDP advertise
  list.
- Decoder share among real users — if our v1 telemetry shows >70%
  Chrome desktop, AV1 lands sooner; if it's >40% Safari mobile,
  AV1 is a quality-of-life win for a minority and the priority
  drops.

## Sources

- [CDNetworks — Compression Standards Explained](https://www.cdnetworks.com/blog/media-delivery/compression-standards-explained/)
- [Antmedia — Video Codecs Explained](https://antmedia.io/video-codecs-streaming-guide/)
- [NVIDIA Developer Blog — Improving Video Quality and Performance with AV1 and Ada Lovelace](https://developer.nvidia.com/blog/improving-video-quality-and-performance-with-av1-and-nvidia-ada-lovelace-architecture/)
- [NVENC Application Note](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-application-note/index.html)
- [Visionular — AV1 for WebRTC benchmarks](https://visionular.ai/av1-for-webrtc/)
- [SVT-AV1 Rate Control appendix](https://github.com/BlueSwordM/SVT-AV1/blob/master/Docs/Appendix-Rate-Control.md)
- [Phoronix — SVT-AV1 3.0 Released](https://www.phoronix.com/news/SVT-AV1-3.0-Released)
- [Phoronix — libaom 3.7 with screen-content gains](https://www.phoronix.com/news/Google-libaom-3.7-AV1)
- [Streaming Learning Center — AV1 encoding times](https://streaminglearningcenter.com/blogs/good-news-av1-encoding-times-drop-to-near-reasonable-levels.html)
- [AMF AV1 Encoder wiki](https://github.com/GPUOpen-LibrariesAndSDKs/AMF/wiki/AV1-Encoder)
- [AMF Recommended FFmpeg Encoder Settings](https://github.com/GPUOpen-LibrariesAndSDKs/AMF/wiki/Recommended-FFmpeg-Encoder-Settings)
- [Tom's Hardware — Video Encoding Tested](https://www.tomshardware.com/news/amd-intel-nvidia-video-encoding-performance-quality-tested)
- [Bitmovin — Apple AV1 Support](https://bitmovin.com/blog/apple-av1-support/)
- [VideoConverter Factory — Apple AV1 Support 2026](https://www.videoconverterfactory.com/multimedia-solution/apple-av1.html)
- [webrtcHacks — The Hidden AV1 Gift in Google Meet](https://webrtchacks.com/the-hidden-av1-gift-in-google-meet/)
- [Meetecho — AV1, H.265 and Janus](https://www.meetecho.com/blog/av1-h265-janus/)
- [Chrome Status — AV1 Encoder feature](https://chromestatus.com/feature/6206321818861568)
- [Janus Gateway issue #2844 — AV1 / AV1X SDP](https://github.com/meetecho/janus-gateway/issues/2844)
- [W3C — WebRTC SVC Extension](https://w3c.github.io/webrtc-svc/)
- [caniuse — AV1 video format](https://caniuse.com/av1)
- [MDN — Web video codec guide](https://developer.mozilla.org/en-US/docs/Web/Media/Guides/Formats/Video_codecs)
- [antmedia — WebRTC Browser Support 2026](https://antmedia.io/webrtc-browser-support/)

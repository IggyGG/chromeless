# HDR + wide color gamut research (Phase 4 candidate)

**Status:** research only. Phase 4 stretch — implementation gated on
real customer demand.
**Owner:** chromium-dev.
**Cross-references:**
`docs/research/av1-encoders.md` (T43 — encoder survey),
`docs/research/decoder-matrix.md` (T79 — per-client decoder
support; HDR support is even more constrained than the codec
support documented there),
`docs/research/rendering-matrix.md` (T91 — WebGL/WebGPU under
SwiftShader; HDR rendering needs hardware GPU per T95),
`docs/research/gpu-passthrough.md` (T95 — the upstream unblocker),
`docs/internal/nvenc-tuning-rationale.md` (T63 — explicit
"No 10-bit profile in v1" non-goal),
`docs/internal/vaapi-tuning-rationale.md` (T70 — same),
`docs/internal/svtav1-tuning-rationale.md` (T75 — same),
`docs/internal/vp9-tuning-rationale.md` (T35 — same),
`docs/capture/encoder-factory-catalog.md` (T97).

The project brief lists "HDR and wide color gamut" as a Phase 4
candidate. This doc says what it would mean for our pipeline,
where the pieces are today, and why the right answer is **defer
until a customer asks**.

## 1. What HDR means here

"HDR" in this pipeline means three things, all of which must agree
end-to-end (capture → encoder → transport → decoder → render →
display):

- **10-bit per channel** instead of 8-bit. Our entire encoder
  factory ships 8-bit 4:2:0 today; the per-codec rationale docs
  explicitly call this out as a non-goal (T35 §"What we do not
  do," T36 §same, T63 §same, T70 §same, T75 §same).
- **A non-linear transfer function carrying brightness above
  100 nits.** The two web-relevant ones are:
  - **HLG** (Hybrid Log-Gamma) — broadcast-friendly, backwards-
    compatible with SDR pipelines because the lower part of the
    curve is gamma-shaped.
  - **PQ** (Perceptual Quantizer, ST.2084) — peak-luminance-fixed
    (10,000 nits absolute). Used by HDR10 (PQ + static metadata)
    and HDR10+ (PQ + dynamic metadata).
  - **Dolby Vision** — proprietary; profile 5 layers metadata over
    HEVC, profile 8/10 over AV1. Not realistic for our pipeline —
    licensed, decoder-restricted, and not in any open codec
    factory.
- **Wide color gamut.** BT.2020 in place of BT.709. A larger
  triangle on the chromaticity diagram; saturated reds/greens that
  can't be expressed in BT.709 round-trip through BT.2020 cleanly.

For the rest of this doc, "HDR" = 10-bit + (HLG or PQ) + BT.2020.
Anything Dolby Vision is out of scope.

## 2. Codec support

Per-codec status, focused on what each ships in our factory:

### HEVC (T63 NVENC, T70 VAAPI; no SW path)

- Supports HDR10, HDR10+, Dolby Vision Profile 5 in the standard.
- NVENC HEVC: 10-bit + BT.2020 + HDR metadata SEI is fully
  supported on Turing+ (every NVENC-capable card our T43 footnote
  lists).
- VAAPI HEVC: 10-bit Main10 profile supported on Intel iHD (Skylake+)
  and Mesa radeonsi (RDNA1+).
- **WebRTC HEVC** itself shipped in Chrome 136 (March 2025) and
  Safari 18 ([Chrome blink-dev](https://chromestatus.com/feature/5153479456456704),
  [WebRTC HEVC compat checker](https://vdo.ninja/h265)). HDR over
  WebRTC HEVC is the most-mature codec path **as of 2026**, but
  Firefox doesn't have WebRTC HEVC at all yet.

### AV1 (T63 NVENC, T70 VAAPI, T75 SVT-AV1 SW)

- Supports HDR10 + HDR10+ in the standard. Future Dolby Vision
  Profile 10 is announced but not in stable encoders yet.
- NVENC AV1: 10-bit on Ada Lovelace+ only.
- VAAPI AV1: 10-bit on Intel Arc / AMD RDNA 3+ only.
- SVT-AV1: 10-bit Main10 profile fully supported (the encoder is
  best-known for HDR-VOD pipelines at Netflix). Realtime mode at
  preset M8 (our default per T75 §preset) handles 10-bit but with
  a measurable CPU bump (~1.4×).
- **AV1 HDR over WebRTC** is the cleanest standards story (RTP
  payload format already carries the necessary metadata) but
  decoder support is the bottleneck — see §3.

### VP9 (T35 SW, T70 VAAPI; no NVENC path)

- VP9 Profile 2 is 10-bit 4:2:0 (BT.2020). libvpx supports it.
- Tooling-wise the SW path is cleanest: a single config-flag flip
  (`g_input_bit_depth`, `g_bit_depth`) puts libvpx into 10-bit
  mode. Costs ~1.3× CPU vs 8-bit Profile 0.
- **VP9 HDR over WebRTC** is documented in RFC 7741 / VP9 RTP
  payload format spec; works in Chrome and Firefox in principle.
  Safari historically does not negotiate VP9 at all (per T79
  decoder matrix), let alone HDR VP9.

### H.264 (T36 SW, T63 NVENC, T70 VAAPI)

- **8-bit only.** H.264 has a High10 profile spec'd (10-bit), but
  no consumer decoder ships it — not Safari, not Chrome, not any
  iPhone — so the codec is effectively SDR-only in the wild. A
  full HDR pipeline cannot use H.264 at all.

### Decision shape

If we want HDR end-to-end in our cloud-browser pipeline we must
**negotiate HEVC, AV1, or VP9** — H.264 is off the table.

## 3. Browser HDR rendering support

Codec support is necessary but not sufficient: the receiving
browser must also drive the OS HDR pipeline + a real HDR display.
Per recent vendor docs:

- **Chrome / Edge.** PQ, HDR10 (PQ + static metadata), HLG all
  supported on Windows and macOS as of 2025; Linux Wayland
  support [merged in August 2025](https://www.phoronix.com/news/Chrome-Lands-Wayland-CM).
  X11 still SDR-only. **Chromium auto-tone-maps based on static
  metadata when present.**
- **Safari.** HDR on M1+ Macs and iPhone 12 Pro / Pro Max+ /
  iPad Pro M1+ that have HDR-capable displays. iPhone HDR
  rendering is the most-polished implementation in any mobile
  browser per current vendor reporting.
- **Firefox.** [Bug 1539685](https://bugzilla.mozilla.org/show_bug.cgi?id=1539685)
  is the long-running tracking issue. As of late 2025 HDR is
  behind flags, broken on most Linux configs, and not a
  realistic target.
- **HDR canvas + WebGPU.** Chrome shipped [HDR `<canvas>`](https://chromestatus.com/feature/5703719636172800)
  and is shipping [WebGPU extended-range HDR](https://chromestatus.com/feature/6196313866895360),
  but both require the page to opt in (`{ colorSpace: "rec2100-pq" }`)
  AND the OS-level HDR pipeline to be live. Streaming-the-page
  inside the cloud-browser doesn't naturally engage these — the
  page renders into our captured framebuffer regardless of the
  client display's HDR state.

The user-facing reality: **most users don't have HDR displays**.
Even the ones who do have HDR usually don't have it enabled (it's
a per-app and per-OS-display-mode toggle that defaults off on
Windows for desktop work). Our pipeline can do HDR perfectly and
the user still sees SDR.

## 4. Pipeline implications

What it would take to ship HDR in our pipeline, layer by layer:

### Capture (T55 FrameSinkVideoCapturer)

We currently configure the capturer with
`media::PIXEL_FORMAT_NV12` or `I420` (8-bit 4:2:0). Viz can produce
10-bit 4:2:0 (`PIXEL_FORMAT_P010` for 10-bit NV12 equivalent) but
only when:

- The renderer's compositor surface is itself 10-bit. Chromium
  pulls this from the OS HDR pipeline — we don't *have* an OS HDR
  pipeline under Xvfb in v1; we'd need either a real HDR display
  (impossible in headless containers) or a synthetic 10-bit
  framebuffer.
- The capturer is told to emit 10-bit (`SetFormat(PIXEL_FORMAT_P010)`)
  and the underlying renderbuffer can fulfil that.

This is the **biggest lift**. Our current `CloudBrowserFrameSinkCapturer`
(T55) plumbs `info->metadata` and color_space through, but the
end-to-end "page renders 10-bit → compositor surface is 10-bit →
capturer emits 10-bit" chain isn't wired. Re-validate against the
Phase-2 from-source build (T17) — Google's headless Chromium is
known to support a 10-bit synthetic surface, but we haven't tested
it.

### Encoder

The factory config gains one boolean:

```cpp
struct Config {
  // ... existing knobs ...

  // HDR (T98). When true, every encoder is configured for 10-bit
  // 4:2:0 + BT.2020 + the chosen transfer function. Forces the
  // factory to skip the H.264 path entirely (H.264 has no
  // production HDR decoder support — see hdr-wide-color.md §2).
  bool enable_hdr = false;
  enum class HdrTransfer { HLG, PQ };
  HdrTransfer hdr_transfer = HdrTransfer::PQ;
};
```

Per-codec wiring is small:

- **VP9 (T35).** `vpx_codec_enc_cfg_t.g_input_bit_depth = 10`,
  `g_bit_depth = VPX_BITS_10`, `g_profile = 2`. Tag color via
  `VP9E_SET_COLOR_SPACE = VPX_CS_BT_2020_NCL`,
  `VP9E_SET_COLOR_RANGE`, and the bitstream's
  `colour_description_present_flag` writes the SDP-side fmtp.
- **H.264 (T36).** Skipped — no HDR.
- **NVENC (T63).** `NV_ENC_BUFFER_FORMAT_YUV420_10BIT`,
  `pictureStruct` + `colorPrimaries` / `colorMatrix` /
  `transferCharacteristics` set per HDR10 / HLG. NVENC HDR10
  metadata SEI is enabled via `NV_ENC_PIC_PARAMS_HEVC.seiPayloadArray`
  (HEVC) or the equivalent AV1 OBU header.
- **VAAPI (T70).** `VAEncMiscParameterTypeHRD` + the codec-
  specific `seq_param_buffer` fields for color primaries /
  transfer / matrix coefficients. Intel iHD has the cleanest
  exposure; Mesa AMD requires Mesa 24+ for the metadata SEI path.
- **SVT-AV1 (T75).** `EbSvtAv1EncConfiguration.encoder_bit_depth = 10`,
  `mastering_display`, `content_light_level`, `color_range`,
  `color_primaries`, `transfer_characteristics`,
  `matrix_coefficients`. SVT-AV1 has the most-tested HDR config
  surface of any of our encoders.

Per-codec change is ~50-100 LOC each. The hard part is testing.

### Transport (libwebrtc)

WebRTC's RTP payload formats already carry HDR metadata:
- VP9: VP9 video color information in the bitstream itself, plus
  the `profile-level-id` SDP fmtp distinguishes Profile 2.
- HEVC: `RFC 7798` payload format carries everything including
  HDR10+ SEI; SDP fmtp `profile-id=2` for Main10.
- AV1: `RFC 9329` payload format; `profile=1` indicates Main10.

So **transport is mostly free** — the bits flow through libwebrtc's
existing packetizers as long as our encoder emits the right
bitstream and the SDP fmtp parameters are populated correctly.

Our `encoder_factory_stub.cc::GetSupportedFormats()` would gain
HDR-tagged variants when `Config::enable_hdr` is true:

```cpp
if (config_.enable_hdr) {
  webrtc::SdpVideoFormat hevc_main10("H265");
  hevc_main10.parameters["profile-id"] = "2";
  hevc_main10.parameters["tier-flag"] = "0";
  hevc_main10.parameters["level-id"] = "120";
  formats.push_back(std::move(hevc_main10));
  // ... AV1 Main10, VP9 Profile 2 similar.
}
```

### Decode + render on the client

Per §3, the client's HDR-rendering capability is the binding
constraint. The pipeline **must gracefully fall back** when the
client can't HDR — the negotiation does this naturally if the
client's `RTCRtpReceiver.getCapabilities("video")` doesn't list
the HDR-tagged variants (per T79's runtime-probing protocol).

## 5. Use cases for cloud-browser HDR

In rough order of who would actually want this:

1. **Watching HDR videos in the cloud browser** — YouTube HDR,
   Netflix HDR, Apple TV+ HDR. *The killer use case in principle,
   killed in practice by DRM.* Widevine L1 / FairPlay don't
   passthrough out of a streamed cloud Chromium; even if we
   plumbed HDR end-to-end, the page would refuse to play HDR DRM
   content. Lower-tier DRM (Widevine L3) plays at SDR only. So:
   **any HDR feature for video playback is half-broken at the
   DRM layer** regardless of our work.
2. **Photo editing UIs** — Lightroom Web, Photopea. Increasingly
   work in 10-bit color spaces; if the cloud browser is the
   user's editing surface, HDR matters. Niche but real.
3. **Design tools / future video editing UIs** — Frame.io,
   Descript, etc. Same shape as photo editing.
4. **HDR demos / showcase content** — bandwidth-intensive,
   marketing-driven, but real for vendors who want to demo their
   own HDR pipelines through our cloud browser.

The realistic v1+ assessment: **low priority**. The largest user
benefit (HDR video playback) is mostly killed by DRM constraints
that aren't ours to fix. The remaining use cases are niche enough
that the right move is "implement when a customer asks."

## 6. Recommendation

Phase 4 stretch — **implement when a real customer asks for it**.
When that happens, the work decomposes as follows:

| Item                                      | Effort                                          | Gating                              |
|-------------------------------------------|--------------------------------------------------|-------------------------------------|
| `Config::enable_hdr` flag + per-codec 10-bit toggles | ~2-3 days; small change in T35/T63/T70/T75. T36 explicitly skipped. | Once T17's build env runs.          |
| Capturer 10-bit (P010) emission           | ~1-2 weeks; requires Chromium 10-bit compositor configuration which is the genuinely uncertain part. | Phase 2 FrameSink integration must land first. |
| SDP fmtp HDR-tagged formats               | ~1 day; data-only change in `GetSupportedFormats`. | Encoder side must be ready first.   |
| Per-encoder HDR bench                     | ~3-5 days per codec; includes finding HDR test content + CPU/quality regression vs SDR. | After all of the above.             |
| Per-client decoder probing for HDR variants| Already designed (T79 §4 Phase-2 stretch). Reuse the data-channel runtime probe; no new design needed. | Can land independently any time.    |
| HDR rendering verification fixture        | ~1-2 days; mirror T91's WebGL fixture but probe `display-p3` / `rec2100-pq` canvas color spaces. | Independent.                        |

Defaults:

- `Config::enable_hdr = false`. Even after the work lands, ship
  off-by-default. Per-tenant flip via the same config plumbing
  T67 already does for namespacing.
- When enabled, the factory advertises BOTH the SDR variant
  (existing) AND the HDR variant for HEVC / VP9 / AV1. Clients
  that can't decode HDR variants negotiate down to the SDR
  variant; clients that can pick the HDR one.
- H.264 stays SDR-only forever. Document.

## 7. Cross-references

- **T43 (av1-encoders.md)** — codec landscape; AV1 HDR support
  matrix is in §3 there, and the cloud-GPU coverage matrix tells
  us which cloud GPUs can encode AV1 HDR (the Ada Lovelace+
  subset).
- **T63 (nvenc-tuning-rationale.md)**, **T70 (vaapi-tuning-rationale.md)**,
  **T75 (svtav1-tuning-rationale.md)**, **T35 (vp9-tuning-rationale.md)** —
  every per-codec doc has an explicit "No 10-bit profile" non-goal
  in §"What we do not do." Those are the lines that flip when this
  task moves from research to implementation.
- **T79 (decoder-matrix.md)** — per-client codec decoder support;
  HDR support is **strictly more constrained** than the matrix
  there. Safari's M1+ HDR and Chrome's Windows/macOS HDR are the
  realistic targets; everything else is SDR-fallback.
- **T91 (rendering-matrix.md)** — HDR rendering on the cloud
  side requires real GPU. Under v1's SwiftShader, HDR rendering
  is impossible; the page would render at 8-bit regardless of
  what the encoder is configured for.
- **T95 (gpu-passthrough.md)** — the upstream unblocker for
  capture-side HDR. Without GPU passthrough, Chromium has no path
  to a 10-bit compositor surface.
- **T97 (encoder-factory-catalog.md)** — when this lands, the
  catalog gains an "HDR" row in §1 capability table and an HDR
  column in §2 codec coverage matrix.

## 8. What this doc explicitly does NOT do

- **Doesn't commit code changes.** Every encoder ships 8-bit
  today; the per-codec docs say so explicitly. This is the
  research that tells us how big the change would be.
- **Doesn't pick HLG vs PQ.** Both have valid use cases (HLG for
  broadcast-shaped content; PQ for cinematic / static-image). The
  customer asking for HDR will tell us which.
- **Doesn't address Dolby Vision.** Licensing + decoder support
  rule it out for an open-source pipeline.
- **Doesn't address HDR DRM passthrough.** That's a Widevine /
  FairPlay problem that lives outside our codec factory and
  effectively kills the highest-value HDR use case
  (DRM-protected HDR video playback). If a customer needs HDR
  for non-DRM workflows (photo / design), the work below
  unblocks them; if they need HDR-DRM, no work on our side
  fixes that.

---

**Bottom line:** HDR is a **small encoder-config change** behind a
**massive capture-side configuration challenge** with a **very
narrow user benefit** (most users don't have HDR; the killer app
is DRM-blocked). Defer until a real customer asks; when they do,
the encoder layer is ~2-3 days of work and the capture layer is
the gate.

## Sources

- [WebRTC Browser Support 2026](https://antmedia.io/webrtc-browser-support/)
- [chromestatus — H.265 in WebRTC](https://chromestatus.com/feature/5153479456456704)
- [Chris Hiszpanski — Does your browser support WebRTC + HEVC?](https://www.hiszpanski.name/posts/is-webrtc-hevc-supported/)
- [vdo.ninja WebRTC H.265 / HEVC compat checker](https://vdo.ninja/h265)
- [chromestatus — HDR `<canvas>`](https://chromestatus.com/feature/5703719636172800)
- [chromestatus — WebGPU extended-range (HDR)](https://chromestatus.com/feature/6196313866895360)
- [W3C ColorWeb-CG — High Dynamic Range and Wide Gamut Color on the Web](https://w3c.github.io/ColorWeb-CG/)
- [Phoronix — Chromium Wayland color management lands](https://www.phoronix.com/news/Chrome-Lands-Wayland-CM)
- [Bugzilla 1539685 — Add HDR support to Gecko (Firefox)](https://bugzilla.mozilla.org/show_bug.cgi?id=1539685)
- [whatwg/html issue #9112 — Standardize rendering of PQ and HLG HDR image content](https://github.com/whatwg/html/issues/9112)

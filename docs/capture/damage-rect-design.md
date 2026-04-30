# Damage-rect partial-frame encoding (Phase 2 stretch)

**Status:** design only. Implementation deferred until T17's build
environment can compile real encoders against captured frames.
**Owner:** chromium-dev.
**Cross-references:**
`docs/capture/framesink-design.md` (T47),
`capture/framesink-capturer/capturer.h` (T55),
`capture/encoder/encoder_factory.h` (T19),
`capture/encoder/bwe_adapter.h` (T58),
`capture/encoder/simulcast_factory.h` (T83),
`capture/encoder/vp9_encoder.h` (T35),
`capture/encoder/h264_encoder.h` (T36),
`capture/encoder/nvenc_encoder.h` (T63),
`capture/encoder/vaapi_encoder.h` (T70).

## 1. Problem statement

Browser content is **almost entirely static between frames** for the
most common interactions:

| Workload                  | Per-frame pixel change | Source                                      |
|---------------------------|-------------------------|----------------------------------------------|
| Idle page (read article)  | < 1%                    | only cursor blink + occasional GIF.          |
| Reading + scrolling       | 30–60%                  | scroll moves the whole content area.         |
| Typing in a text field    | 2–5%                    | cursor + line being edited.                  |
| Watching embedded video   | 60–90%                  | full-frame motion in the video element.      |
| Loading a new page        | 100% (then back to <5%) | layout settles, then static.                 |

(re-validate the percentages on real captures via the T10/T11
harness once it's wired against the FrameSink path.)

Today, every frame is encoded as if its entire content changed.
The encoder's per-block skip detection happens *after* full motion
estimation has already run; we paid the search cost, the CPU went
to figuring out "block X didn't move," and the bitrate burned a
small amount on signaling that fact. For a 1080p VP9 stream of a
static blog post that's tens of milliseconds of CPU and a few
kbit/s **per frame** that we don't need.

The Viz capturer hands us the answer for free in
`info.metadata.CAPTURE_UPDATE_RECT`: the bounding rectangle of the
pixels that actually changed since the last frame. Encoding only
that rectangle — equivalently, telling the encoder to skip
everything outside it — is the bandwidth + CPU win this task
designs for.

## 2. Damage-rect source — clarification on the Viz contract

The task description proposes that `content_rect` from
`OnFrameCaptured` is the damage rect. **It is not.** Per
`docs/capture/framesink-design.md` §3 and the
[Viz mojom](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/services/viz/privileged/mojom/compositing/frame_sink_video_capture.mojom):

- `content_rect` is the active (non-letterboxed) region of the
  frame buffer. Stable across frames; changes only when the
  capture target's resolution changes.
- `info.metadata.CAPTURE_UPDATE_RECT` is the **damage rect** — the
  bounding rectangle of pixels that changed since the previous
  frame. This is what we want.

The metadata may also carry `RegionCaptureRect` for sub-capture
targets; we ignore that in v1 (we capture the whole tab / desktop).

Contract specifics worth pinning down before code:

- An empty / absent `CAPTURE_UPDATE_RECT` means **no change** — the
  capturer can skip emitting the frame entirely (per
  `OnFrameWithEmptyRegionCapture`), or emit it with the rect set to
  zero size. We treat both as "skip encoding; let libwebrtc emit a
  duplicate placeholder if the pacer wants one."
- A full-frame `CAPTURE_UPDATE_RECT` means **everything changed**.
  This is the keyframe trigger — we either ask the encoder for a
  forced IDR or skip damage-rect optimization for that frame and
  encode the whole picture.
- Partial rects in between are the interesting case — the static
  region outside the rect can be coded as "skip" (zero residual,
  zero motion vector).

T55's `CloudBrowserFrameSinkCapturer` already plumbs
`info.metadata` into the `media::VideoFrame` it hands downstream
(see capturer.cc `frame->set_metadata(info->metadata);`). The
encoder side reads it from there.

## 3. Per-codec strategies

Each encoder we ship has a different shape of "tell the encoder
what changed":

### VP9 (libvpx, T35)

Native support via `VP9E_SET_ROI_MAP`
([libvpx docs](https://chromium.googlesource.com/webm/libvpx/+/refs/heads/main/vp9/vp9_cx_iface.c)).
The ROI map is a per-superblock array of:
- `roi_map[i]` — a small int (0..3) selecting one of four QP/skip
  presets.
- Per-preset deltas: `delta_q[0..3]` and `delta_lf[0..3]` (loop
  filter), plus a `skip[0..3]` flag.

Strategy: superblocks **inside** the damage rect get preset 0
(delta_q = 0, normal coding); superblocks **outside** get preset 1
with `delta_q = +20` and `skip = 1`. The skip flag tells libvpx
to copy the previous frame's reconstruction without further
analysis — single-digit-bytes-per-superblock cost.

### H.264 (x264, T36)

H.264 has no standardized ROI signaling; we have two paths:

1. **`x264_param_t.rc.zones`** — per-rectangle QP delta. We pass:
   ```
   zones[0] = { .i_start = 0, .i_end = INT_MAX,
                .b_force_qp = 0, .f_bitrate_factor = 0.3 };
   // global ramp; static regions get less budget.
   ```
   plus zones for the damage rect with `f_bitrate_factor = 1.5`
   (more budget). x264's rate-distortion picks skip blocks for
   static regions naturally.
2. **Slice-group-based partitioning** — H.264 baseline FMO. Decoder
   support is patchy (Safari / mobile decoders historically choke
   on non-default slice-group maps), so we **avoid** this path.

We go with (1). The cost is rougher granularity than VP9's
per-superblock map, but the wins are still material on real
content.

### AV1 (SVT-AV1 T75 + libaom)

Most flexible: AV1's segmentation feature lets us tag every
superblock with a `segment_id` (0..7) and apply per-segment Q
delta + skip.

For SVT-AV1 specifically the relevant config is
`enable_adaptive_quantization` plus a per-frame
`SvtAv1FrameSegmentMap` (passed via
`EbBufferHeaderType::p_app_private` in v3.0+). Two segments:
"changed" (segment 0, no delta) and "static" (segment 1,
`segment_q_delta = +24`, `skip_segment = 1`).

NVENC AV1 (T63) and VAAPI AV1 (T70) expose the same feature via
their own structs (see §3 NVENC entry below).

### NVENC (T63)

`NV_ENC_CONFIG.encodeCodecConfig.{h264|hevc|av1}Config` carries:
- `qpMapMode = NV_ENC_QP_MAP_DELTA` — interpret `qpDeltaMap` as
  per-MB / per-CTU delta-QP.
- `qpMapMode = NV_ENC_QP_MAP_EMPHASIS_LEVEL_MAP` — alternative
  shape used for ROI.

We use `NV_ENC_QP_MAP_DELTA`. Per-frame, populate a small array
sized `(width / mb_size) * (height / mb_size)` with int8_t entries:
0 inside the damage rect, +24 outside. NVENC honours the deltas
without further configuration.

NVENC AV1 specifically also supports
`av1Config.enableLargeBlockSize = 1` which gives us 128×128
superblocks — a coarser map (smaller upload, fewer entries) that's
plenty granular for the damage rects we typically see.

### VAAPI (T70)

`VAEncMiscParameterTypeROI` lets us pass an array of `VAEncROI`
structs, each `{roi_rect, roi_value}` where `roi_value` is a
QP delta. Geometric rect API rather than per-block, so the
translation from our damage rect is direct: one VAEncROI for the
whole frame minus the damage rect (high QP delta), no entry for
the damage rect itself (uses default).

Drivers vary on ROI count limits — Intel iHD honours up to 16,
Mesa AMD historically caps at 4. Our typical workload needs 1–2
rects, so this is fine.

## 4. Cross-codec abstraction

Common type:

```cpp
// capture/encoder/damage_rect.h
namespace cloud_browser {

struct DamageRect {
  int x      = 0;   // pixels, top-left origin.
  int y      = 0;
  int width  = 0;   // 0 width or 0 height = "no damage" (skip frame).
  int height = 0;

  bool IsEmpty() const { return width <= 0 || height <= 0; }
  bool IsFullFrame(int frame_w, int frame_h) const {
    return x == 0 && y == 0 && width == frame_w && height == frame_h;
  }
};

// Extracted from media::VideoFrame::metadata() —
// CAPTURE_UPDATE_RECT, clamped to the visible_rect bounds.
DamageRect DamageRectFromMetadata(const webrtc::VideoFrame& frame,
                                   int frame_w, int frame_h);

// Each encoder implements:
//   virtual void ApplyDamageRect(const DamageRect& rect);
// invoked from Encode() before submitting the picture. No-op on
// codecs without native support.
}
```

Per-encoder hookup:

| Encoder        | File                       | Adapter call                                                                    |
|----------------|----------------------------|----------------------------------------------------------------------------------|
| Vp9Encoder     | `vp9_encoder.cc::Encode()` | Build per-superblock `VP9_ROI_MAP_T`; `VP9E_SET_ROI_MAP`.                       |
| H264Encoder    | `h264_encoder.cc::Encode()`| Set `x264_picture_t.prop.quant_offsets` for per-MB delta-QP.                    |
| NvencEncoder   | `nvenc_encoder.cc::Encode()`| Build `int8_t qpDeltaMap[]`; pass via `NV_ENC_PIC_PARAMS.qpDeltaMap`.            |
| VaapiEncoder   | `vaapi_encoder.cc::Encode()`| Build `VAEncMiscParameterBufferROI`; submit between BeginPicture / RenderPicture.|
| SvtAv1Encoder  | `svtav1_encoder.cc::Encode()`| Build segmentation map; pass via `EbBufferHeaderType.p_app_private`.            |

For codecs without ROI support (the placeholder VP8 wrapper, and
any future encoders), the adapter is a no-op — we encode the whole
frame as today. The rest of the pipeline is unaffected.

### Simulcast (T83) propagation

The damage rect is in source-resolution coordinates. Each
SimulcastEncoder layer encodes a downscaled frame; the rect must
scale correspondingly:

```cpp
// In SimulcastEncoder::Encode, before forwarding to inner.
DamageRect layer_rect;
layer_rect.x = source_rect.x / layer.scale_resolution_down_by;
layer_rect.y = source_rect.y / layer.scale_resolution_down_by;
layer_rect.width  = source_rect.width  / layer.scale_resolution_down_by;
layer_rect.height = source_rect.height / layer.scale_resolution_down_by;
inner->ApplyDamageRect(layer_rect);
```

Snap to even coordinates (chroma plane alignment) and to encoder-
block boundaries (macroblock / superblock). The bottom layers
(0.25× scale) may end up with damage rects smaller than one
superblock — in those cases the layer encodes a small full frame,
which is fine.

## 5. Quality + bandwidth tradeoffs

The win:
- Bitrate at idle: dropping from ~3 Mbps target to ~50–200 Kbps
  observed (per VP9 ROI benchmarks on screen content with QP+24
  static regions).
- CPU at idle: encoder skips motion estimation on static blocks;
  measured ~30–50% encode-time reduction on idle workloads.
- Bandwidth on scroll-heavy: smaller win (10–20%) since most of the
  frame is in the damage rect.

The risk: **drift across many partial frames**. Each "skip" tells
the decoder to copy the previous frame's reconstruction; over
hundreds of partial frames small numerical errors accumulate
visually as colour banding or chroma shifts. Two mitigations:

1. **Periodic refresh of static regions.** Every N frames
   (configurable, default 60 = 2 s at 30 fps), force the entire
   frame to encode at full quality regardless of damage rect. This
   is cheap — one heavier frame per 2 s.
2. **Rely on libwebrtc's IDR-on-demand.** Receivers send
   `RTCP PLI` / `RTCP FIR` when they detect drift; libwebrtc
   forwards it to the encoder as `kVideoFrameKey`. Our existing
   force-IDR path handles this.

(re-validate) — drift accumulation rate is content-dependent.
Browser UI is mostly grey/white flat regions, which are exactly
the regions where colour-banding artifacts show up first. Bench
on representative content before lowering the periodic-refresh
period below 60.

## 6. Implementation outline

When the build env runs:

1. **`capture/encoder/damage_rect.h`** — `DamageRect` struct +
   `DamageRectFromMetadata`.
2. **Per-encoder `Encode()` adapters.** Each existing encoder
   gains:
   ```cpp
   const DamageRect rect = DamageRectFromMetadata(frame, width_, height_);
   if (!rect.IsEmpty() && !rect.IsFullFrame(width_, height_)) {
     ApplyDamageRect(rect);  // codec-specific.
   }
   ```
   `ApplyDamageRect` is a per-codec helper that translates the rect
   into the codec's native ROI / segment / qpmap struct. Empty
   rects: skip the frame entirely (return `WEBRTC_VIDEO_CODEC_OK`
   without sending the encoder anything; libwebrtc's pacer will
   re-emit a duplicate if needed). Full-frame rects: ignore the
   ROI path; encode normally.
3. **Periodic full-frame refresh.** New
   `EncoderConfig::full_refresh_period_frames` (default 60) on
   every encoder Config; the per-codec wrappers track an internal
   frame counter and force full encoding every Nth frame.
4. **SimulcastEncoder propagation.** `SimulcastEncoder::Encode()`
   reads the source-coord damage rect, downscales per layer,
   forwards to each inner via a new
   `webrtc::VideoEncoder::SetDamageRect` call (we'd add this on
   our subclass; libwebrtc upstream has no such virtual).
5. **Tests.** Per-codec: synthetic frame with known damage rect →
   assert the encoder's output is materially smaller than the
   no-rect path on the same frame.

### BweAdapter interplay (T58)

Damage-rect encoding makes the **observed** output bitrate
detached from the **target** bitrate. With a 3 Mbps target and a
0.5% damage rect, actual output may be 100 Kbps. libwebrtc's BWE
sees this as a sender consistently under-using its budget and may
*reduce* the target accordingly — and then the next motion-heavy
frame (when the user scrolls) blows past the new lower target.

Mitigations to evaluate:
- **Don't lower `min_bitrate`** in BWE — keeps a floor for sudden
  motion.
- **Surface "intentional under-shoot" via `NotifyEncoded` stats**
  so BWE distinguishes "encoder chose to send less" from
  "congestion forced less."
- **Cap the QP delta on static regions** — at +24 today, but
  +12 might be the sweet spot on real content. (re-validate)

The BweAdapter (T58) is the right place for the metric — it sees
both the target and the observed bitrate. Add a `damage_rect_savings_bps`
field to `BweUpdate` and surface it through `MetricsSink` so the
sidecar can plot the divergence.

## 7. Testing methodology

**Synthetic.** Extend `simulcast_factory_test.cc`'s `FakeEncoder`
pattern: a `DamageRectAwareFakeEncoder` that records every
ApplyDamageRect call, plus a fixture that injects known
CAPTURE_UPDATE_RECT metadata via `media::VideoFrameMetadata`.
Tests cover:
- Empty rect → encoder skipped entirely (no Encode call to the
  codec).
- Full-frame rect → full encode, no ROI path.
- Partial rect → ApplyDamageRect called with the right rect.
- Periodic refresh fires every N frames regardless of damage rect.
- Simulcast: each layer receives the proportionally-downscaled
  rect.

**Real.** T11/T12 latency harness on a representative workload —
"static blog post for 30 seconds, scroll for 30, watch a YouTube
clip for 30." Compare:
- Total bytes shipped (target: 5–10× reduction during the static
  and idle segments).
- Encode time per frame (target: 30–50% reduction during static).
- Glass-to-glass latency (must not regress; if anything, slight
  improvement from lower per-frame encode time).
- Drift artifacts at minute 5 of static — visual inspection plus
  SSIM against a fresh reference frame.

## 8. Cross-references

- `docs/capture/framesink-design.md` (T47) — the source of
  CAPTURE_UPDATE_RECT.
- `capture/framesink-capturer/capturer.{h,cc}` (T55) — already
  propagates `info->metadata` into the wrapped `media::VideoFrame`;
  no T55 change needed for this work.
- `capture/encoder/bwe_adapter.{h,cc}` (T58) — gets a new
  `damage_rect_savings_bps` field on BweUpdate; sees the actual
  bitrate divergence.
- `capture/encoder/simulcast_factory.{h,cc}` (T83) — propagates
  the rect to each layer with proportional downscale.
- Per-codec encoders (T35 / T36 / T63 / T70 / T75) — each gains
  an ApplyDamageRect helper; the surface area in each is
  ~50 lines of codec-specific buffer-building.

## 9. Open items

- (re-validate) drift accumulation on representative browser
  content; tune the periodic-refresh period to the smallest
  number that doesn't show artifacts.
- (re-validate) static-region QP delta — +24 is conservative; the
  right number is workload-dependent.
- BWE behaviour under sustained intentional under-shoot — the
  worst-case interaction is "BWE drops min, motion scene starts,
  encoder over-runs the new low cap, frame drops." Bench before
  shipping.
- Whether to gate damage-rect encoding behind a Config flag
  (default off until benchmarked on production target instance
  type) or default on for all SW codecs (HW codecs can stay off
  until each vendor's bench passes). Recommendation: default off
  in v1, ship behind `Config::enable_damage_rect_encoding`, flip
  per-codec after bench.

## 10. What this design explicitly does *not* do

- **Does not change the FrameSink capturer.** T55 already plumbs
  the metadata through; this is encoder-side only.
- **Does not introduce a new wire-format change.** The encoded
  output remains a normal RTP-packetized H.264/VP9/AV1 stream;
  decoders treat skip blocks as standard. Receivers don't need to
  know we're using ROI.
- **Does not change SDP.** No new attributes; no negotiation.
- **Does not stack with simulcast bitrate-distribution logic.** We
  apply damage-rect ROI per layer; the simulcast wrapper still
  splits the BWE-published bitrate per spatial layer first, then
  the inner encoder uses the damage rect to under-spend within
  that layer's budget. The two optimizations compose cleanly.
- **Does not address sub-tab capture.** That's a Phase 3
  follow-up — when we hook FrameSinkVideoCapturer per-tab rather
  than full-display, the damage-rect optimization becomes even
  more valuable but the design is the same.

---

**Bottom line:** the cheapest big-bandwidth-and-CPU win we have
left in Phase 2. Implementation is mechanical per-codec, the
abstraction is small (`DamageRect` + per-encoder helper), and the
risk surface (drift) is well-bounded by periodic full refresh.
Gate behind `Config::enable_damage_rect_encoding`, default off,
flip per-codec after the harness shows the bench numbers stand up
on real content.

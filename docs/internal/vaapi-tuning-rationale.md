# VAAPI tuning rationale (Phase 4)

The VAAPI sibling of `nvenc-tuning-rationale.md` (T63),
`vp9-tuning-rationale.md` (T35), and `h264-tuning-rationale.md`
(T36). VAAPI is the cross-vendor Linux HW encode API; this is our
non-NVIDIA HW path.

**Status of the numbers below.** Same as every other encoder doc:
the build environment from T17 is not yet provisioned, and we
additionally need `libva` + a working `iHD` (Intel) / `radeonsi`
(AMD Mesa) driver path before any of these can be benched. Items
tagged **(re-validate)** await measurement on the chosen instance
type.

Cross-references:
- `capture/encoder/encoder_factory.h` — `prefer_vaapi_*` flow into
  `VaapiEncoderConfig`.
- `capture/encoder/vaapi_encoder.cc` — implementation.
- `capture/encoder/README.md` — runtime SW-vs-NVENC-vs-VAAPI
  selection rule.
- `docs/internal/encoder-factory-design.md` — broader factory design.
- `docs/research/av1-encoders.md` (T43) — codec-by-codec HW
  landscape.

## Vendor compatibility matrix

From T43 plus the codec/profile coverage VAAPI itself documents.
What VAAPI exposes ≠ what each driver implements; the matrix below
reflects driver reality on common Linux distros (Debian 12 / Ubuntu
24.04 baseline):

| GPU / driver path                    | H.264 | HEVC | AV1   | VP9 |
|--------------------------------------|-------|------|-------|-----|
| Intel iGPU (Skylake → Tiger Lake) — `iHD` | ✓     | ✓    | ✗     | ✓ |
| Intel Arc / Xe-HPG (Alchemist+) — `iHD`   | ✓     | ✓    | ✓     | ✓ |
| Intel iGPU (legacy Broadwell-) — `i965`   | ✓     | ✗    | ✗     | ✗ |
| AMD RDNA 1 / 2 — Mesa `radeonsi`     | ✓     | ✓    | ✗     | ✗ |
| AMD RDNA 3+ — Mesa `radeonsi`        | ✓     | ✓    | ✓ *   | ✗ |
| AMD APUs (Vega→Phoenix) — `radeonsi` | ✓     | ✓    | ✓ *   | ✗ |

\* AMD RDNA 3+ AV1 encode via VAAPI requires Mesa 24.0+ and is
documented but reported flakey under low-latency CBR — re-bench
once Mesa 25 lands. (AMF native is the alternate path and may be
the better landing on AMD specifically; not blocking v1.)

`NvencEncoder::ProbeAvailable(codec)` runs at process startup per
codec; the factory falls back to SW on a miss. Same pattern here:
**every host always has a working H.264 path** because the x264 SW
fallback is unconditional.

## Driver-name override

`VaapiEncoderConfig::driver_override` is empty by default; libva's
own `LIBVA_DRIVER_NAME` env var or autodetect picks the right
backend for the GPU. Operators set the override when:

- **Intel host with both i965 and iHD installed.** Force `"iHD"` —
  i965 is the legacy driver and lacks HEVC/AV1.
- **AMD host where the user wants the proprietary AMD driver.**
  Mesa's `radeonsi` is the recommended path; only override if a
  specific bug triggers a workaround.
- **Container deployment.** Set the override deterministically so
  the running driver is not host-libva's surprise pick. Production
  Phase-4 manifests should pin the driver.

## Configuration attributes (`vaCreateConfig`)

| Attribute                        | Value                          | Why                                                                                                              |
|----------------------------------|--------------------------------|-------------------------------------------------------------------------------------------------------------------|
| `VAConfigAttribRateControl`      | `VA_RC_CBR` (default), `VA_RC_VBR`, or `VA_RC_CQP` | CBR matches WebRTC pacer expectations. CQP is offline-only — never set in this pipeline.                          |
| `VAConfigAttribRTFormat`         | `VA_RT_FORMAT_YUV420`          | We always feed I420 from the libwebrtc frame; 10-bit is Phase 4.5 follow-up.                                      |
| `Profile`                        | per-codec: ConstrainedBaseline / Main / HEVCMain / AV1Profile0 / VP9Profile0 | Resolved from `codec_type` + `profile_level_id` (same parser as H264Encoder T36).                            |
| `Entrypoint`                     | prefer `VAEncEntrypointSliceLP`, fall back to `VAEncEntrypointSlice` | Low-power path is the latency win. Probe both at session init; fail closed if neither is supported. |

## Rate-control misc parameter (`VAEncMiscParameterRateControl`)

| Field                | Value                          | Why                                                                                          |
|----------------------|--------------------------------|-----------------------------------------------------------------------------------------------|
| `bits_per_second`    | `target_bitrate_bps`           | BWE-driven via SetRates().                                                                    |
| `target_percentage`  | `100`                          | "Hit the target." 95% for VBR with overshoot tolerance is offline-shaped; we don't want it.   |
| `window_size`        | `1000` ms                      | 1s VBV. Same as T36 / T63.                                                                    |
| `initial_qp`         | `26`                           | Mid-range start. Driver clamps within reason.                                                |
| `min_qp`             | `0` (driver picks)             | Don't preemptively floor; let driver pick from texture.                                       |
| `basic_unit_size`    | `0`                            | Driver-dependent — default is fine; only override on measured artefact regression.            |

## Framerate misc parameter (`VAEncMiscParameterFrameRate`)

`framerate` field set to `cfg.framerate`. Re-emitted on every
SetRates() so BWE-driven framerate scaling lands at the right
sequence boundary.

## Intra-refresh (`VAEncMiscParameterRIR`)

H.264 / HEVC only. AV1 / VP9 use cyclic-refresh AQ at the
slice / segment level instead — same shape as
`vp9-tuning-rationale.md` §VP9-specific.

| Field                          | Value                                  | Why                                              |
|--------------------------------|----------------------------------------|---------------------------------------------------|
| `rir_flags.bits.enable_rir_row`| `1`                                    | Row-based intra-refresh — the simplest path that works on every driver. Column-based is also valid; row was easier to reason about for our 16:9 captures. |
| `intra_insert_size`            | `(width / 16) / intra_refresh_period`  | Bands per frame; default tuning at 60-frame period for a 1920×1080 stream produces ~2 macroblock rows refreshed per frame. (re-validate)                       |
| `qp_delta_for_inserted_intra`  | `0`                                    | No QP delta on refreshed regions. Drivers vary on whether positive deltas help; default is the safe pick. |

If the driver doesn't support RIR (some legacy stacks), the buffer
creation fails; we drop the misc parameter and proceed without
intra-refresh. The encoder then relies on libwebrtc's IDR-on-demand
path for recovery — same fallback as the SW encoders if their
intra-refresh is disabled.

## Sequence parameter (codec-specific)

H.264 (`VAEncSequenceParameterBufferH264`):
- `level_idc = 31` (Level 3.1 — same default as T36).
- `intra_period = 0xFFFFFFFF` when `gop_size = -1`; pass-through
  otherwise.
- `intra_idr_period = intra_period` — keep IDR-on-demand only.
- `ip_period = 1` — IP only; **no B-frames**.
- `max_num_ref_frames = 1` — single reference. Phase 4.5 may bump
  this for SVC / temporal layering.
- `time_scale = 90000`, `num_units_in_tick = 90000 / framerate` —
  RTP video clock.

HEVC (`VAEncSequenceParameterBufferHEVC`): mirror shape with
`general_level_idc = 93` (HEVC Level 3.1).

AV1 (`VAEncSequenceParameterBufferAV1`): `intra_period`,
`ip_period = 1`, geometry. Driver fills profile / sequence header
defaults; we override only the latency-critical bits.

VP9 (`VAEncSequenceParameterBufferVP9`): single-reference,
`intra_period`, geometry.

## Encode-call (per-frame)

- `force_idr` from libwebrtc → codec-specific picture flag:
  - H.264: `pic_fields.bits.idr_pic_flag = 1`.
  - HEVC: `pic_fields.bits.idr_pic_flag = 1`.
  - AV1: `picture_flags.bits.frame_type = KEY_FRAME (0)`.
  - VP9: `pic_flags.bits.frame_type = 0`.
- Slice / tile-group params are codec-specific; v1 ships a single
  slice / tile-group covering the whole frame. Multi-slice latency
  parallelism is a Phase 4.5 follow-up.

## Coded-buffer drainage (`VACodedBufferSegment`)

VAAPI returns the bitstream as a singly-linked list of
`VACodedBufferSegment`s. We walk the chain, copying each segment
into the `EncodedImage` buffer. Drivers may emit the whole frame in
one segment or split it; we don't depend on the count.

`VA_CODED_BUF_STATUS_BAD_BITSTREAM` triggers a hard fail — this is
typically a driver / hardware bug surfacing, not something we can
recover from. libwebrtc will trigger an IDR on the next frame.

## What we explicitly do *not* do

- **No multi-slice / tile-row encoding in v1.** Single slice / tile
  group per frame. Phase 4.5 follow-up: H.264 multi-slice for
  latency parallelism on Intel iGPUs.
- **No 10-bit profile.** 8-bit 4:2:0 only.
- **No DMA-buf import from the FrameSink capturer (T55).** Today we
  `vaDeriveImage` + memcpy I420 into the surface. Phase 4.5 swaps
  this for `VASurfaceAttribExternalBuffers` import of the GMB-backed
  frame. Same Phase 4.5 follow-up as NVENC's CUDA interop story.
- **No B-frames.** Latency budget.
- **No SVC.** AV1 VAAPI exposes spatial/temporal layers; T58's
  BweAdapter is the natural place for the layer-bitrate split when
  we ship simulcast.
- **No HDR.** Phase 4 stretch.

## Observability / future work

Once the build + libva + a real iGPU/dGPU are available:

1. Bench encode latency p50/p95/p99 vs the SW path and the NVENC
   path. VAAPI on Intel Arc is competitive with NVENC on Ada
   Lovelace per T43; verify on the chosen instance type.
2. Confirm intra-refresh actually rotates the picture within
   `intra_refresh_period_frames` by parsing the slice headers.
3. Wire `vaQueryVendorString` into the metrics sidecar so per-vendor
   p99s are observable in production. The `EncoderInfo
   ::implementation_name` already encodes the vendor — sidecar just
   has to parse it.
4. Add the `VASurfaceAttribExternalBuffers` zero-copy path once the
   FrameSink capturer is producing GMB-backed frames in production.

## Selkies cross-reference

Selkies' `vah264enc` / `vah265enc` GStreamer elements are the
prior art here. They wrap libva similarly, with the GStreamer
property surface as their config seam. Our intent is the same —
zero-lag, no reorder, intra-refresh — but expressed against the
libwebrtc `VideoEncoderFactory` instead of `webrtcbin`. See
`docs/prior-art/selkies.md`.

# NVENC tuning rationale (Phase 4)

This is the HW sibling of `vp9-tuning-rationale.md` (T35) and
`h264-tuning-rationale.md` (T36). Same shape: every knob set in
`capture/encoder/nvenc_encoder.cc` is justified below.

NVENC is the Phase 4 priority per `docs/research/av1-encoders.md`
(T43). H.264 / HEVC are the immediate landable HW codecs; AV1 is the
quality win, gated on Ada Lovelace+ hardware (L4 / L40 / H100 — see
T43 §2 cloud-GPU footnote).

**Status of the numbers below.** Same as the SW siblings: the build
environment from T17 is not yet provisioned, and we additionally need
the NVIDIA Video Codec SDK + a real GPU before any of these can be
benched. Items tagged **(re-validate)** await measurement on the
chosen instance type.

Cross-references:
- `capture/encoder/encoder_factory.h` — `prefer_nvenc_*` flow into
  `NvencEncoderConfig`.
- `capture/encoder/nvenc_encoder.cc` — implementation.
- `capture/encoder/README.md` — runtime SW-vs-HW selection rule.
- `docs/internal/encoder-factory-design.md` — broader factory design.
- `docs/research/av1-encoders.md` (T43) — codec-by-codec HW landscape.

## Session setup (`NV_ENC_INITIALIZE_PARAMS`)

| Field             | Value                                       | Why                                                                                                                          |
|-------------------|---------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------|
| `encodeGUID`      | one of NV_ENC_CODEC_{H264,HEVC,AV1}_GUID     | Resolved from `Config::codec_type` at session init.                                                                         |
| `presetGUID`      | "P3" (NV_ENC_PRESET_P3_GUID) on Ada Lovelace+; "LL_HQ" (NV_ENC_PRESET_LOW_LATENCY_HQ_GUID) on legacy hardware | "P3" is the modern balanced low-latency preset; "LL_HQ" is the legacy fallback for pre-Ada GPUs. (re-validate exact preset choice) |
| `tuningInfo`      | `NV_ENC_TUNING_INFO_LOW_LATENCY` (default)   | Disables B-frames + lookahead. `ULTRA_LOW_LATENCY` is the next step if measurement shows we still miss budget.              |
| `frameRateNum/Den`| `framerate / 1`                              | CFR matching the source.                                                                                                     |
| `enablePTD`       | 1                                            | Picture-type decision at encode time. Must be 1 for `forceIDR`-on-demand to work.                                            |

## Rate-control (`NV_ENC_RC_PARAMS`)

| Field                   | Value                          | Why                                                                                                                |
|-------------------------|--------------------------------|---------------------------------------------------------------------------------------------------------------------|
| `rateControlMode`       | `NV_ENC_PARAMS_RC_CBR`         | Same reason as VP9 / x264: WebRTC pacer prefers stable target. CBR matches BWE shape.                              |
| `averageBitRate`        | `target_bitrate_bps`           | Driven by `SetRates()` from libwebrtc BWE.                                                                          |
| `maxBitRate`            | `= averageBitRate`             | Cap peaks at target — same VBV-shape as x264 (T36).                                                                  |
| `vbvBufferSize`         | `= averageBitRate` (~1 s)      | 1-second buffer model.                                                                                              |
| `vbvInitialDelay`       | `= vbvBufferSize`              | Match. Smaller delays the pacer.                                                                                    |
| `enableMinQP`/`MaxQP`   | `0`                            | NVENC's RC is solid; we don't clamp QP at the encoder. Could revisit at Phase 4.5 if quality oscillates.            |

## Codec-specific intra-refresh (every codec)

The shared structural choice across H.264 / HEVC / AV1: **intra-refresh
ON, IDR-only-on-demand**. Same intent as VP9 cyclic-refresh and
x264 `b_intra_refresh` — each frame refreshes a sliding band; over
`intra_refresh_period_frames` the entire picture is refreshed without
ever inserting a full IDR.

**H.264** (`encodeCodecConfig.h264Config`):
- `idrPeriod = NVENC_INFINITE_GOPLENGTH` — no automatic IDR.
- `repeatSPSPPS = 1` — SPS/PPS in front of every IDR-equivalent.
  Same semantics as `b_repeat_headers = 1` in x264 (T36).
- `enableIntraRefresh = 1`, `intraRefreshPeriod = N`,
  `intraRefreshCnt = max(1, N/4)`.
- `outputFramePackingSEI = 0`, `outputBufferingPeriodSEI = 0`,
  `outputPictureTimingSEI = 0` — we do not use SEI as a side
  channel.

**HEVC** (`encodeCodecConfig.hevcConfig`): same shape with
`idrPeriod`, `repeatSPSPPS`, `enableIntraRefresh`, etc.
HEVC `intraRefreshCnt` follows the same formula.

**AV1** (`encodeCodecConfig.av1Config`): `idrPeriod`,
`repeatSeqHdr`, `enableIntraRefresh`, `intraRefreshPeriod`,
`intraRefreshCnt` — symmetric to H.264 / HEVC. AV1 specifics
(film-grain, screen-content tune knobs) are open follow-ups.

## GOP / B-frames

```
gopLength = NVENC_INFINITE_GOPLENGTH
frameIntervalP = 1
```

- **Infinite GOP**: combined with intra-refresh, this means the
  driver never inserts a hidden IDR on a fixed schedule. Every IDR
  is something we asked for via libwebrtc's keyframe-on-demand path.
- **`frameIntervalP = 1`**: no B-frames. NVENC respects this both
  for H.264 and HEVC.

## Encode-call (`NV_ENC_PIC_PARAMS`)

- `pictureType = NV_ENC_PIC_TYPE_IDR` only when libwebrtc passes
  `kVideoFrameKey` in `frame_types`; otherwise `NV_ENC_PIC_TYPE_P`.
- `encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR` paired with the IDR
  pictureType — both are needed for NVENC to honor the request
  reliably across SDK versions.
- `inputTimeStamp` = our PTS in NVENC ticks — currently the input
  frame counter; revisit if the bitstream-side timestamps need to
  match capture-end-time (T55 `info.metadata.CAPTURE_END_TIME`).

## Buffer pool

Pool depth defaults to **4** in `Impl::Initialize`. NVENC requires
the consumer to keep input + output buffers alive for the duration
of in-flight frames; 4 is a safe over-provision for our 30 fps
realtime path. Bump if observability shows the encoder waiting on a
buffer (`nvEncEncodePicture` returns `NV_ENC_ERR_NEED_MORE_INPUT`
in steady state).

## What we explicitly do *not* do

- **No simulcast / SVC inside one NVENC session.** AV1 NVENC has SVC
  modes; we ignore them in v1. Multi-layer is the BweAdapter's
  Phase-2-stretch problem.
- **No interleaved B-frames.** Per the latency budget. NVENC's
  high-quality presets enable B-frames by default; we explicitly
  disable.
- **No 10-bit profile in v1.** 8-bit 4:2:0 only. Same reason as VP9.
- **No driver-side resolution scaling.** Resolution changes come
  from the capture side (T55 `ChangeTarget` + reconfigure); the
  encoder Init re-runs on a Encode that observes a different
  width/height.
- **No GPU-memory-buffer fast path yet.** Today we lock + memcpy
  I420 into the NVENC input buffer. Phase 4.5 follow-up: CUDA
  interop with the capture-side GMB so we keep frames on the GPU
  end-to-end. Holds latency on the table; revisit when the harness
  shows it.

## Codec availability matrix (cloud GPU; from T43)

| Cloud GPU       | H.264 NVENC | HEVC NVENC | AV1 NVENC |
|-----------------|-------------|------------|-----------|
| T4 (Turing)     | ✓           | ✓          | **✗**     |
| A10 (Ampere)    | ✓           | ✓          | **✗**     |
| L4 (Ada)        | ✓           | ✓          | ✓         |
| L40 (Ada)       | ✓           | ✓          | ✓         |
| H100 (Hopper)   | ✓           | ✓          | ✓         |

`NvencEncoder::ProbeAvailable(codec)` runs at process startup and
caches per-codec; the factory falls back to the SW wrapper on a
miss. **Every host always has a working H.264 path** because the
x264 SW fallback is unconditional.

## Observability / future work

When the build + SDK + GPU are all available:

1. Bench encode latency p50 / p95 / p99 vs the SW path. The win
   over `H264Encoder::ultrafast/zerolatency` should be ~2–5 ms
   per frame at 1080p. (re-validate)
2. Confirm CBR target tracking within ±5% on representative
   content. NVENC's RC is documented as tighter than libvpx —
   smaller window than the T35 / T36 ±10% target.
3. Switch one default to ULTRA_LOW_LATENCY tuning and re-measure;
   adopt if the latency win is real and the bitrate cost is small.
4. Profile the lock+memcpy input path; once the GMB interop story
   lands, that whole step disappears.

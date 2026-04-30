# VP9 (libvpx) tuning rationale

This document explains every encoder knob set in
`capture/encoder/vp9_encoder.cc`. It is paired with `T35` (the
encoder implementation task) and `T19` (the factory contract).

**Status of the numbers below.** The build environment from T17 is not
yet provisioned — these settings are chosen against libvpx and
libwebrtc documentation, the experience encoded in the
`PROJECT_BRIEF.md` Phase 1 plan (zero-latency tuning, no B-frames,
intra-refresh, small GOPs), and our prior-art read of Selkies
(`docs/prior-art/selkies.md`). Every claim marked **(re-validate)**
needs to be measured once the harness (T10/T11) is wired against a
real built encoder. Do not treat the magnitudes as final.

Cross-references:
- `capture/encoder/encoder_factory.h` — how `Config::zero_latency`,
  `disable_b_frames`, `intra_refresh`, `gop_length_frames` flow into
  `Vp9EncoderConfig`.
- `capture/encoder/vp9_encoder.cc` — implementation.
- `docs/internal/encoder-factory-design.md` — the broader design.

## `vpx_codec_enc_cfg_t` knobs (set in `InitEncode`)

| Field                | Value                          | Why                                                                                                                              |
|----------------------|--------------------------------|-----------------------------------------------------------------------------------------------------------------------------------|
| `g_w` / `g_h`        | from `VideoCodec`              | Caller decides geometry; we honor it.                                                                                            |
| `g_timebase`         | 1 / 90000                      | RTP video clock. Lets pts arithmetic stay integral across the stack.                                                              |
| `g_lag_in_frames`    | `0`                            | **Zero buffer-ahead.** Encoder cannot reorder, cannot insert altrefs that depend on future frames. The single biggest latency knob in libvpx. |
| `g_pass`             | `VPX_RC_ONE_PASS`              | We have one shot per frame — no two-pass rate control in realtime.                                                                |
| `rc_end_usage`       | `VPX_CBR`                      | Constant bitrate. WebRTC's BWE wants stable target; CBR avoids VBR's burstiness that fights the pacer.                            |
| `rc_target_bitrate`  | `cfg.target_bitrate_bps / 1000`| libvpx wants kbps. SetRates() updates this on BWE feedback.                                                                       |
| `rc_min_quantizer`   | `2`                            | Floor — let libvpx use very low QP at low motion (still text on screen).                                                          |
| `rc_max_quantizer`   | `56`                           | Ceiling — keeps quality from collapsing under congestion. (re-validate)                                                          |
| `rc_under/overshoot_pct` | `50` / `50`                | Symmetric tolerance. Default 100/100 lets libvpx swing too far for our latency budget. (re-validate)                              |
| `rc_buf_initial_sz`  | `500`                          | Buffer model in ms. Keep small — we do not want a buffer-fullness aware encoder hesitating.                                       |
| `rc_buf_optimal_sz`  | `600`                          | Slight headroom over initial.                                                                                                     |
| `rc_buf_sz`          | `1000`                         | Hard cap; 1 s is enough.                                                                                                          |
| `rc_dropframe_thresh`| `0`                            | **Never let the encoder drop frames.** libwebrtc's pacer is the right place for backpressure; if the encoder drops, we lose visibility into why. |
| `rc_resize_allowed`  | `0`                            | Resolution changes are an upper-layer (capture or simulcast) decision, not the encoder's business in v1.                          |
| `kf_mode`            | `VPX_KF_DISABLED`              | **No automatic keyframes.** We rely on intra-refresh + libwebrtc's IDR-on-demand.                                                  |
| `kf_min/max_dist`    | `keyframe_interval` if positive | Escape hatch for tests / odd configs. Default config keeps it disabled.                                                          |
| `g_threads`          | `max(1, num_cores - 1)`        | Leaves one core for the rest of the streamer (capture, networking, supervisord). Avoids the encoder pegging every core under load.|
| `g_error_resilient`  | `VPX_ERROR_RESILIENT_DEFAULT`  | Tolerate single-packet loss without artifact cascades. Costs ~1–2% bitrate; cheap.                                                |

## VP9-specific `vpx_codec_control` calls (`ApplyVp9Controls`)

| Control                                 | Value              | Why                                                                                                                                   |
|-----------------------------------------|--------------------|----------------------------------------------------------------------------------------------------------------------------------------|
| `VP8E_SET_CPUUSED`                      | `8`                | **Fastest** preset. libvpx VP9 is famously slow at the default setting; 8 hits realtime on commodity CPUs at the cost of bitrate efficiency. (re-validate exact value) |
| `VP9E_SET_TUNE_CONTENT`                 | `VP9E_CONTENT_SCREEN` | We are encoding a Chromium tab — text, UI, occasional video. Screen tune trades motion-estimation effort for crisp text; correct for our workload. |
| `VP9E_SET_AQ_MODE`                      | `3` (cyclic refresh) | This is **how libvpx VP9 implements intra-refresh.** Each frame refreshes a sliding band; over `intra_refresh_period_frames`, every macroblock is refreshed without ever inserting a full IDR. |
| `VP9E_SET_AQ_MODE_CYCLIC_REFRESH_PERIOD`| `intra_refresh_period_frames` | The period over which the cyclic refresh sweeps the frame. We default to 60 frames (~2 s at 30 fps); shorter wastes bitrate, longer slows recovery. |
| `VP9E_SET_DELTAQ_MODE`                  | `0`                | Disable adaptive QP modes that conflict with cyclic refresh. (re-validate)                                                            |
| `VP9E_SET_NOISE_SENSITIVITY`            | `0`                | Off. Temporal denoiser hurts text edges and adds latency.                                                                              |
| `VP9E_SET_FRAME_PARALLEL_DECODING`      | `0`                | Don't optimize for parallel decoding — the client is a browser, not a multi-core decoder rig, and the constraint blocks some efficiency wins. |
| `VP9E_SET_ROW_MT`                       | `1`                | Row-level multithreading on the encoder side. Cheap parallelism win.                                                                   |

## Encode-call flags

- `flags = VPX_EFLAG_FORCE_KF` when libwebrtc passes
  `VideoFrameType::kVideoFrameKey`. This is how libwebrtc's
  IDR-on-demand mechanism reaches us; honor it unconditionally.
- `deadline = VPX_DL_REALTIME` always. Other deadlines exist
  (`VPX_DL_GOOD_QUALITY`, `VPX_DL_BEST_QUALITY`); they trade latency
  for compression, which is the wrong way for us.

## What we explicitly do *not* do

- **No simulcast / SVC.** v1 emits a single layer. Spatial / temporal
  scalability arrives if/when the harness shows we need it.
- **No `VPX_VBR` / VPX_Q` rate control.** CBR only.
- **No long-term reference frames.** They reduce keyframe size but
  add memory and latency we don't need.
- **No 10-bit profile.** VP9 profile 0 (8-bit, 4:2:0) is the only
  profile we target; libwebrtc and most browser decoders are happiest
  there.

## Observability / future work

When the build is up:

1. Wire the encoder under our latency harness (T10/T11) and record
   p50/p95/p99 encode time per frame. **(re-validate)** the
   `cpu-used = 8` choice on the chosen instance type — if encode
   p99 fits under ~6 ms with cpu-used = 7, drop one.
2. Confirm intra-refresh actually rotates the full frame within
   `intra_refresh_period_frames` by parsing `EncodedImage` data and
   counting refreshed superblocks. (Not load-bearing, but a useful
   sanity check before Phase 2.)
3. Wire `SetRates` calls into our metrics; we want a histogram of
   target-bitrate updates per minute to spot BWE pathologies.
4. Decide whether to enable simulcast / SVC layers.  Default for v1
   is **no**.

## Selkies cross-reference

Selkies' `x264enc` / VP9 invocation lives in their `media_pipeline.py`
(see `docs/prior-art/selkies.md`). They tune similar latency knobs
but ride GStreamer's `webrtcbin`, so their tuning is per-element
(`x264enc tune=zerolatency`, etc.) rather than per-libvpx-control.
The intent is the same — zero lag, no reorder, intra-refresh — but
the seam is different. Our seam is libwebrtc's `VideoEncoderFactory`,
not GStreamer.

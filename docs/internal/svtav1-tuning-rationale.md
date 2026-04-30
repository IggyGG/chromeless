# SVT-AV1 (libSvtAv1Enc) tuning rationale

The SVT-AV1 sibling of `vp9-tuning-rationale.md` (T35),
`h264-tuning-rationale.md` (T36), `nvenc-tuning-rationale.md`
(T63), and `vaapi-tuning-rationale.md` (T70). SVT-AV1 is the
software AV1 encoder — the fallback when no AV1 hardware path is
available.

**Why SVT-AV1 and not libaom.** Per `docs/research/av1-encoders.md`
(T43): libaom realtime mode (`--cpu-used=8`) does not hit our
latency budget at 1080p on commodity cloud CPUs; SVT-AV1 at preset
M8 with the screen-content tune does. SVT-AV1 3.0 (Feb 2025) further
closed the speed gap to NVENC at low presets. We chose SVT-AV1 once,
in T43; this doc captures the per-knob detail.

**Status of the numbers below.** Same as every other encoder doc:
T17's build environment is not yet provisioned, and we additionally
need libSvtAv1Enc linked + a working AVX2/AVX-512 host before any
of these can be benched. Items tagged **(re-validate)** await
measurement on the chosen instance type.

Cross-references:
- `capture/encoder/encoder_factory.h` — `enable_svt_av1` flow into
  `SvtAv1EncoderConfig`.
- `capture/encoder/svtav1_encoder.cc` — implementation.
- `docs/internal/encoder-factory-design.md` — broader factory design.
- `docs/research/av1-encoders.md` (T43) — codec landscape and the
  recommendation that landed here.

## Preset (`enc_mode`)

SVT-AV1 exposes presets `M0` (slowest / highest quality) through
`M13` (fastest / worst quality). We default to **M8**.

| Preset | Speed (rel. M0) | Quality (BD-rate, rel. M5) | Realtime at 1080p? |
|--------|-----------------|----------------------------|---------------------|
| M0–M4  | very slow       | best                       | offline only        |
| M5     | reference       | reference                  | barely, on big CPUs |
| M6     | ~2× M5          | -3% to -5%                 | mostly              |
| M7     | ~4× M5          | -8% to -12%                | yes on x86_64       |
| **M8** | **~8× M5**      | **-15% to -20%**           | **yes (default)**   |
| M9     | ~12× M5         | -25% to -30%               | always              |
| M10–M13| 16–30× M5       | severe                     | always              |

(numbers are approximate, taken from upstream + Streaming Learning
Center benches. (re-validate) on our target instance type.)

We pick M8 because:
- It hits realtime 30 fps at 1080p on a 16-vCPU `c5.4xlarge`-equivalent
  with headroom.
- Its quality is competitive with VP9 cpu-used=8 + screen-content tune
  per T43 §3.
- M9–M13 cut quality faster than they buy us latency. The latency
  delta from M8 to M9 is single-digit milliseconds; the quality cost
  is double-digit BD-rate. Wrong trade for us.

## Latency-critical knobs

| SVT-AV1 field             | Value | Why                                                                                                    |
|---------------------------|-------|---------------------------------------------------------------------------------------------------------|
| `hierarchical_levels`     | `0`   | **No B-frames.** Same intent as VP9 / x264 / NVENC siblings — B-frames force the encoder to wait.       |
| `pred_structure`          | `0`   | low-delay-P. Disables the random-access prediction shape that introduces reorder.                       |
| `look_ahead_distance`     | `0`   | No lookahead. Single biggest knob after `hierarchical_levels`.                                          |
| `enable_tpl_la`           | `0`   | Temporal-pyramid lookahead off — buys quality, costs frames of latency.                                 |
| `scene_change_detection`  | `0`   | Disable SCD. Browser content has no cuts; SCD only inserts spurious IDRs that trash bitrate budget.    |
| `intra_period_length`     | `-1`  | Infinite GOP. We rely on libwebrtc's IDR-on-demand (forced via `frame_types` in Encode) plus open-GOP   |
|                           |       | recovery. Same shape as the other encoders' `keyframe_interval = -1`.                                  |
| `intra_refresh_type`      | `2`   | Open-GOP key frame. Closed-GOP IDR (1) would reset reference state and cost the next P frame extra QP. |

## Rate control

| Field                          | Value                          | Why                                                                                              |
|--------------------------------|--------------------------------|---------------------------------------------------------------------------------------------------|
| `rate_control_mode`            | `2`                             | CBR. Same shape as VP9 / x264 / NVENC / VAAPI — matches WebRTC pacer.                            |
| `target_bit_rate`              | `target_bitrate_bps`           | Driven by SetRates() from libwebrtc BWE.                                                          |
| `max_bit_rate`                 | `= target`                     | Cap peaks at target.                                                                              |
| `maximum_buffer_size_ms`       | `1000`                         | 1 s VBV. Matches the rest of the encoder family.                                                  |
| `starting_buffer_level_ms`     | `1000`                         | Start fully-buffered.                                                                             |
| `optimal_buffer_level_ms`      | `600`                          | Slight headroom; SVT-AV1 docs recommend ~60% of buffer size.                                     |
| `max_qp_allowed` / `min_qp_allowed` | `63 / 1`                  | Full QP range — let SVT-AV1's RC find its own clamp. Tightening only worth it if we observe the   |
|                                |                                | encoder hitting QP 63 sustained, which means the bitrate is too low.                             |

## Tune

| Tune  | When to pick it                                                                  |
|-------|-----------------------------------------------------------------------------------|
| `VQ`  | Default. Visual quality / SSIMULACRA-shaped loss. Best for browser UI + text.    |
| `PSNR`| Pick if comparing against VP9/H.264 PSNR benchmarks; not a production choice.    |
| `SSIM`| Slight quality bump on flat regions; we don't have measurements yet. Phase 4.5.   |

## Screen content

`screen_content_mode = 1` is **always on** for our use case. T43 §1
documents 10–19% BD-rate gain for screen content over the default
tune at speeds 6–10 — every cloud-browser session is screen content
all the way down.

## Threading

`logical_processors`:
- `cfg.num_threads > 0` → use that.
- `cfg.num_threads = 0` (default) → `max(1, hardware_concurrency() - 1)`.

Same shape as VP9 / x264 — leave a core for capture / signaling /
supervisord. SVT-AV1's own default is "all cores"; we override to
keep one in reserve.

`tile_rows = 0`, `tile_columns = 0` — single tile. Multi-tile parallelism
is a Phase 4.5 follow-up; it can buy encode latency at the cost of
some quality. Decoder-side parallelism it would also buy doesn't
benefit our single-PC client.

## What we explicitly do *not* do

- **No simulcast / SVC in v1.** Single layer.
- **No 10-bit / Profile 2.** 8-bit 4:2:0 only.
- **No film-grain synthesis.** AV1's killer feature for film
  encodes; useless for browser content.
- **No multi-tile encoding.** Phase 4.5.
- **No HDR.** Phase 4 stretch (everywhere).

## Observability / future work

When the build + libSvtAv1Enc + a real CPU are available:

1. Bench encode latency p50/p95/p99 at 1080p on the chosen instance
   type. The gate to ship SVT-AV1 by default is "fits the brief's
   latency budget and quality is at least equal to VP9 SW at the
   same bitrate." (re-validate)
2. **(re-validate)** the M8 choice. If M7 fits the latency budget on
   the production instance type, take it — quality win is real.
3. Investigate tile-rows on multi-core hosts. Wider tiles mean
   parallel encode at the cost of compression. (Phase 4.5)
4. Wire SVT-AV1's per-frame stats (`encoder_speed`, `slot_count`)
   into the metrics sidecar — more granular than the BweAdapter's
   per-update view.

## Selkies cross-reference

Selkies' encoder list (T3 / `docs/prior-art/selkies.md`) does not
include SVT-AV1; their AV1 path is `nvav1enc` (NVENC) only. Our
SVT-AV1 wrapper closes the SW AV1 gap they leave open.

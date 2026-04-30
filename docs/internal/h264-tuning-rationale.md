# H.264 (x264) tuning rationale

This is the H.264 sibling of `vp9-tuning-rationale.md`. Same shape:
every knob set in `capture/encoder/h264_encoder.cc` is justified
below.

H.264 exists in v1 specifically because **Safari prefers it** in SDP
negotiation. Chrome / Firefox should land on VP9 first (per the
factory's preference order in `encoder_factory_stub.cc`); H.264 is the
fallback that keeps the cloud browser usable on macOS / iOS clients.

**Status of the numbers below.** As with VP9, the build environment
from T17 isn't yet provisioned, so these settings are chosen against
the documented x264 + libwebrtc headers and our prior-art reading of
Selkies' `x264enc tune=zerolatency` baseline (see
`docs/prior-art/selkies.md`). Items tagged **(re-validate)** are
budgets that must be re-measured with the harness once the encoder
actually compiles.

Cross-references:
- `capture/encoder/encoder_factory.h` — how `Config::zero_latency`,
  `disable_b_frames`, `intra_refresh`, `gop_length_frames` flow into
  `H264EncoderConfig`.
- `capture/encoder/h264_encoder.cc` — implementation.
- `docs/internal/encoder-factory-design.md` — broader factory design.
- `docs/internal/vp9-tuning-rationale.md` — the VP9 sibling.

## Preset / tune at init

```cpp
x264_param_default_preset(&params, "ultrafast", "zerolatency");
```

- **`ultrafast`** is the only preset that hits realtime at 1080p on
  commodity cloud CPUs. Anything slower (`superfast`, `veryfast`,
  etc.) buys quality at the cost of encode time, which is not the
  trade we want for v1. (re-validate)
- **`zerolatency`** disables lookahead (`i_sync_lookahead = 0`),
  disables B-frames (`i_bframe = 0`), and enables sliced threading.
  We re-set those individually below to lock the contract — do not
  rely on the tune profile alone, since x264 has changed its tune
  defaults across versions.

## Knobs we override on top of `ultrafast / zerolatency`

| Field                    | Value                  | Why                                                                                                                                  |
|--------------------------|------------------------|---------------------------------------------------------------------------------------------------------------------------------------|
| `i_width` / `i_height`   | from `VideoCodec`      | Caller decides geometry.                                                                                                              |
| `i_csp`                  | `X264_CSP_I420`        | We always feed I420 from the libwebrtc frame.                                                                                         |
| `i_fps_num` / `i_fps_den`| `framerate / 1`        | CFR. Gets re-set by `SetRates`.                                                                                                       |
| `b_vfr_input`            | `0`                    | We feed CFR; tell x264 not to reinterpret pts.                                                                                        |
| `i_timebase_num/den`     | `1 / 90000`            | RTP video clock. pts arithmetic stays integral.                                                                                       |
| `i_log_level`            | `X264_LOG_ERROR`       | x264 will otherwise spam stdout with per-frame stats.                                                                                 |
| `i_keyint_max`           | `INT_MAX`              | **No automatic IDRs.** We rely on intra-refresh + libwebrtc's IDR-on-demand. x264 won't insert a periodic keyframe.                   |
| `i_keyint_min`           | `INT_MAX`              | Lower bound matched to upper bound — prevents x264 from inserting "scene cut" IDRs.                                                  |
| `b_intra_refresh`        | `1`                    | **The latency win.** x264 inserts intra-coded slices that march across the frame; over `intra_refresh_period_frames` the entire picture is refreshed without a single full IDR. |
| `i_bframe`               | `0`                    | **No B-frames.** B-frames force the encoder to wait for "future" frames before emitting their predecessors. Death for latency.        |
| `i_sync_lookahead`       | `0`                    | No lookahead worker thread. (`zerolatency` already sets this; we re-assert.)                                                          |
| `rc.i_lookahead`         | `0`                    | Same.                                                                                                                                  |
| `rc.i_rc_method`         | `X264_RC_ABR`          | Average-bitrate target. Closest x264 has to libvpx's CBR for our purposes; libwebrtc's BWE drives the target via `SetRates`.          |
| `rc.i_bitrate`           | `target_bitrate_bps/1000` | x264 takes kbps.                                                                                                                       |
| `rc.i_vbv_max_bitrate`   | `= rc.i_bitrate`       | Cap peaks at target — keeps the pacer honest.                                                                                         |
| `rc.i_vbv_buffer_size`   | `= rc.i_bitrate` (~1s) | 1 s of buffer model. Smaller and rate control gets jittery; larger and we let the encoder run hot. (re-validate)                       |
| `b_repeat_headers`       | `1`                    | Emit SPS/PPS in front of every IDR-equivalent. Necessary for mid-stream join / single-loss recovery.                                  |
| `b_annexb`               | `1`                    | Annex-B start codes. libwebrtc's H.264 packetizer expects them; the alternative ("AVCC") would require an extra unwrap step.          |
| `i_threads`              | `max(1, num_cores - 1)`| Same shape as VP9 — leave a core for capture / signaling / supervisord.                                                              |
| `i_lookahead_threads`    | `1`                    | Minimal; we have no lookahead anyway.                                                                                                 |
| `b_sliced_threads`       | `1`                    | **Slice-based threading.** Frame-pipelined threading (the default at higher core counts) costs ~1 frame of lag per worker. Slice threading parallelizes within a frame. |

## Profile / level

We map our SDP `profile-level-id` (e.g. `42e01f` = Constrained
Baseline 3.1) to x264's `--profile` and `--level-idc` strings via the
small parser in the .cc. Supported short list:

| profile-level-id | x264 profile  | level_idc |
|------------------|--------------|-----------|
| `42e01f`         | `baseline`   | 31        |
| `4d401f`, `4d001f` | `main`     | 31        |
| `640c1f`, `64001f` | `high`     | 31        |

Default is **Constrained Baseline 3.1** — the broadest browser
compat. Main is included because some Safari builds prefer it; High
is offered for completeness, but realistically we never want it in
v1 (it allows the decoder to reorder, which our pipeline does not
benefit from).

## Encode-call behaviour

- `pic_in.i_type = X264_TYPE_AUTO` for normal frames; `X264_TYPE_IDR`
  when libwebrtc requests a keyframe (this is how IDR-on-demand
  reaches us).
- The output buffer x264 returns is **already Annex-B-formatted** and
  contiguous across all NALs of the frame. We wrap it with
  `EncodedImageBuffer::Create` in one allocation and hand it to
  libwebrtc.
- `pic_out_.b_keyframe` tells us whether the resulting frame should
  carry `VideoFrameType::kVideoFrameKey` — the codec's own opinion of
  whether this is an IDR, which under intra-refresh is "yes only on
  IDRs we explicitly forced."

## What we explicitly do *not* do

- **No simulcast / SVC.** Same as VP9 — single layer in v1.
- **No CABAC tuning beyond the preset.** `ultrafast` picks CAVLC over
  CABAC for compatibility / speed; we accept that.
- **No psycho-visual rate-distortion.** The corresponding tweaks
  (`--psy-rd`, `--aq-mode 2`, etc.) are off in `ultrafast` and we
  leave them off.
- **No 10-bit / High10 profile.** 8-bit only.
- **No SEI insertion beyond what x264 emits by default.** We don't
  use SEI as a side channel.

## Observability / future work

When the build is up:

1. Wire under the latency harness (T10/T11) and record encode time
   per frame. **(re-validate)** the `ultrafast` choice — if we have
   p99 budget at `superfast`, take it.
2. Validate intra-refresh actually marches the full frame within
   `intra_refresh_period_frames` by parsing the slice headers. (Same
   sanity check as VP9.)
3. Measure observed bitrate vs target on representative content; the
   test in `h264_encoder_test.cc` accepts ±50% for synthetic frames
   (the noise floor), but the operational target is **±10%** on real
   browser content. (re-validate)
4. Confirm SPS/PPS reach the wire on every IDR-equivalent (the
   `b_repeat_headers = 1` claim).

## Selkies cross-reference

Selkies wires the equivalent through GStreamer's `x264enc element`
with `tune=zerolatency speed-preset=ultrafast bframes=0` — very
similar shape, different seam. The only knob we set here that the
GStreamer element doesn't surface as cleanly is `b_sliced_threads`,
which `x264enc` sets implicitly via `threads=auto`. Worth double-
checking once we can run an apples-to-apples bench between the two
pipelines.

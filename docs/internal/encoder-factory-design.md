# Encoder factory — design rationale

**Status:** Phase 1 prep. The interface in `capture/encoder/` is
scaffolding; the design captured here is what the scaffolding is
designed to support.
**Cross-references:** `capture/encoder/encoder_factory.h`,
`docs/build/chromium-from-source.md` (T17),
`docs/prior-art/selkies.md` (T3),
`docs/capture/path-of-least-resistance.md` (T15),
`PROJECT_BRIEF.md` Phase 1 / Phase 4.

## Why a custom factory at all

libwebrtc ships a default `VideoEncoderFactory`
(`webrtc::CreateBuiltinVideoEncoderFactory()`) that does most of what
we need for a generic VC use case. We override it because:

1. **Latency tuning is per-encoder and per-platform.** The defaults are
   tuned for video-conferencing where 100–200 ms of buffered B-frames
   is acceptable. Our budget is LAN <100 ms / regional <200 ms
   end-to-end, which means the encoder cannot afford reorder buffers
   or large GOPs. We need to set those knobs ourselves.
2. **HW encoder slot.** Phase 4 introduces NVENC and VAAPI. The
   built-in factory can't reach those without our patch series anyway,
   so we may as well own the factory from day 1.
3. **Single point for the encoder-selection knob.** Selkies (T3) showed
   that a one-flag-picks-an-encoder model is a real ergonomic win.
   Owning the factory lets us implement it the same way.
4. **Bandwidth-estimator hookup.** libwebrtc's pacer / GCC sends
   target-bitrate updates to the encoder via `VideoEncoder::SetRates()`.
   We want to gate those, log them, and re-shape them (e.g., couple
   bitrate steps to resolution changes via simulcast layers we don't
   yet emit). That all lives below the factory boundary.

## The contract

Documented in detail in `capture/encoder/encoder_factory.h`. Summary
of the load-bearing pieces:

- **`GetSupportedFormats()`** — preference-ordered list. Phase 1 lists
  VP9 first, then H.264 (Constrained Baseline 3.1 for max
  compatibility). VP8 is gated behind `Config::enable_vp8` and stays
  off in v1.
- **`CreateVideoEncoder(format)`** — returns the encoder for a format
  we said we support, or `nullptr` for one we did not. Never returns a
  non-functional encoder (asserts/log-and-die if construction fails).
- **`QueryCodecSupport()`** — reports `is_power_efficient = false` for
  every format in v1 (everything is SW). Phase 4 flips this for the HW
  formats so libwebrtc's encoder selector prefers them automatically.

## Low-latency tuning, mapped to encoder knobs

The `Config` struct on `CloudBrowserVideoEncoderFactory` carries the
intent. Each encoder wrapper applies the config to its own knobs:

| Config field        | libvpx VP9 (`vpx_codec_enc_cfg_t`)              | x264 (`x264_param_t`)                   | NVENC (Phase 4)                  | VAAPI (Phase 4)                |
|---------------------|--------------------------------------------------|------------------------------------------|----------------------------------|--------------------------------|
| `zero_latency`      | `g_pass = VPX_RC_ONE_PASS`, `g_lag_in_frames = 0`, `rc_end_usage = VPX_CBR`, `kf_mode = VPX_KF_DISABLED` | `b_intra_refresh`, `i_sync_lookahead = 0`, `tune = "zerolatency"` | `NV_ENC_TUNING_INFO_LOW_LATENCY` | `low_power = 1`, `rc_mode = CBR` |
| `disable_b_frames`  | implicit in `g_lag_in_frames = 0`                | `i_bframe = 0`                           | `frameIntervalP = 1`             | implicit, no B-frames in low-power |
| `intra_refresh`     | error-resilience flags + manual keyframe gating  | `b_intra_refresh = 1`                    | enable intra refresh             | enable intra refresh           |
| `gop_length_frames` | `kf_max_dist`                                    | `i_keyint_max`                           | `gopLength`                      | `intra_period`                 |

Each wrapper translates the `Config` once at `InitEncode()` time and
does not reinterpret it again. Runtime ABR re-tunes bitrate and
framerate (see below) but not these structural knobs.

## Bandwidth-estimator hookup

libwebrtc calls `VideoEncoder::SetRates(RateControlParameters)` on
every BWE update. The wrappers will:

1. Log the request (target bitrate per spatial+temporal layer, target
   framerate).
2. Clamp it against a `Config`-supplied min/max (Phase 1: a wide
   clamp; tighten as we learn).
3. Map the bitrate onto the encoder's CBR target. For SW VP9 that is
   `rc_target_bitrate`; for x264 that is `rc.i_bitrate`. NVENC/VAAPI
   take it via their respective rate-control structs.
4. If the new framerate is materially lower, optionally drop input
   frames at the wrapper level rather than hand them to the encoder
   (a Phase 2 optimization; v1 just passes them through).

There is no plan in v1 to override libwebrtc's congestion controller.
We let GCC drive and we obey.

## HW encoder slots (Phase 4)

NVENC and VAAPI plug in by:

1. Adding `enable_nvenc` / `enable_vaapi` fields to `Config`.
2. Adding their formats to `GetSupportedFormats()`. Order: HW first,
   SW fallback after, so the SDP advertises HW preference.
3. Adding branches to `CreateVideoEncoder()` returning the HW
   wrapper.
4. Overriding `QueryCodecSupport()` to set `is_power_efficient = true`
   for HW-backed `(format, scalability_mode)` pairs. libwebrtc's
   encoder selector uses this to prefer HW under load.

There is no class-hierarchy split between SW and HW wrappers — both
implement `webrtc::VideoEncoder` directly. We resist the urge to add
an internal "encoder strategy" abstraction; the libwebrtc seam is
already the abstraction.

## What we explicitly do *not* do here

- **No simulcast / SVC in v1.** `GetSupportedFormats()` returns
  flat formats; scalability modes come in Phase 2 once measurement
  shows we need them.
- **No software AV1 in v1.** libaom is too slow at our latency budget;
  AV1 returns via NVENC/AV1 in Phase 4.
- **No per-tab encoder configuration.** The factory is process-wide.
  Per-tab capture is a Phase 2 capture-side concern, handled in the
  Viz hook, not here.
- **No `webrtcbin`-style direct-pipeline approach.** Selkies' transport
  is GStreamer's `webrtcbin` (T3). We deliberately do not adopt that
  — owning the encoder factory inside libwebrtc is precisely the
  benefit we keep by *not* going down the GStreamer route.

## Open questions

- **VP9 vs H.264 default.** VP9 wins on quality-per-bitrate but H.264
  has broader hardware decode support on weak clients. v1 prefers VP9;
  revisit after we have field data.
- **Profile-level-id for H.264.** We default to `42e01f` (Constrained
  Baseline 3.1). Some Safari versions need different param strings;
  T20-era client work will tell us if we need to advertise multiple.
- **Where does process supervision live?** If an encoder asserts at
  runtime (the stub's current behavior), libwebrtc's worker thread
  takes the assert. supervisord then restarts Chromium. That is fine
  for the stub but we should think about graceful recovery once real
  encoders ship.

## Reference libwebrtc revision

See `docs/build/chromium-from-source.md` §6 — we will pin to
`refs/branch-heads/NNNN`. The interface in `encoder_factory.h` is
authored against libwebrtc as of recent main; if the pinned branch
diverges, fix the header at roll time, not opportunistically.

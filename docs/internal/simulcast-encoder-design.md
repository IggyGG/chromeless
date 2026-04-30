# Encoder-side simulcast — design rationale

**Status:** Phase 1 stretch / Phase 2 default. Code at
`capture/encoder/simulcast_factory.{h,cc}` (T83).
**Cross-references:** `capture/encoder/encoder_factory.h` (T19),
`capture/encoder/bwe_adapter.h` (T58 — per-layer rate distribution
arrives via SetRates), `docs/protocols/simulcast.md` (T77 — SDP /
client side), `capture/streamer-page/streamer.js` (T77 — ladder
parsing on the streamer side).

T77 shipped the SDP and streamer-side simulcast plumbing. This task
is the encoder side: per-layer encoder instances behind one
`webrtc::VideoEncoder` facade, so libwebrtc treats us as a single
"simulcast-capable" encoder.

## 1. Why N inner encoders, not native multi-layer

Three different codecs we currently ship, three different reasons
to **not** use the codec's native simulcast / scalability mode:

- **libvpx VP9.** The native simulcast knob (`cfg.ts_number_layers`)
  is **temporal scalability only** — same resolution, different
  frame rates. Our T77 ladder is **spatial** (1080p / 540p / 270p);
  libvpx's native path doesn't help.
- **x264 H.264.** No native simulcast at all. Resolution scaling is
  always upstream.
- **SVT-AV1.** Has scalability modes but they target offline /
  random-access encoding, not realtime. SVT-AV1 in low-delay-P /
  no-lookahead mode (our config from T75) does not expose multi-
  spatial-layer output.
- **NVENC + VAAPI.** Both have multi-resolution output APIs, but
  the shapes are vendor-specific (NVENC's "single-NVENC-session
  multi-output" requires careful NVCUVID glue; VAAPI exposes it via
  `VAEncMiscParameterTemporalLayerStructure` which is again temporal
  not spatial).

The unifying pattern that works for every codec: **wrap N single-
layer encoders behind a `webrtc::VideoEncoder` facade.** This is
what libwebrtc's own
[`SimulcastEncoderAdapter`](https://chromium.googlesource.com/external/webrtc/+/refs/heads/main/media/engine/simulcast_encoder_adapter.cc)
does for the same reasons. We don't reuse that class directly
because we want explicit control over per-layer config injection,
metric tagging, and the inner-encoder builder closure (lets us
pick HW/SW per layer in Phase 4.5).

## 2. Architecture

```
                                 +-------------------+
   webrtc::                      | libwebrtc         |
   VideoEncoderFactory           | VideoStreamEncoder|
   (CloudBrowserVideo            +---------+---------+
    EncoderFactory T19)                    |
              |                              | (one frame in,
              | CreateVideoEncoder           |  N RTP streams out)
              v                              v
   +-----------------------+        +--------------------+
   | SimulcastEncoder T83  |◄-------| InitEncode(codec)  |
   | (this file)           |        | Encode(frame)      |
   |                       |        | SetRates(alloc)    |
   |  ┌──────────────────┐ |        +---------+----------+
   |  │ LayerState[0]    │─┼──> downscale →  ┌────────────┐
   |  │  inner = Vp9Enc  │ |                  │ libyuv     │
   |  │  scratch (1080p) │ |                  │ I420Scale  │
   |  └──────────────────┘ |                  └────────────┘
   |  ┌──────────────────┐ |
   |  │ LayerState[1]    │─┼──> downscale 0.5×, encode @ 540p
   |  │  inner = Vp9Enc  │ |
   |  └──────────────────┘ |
   |  ┌──────────────────┐ |
   |  │ LayerState[2]    │─┼──> downscale 0.25×, encode @ 270p15
   |  │  inner = Vp9Enc  │ |
   |  └──────────────────┘ |
   |                       |
   |  TaggingCallback ─── stamps SpatialIndex / SimulcastIndex
   |  per layer before ──> outer EncodedImageCallback
   |  forwarding          (libwebrtc RTP packetizer routes by SSRC)
   +-----------------------+
```

## 3. Layer ladder source

Two construction paths:

1. **Explicit ladder** (constructor takes `std::vector<SimulcastLayer>`)
   — used by tests that want to pin a specific ladder. The factory
   doesn't take this path in production.
2. **Deferred ladder** (constructor takes only the inner-encoder
   builder + codec name) — the factory's path. Layers are derived
   inside `InitEncode` from the negotiated `webrtc::VideoCodec
   ::simulcastStream[]`. Single-stream SDP degenerates to a single-
   layer ladder, the wrapper has near-zero overhead in that case
   (one extra dispatch per Encode + one extra dispatch per
   SetRates), and the single-layer fast path stays intact.

`SimulcastLayersFromCodec` reverses libwebrtc's lowest→highest
indexing so our `states_[0]` is always the top resolution. This
matches the T77 streamer-side `layer0` / `layer1` / `layer2`
naming convention.

## 4. Per-layer downscale

We use `libyuv::I420Scale` with `kFilterBox`. Box-filter is the
right choice for screen content (browser UI + text), where bilinear
produces visible aliasing on high-frequency edges. The scratch
`I420Buffer` per layer is allocated once at `InitEncode`; the
downscale runs against it on every frame.

The top layer (`scale_resolution_down_by == 1`) skips the downscale
and forwards the source `I420BufferInterface` directly. Zero
allocations on the top-layer hot path.

Phase 4.5 follow-up: when the FrameSink capturer (T55) starts
producing GMB-backed `media::VideoFrame`s, we should swap the libyuv
path for the GPU's hardware scaler (NVENC's CUDA scaler / VAAPI's
VPP). Lower CPU, lower latency, no readback. Holding off until the
zero-copy capture path is in production — `vp9_encoder.cc` and
`h264_encoder.cc` already memcpy I420 from the source frame into
their own buffers, so there's no GMB upstream of us today.

## 5. Bitrate distribution

`SetRates` receives a `webrtc::VideoBitrateAllocation` from the
BweAdapter (T58). libwebrtc indexes spatial layer 0 = lowest; we
map it onto our `states_[N-1]` (the bottom layer in our top-first
ordering). For each layer we sum across temporal layers (we don't
emit temporal scalability in v1 simulcast) and forward.

Fallback: if BWE hasn't yet decided this layer's bitrate
(`sum == 0`) and the layer config carried a configured ceiling, we
forward the ceiling. Avoids handing the inner encoder a
target_bitrate of zero, which most encoders interpret as "drop the
stream entirely." This is the same shape NVENC's
`nvEncReconfigureEncoder` recommends.

The framerate is min-clamped to `layer.max_framerate_fps` when set
(the bottom 270p15 layer needs this — without it BWE's "30 fps"
update would push the small layer back to 30 fps and burn CPU).

## 6. Memory + CPU cost

Per-layer cost vs single-layer:

| Resource             | 1× single-layer | 3× simulcast (top + mid + bottom)        |
|----------------------|-----------------|-------------------------------------------|
| Encoder contexts     | 1               | 3 (each holds full libvpx / x264 state)   |
| Per-frame CPU        | 1× encode       | ~1.6× encode (top dominates; bottom-half layers are 1/4 / 1/16 the work). |
| Memory (1080p VP9)   | ~80 MB          | ~120 MB (reference frames per layer + scratch buffers) |
| RTP streams emitted  | 1               | 3 (one SSRC per rid)                      |

(re-validate) — these are theoretical ratios, not measured. The
harness (T10/T11) will tell us the real numbers once the build env
runs.

The cost is acceptable on the 16-vCPU c5.4xlarge-class hosts our
Phase 1 sizing assumes (T17 §4): top + mid + bottom together at
30+30+15 fps fit inside one core's budget for VP9 / H.264 with
zero-latency tuning. Phase 4 NVENC simulcast will be cheaper by
~1.4× (one shared session, one shared CUDA context).

## 7. What we explicitly do *not* do

- **No temporal scalability layers (`L1T2`, `L1T3`).** v1 simulcast
  is spatial-only. Temporal layers would interact with BWE +
  pacer in ways we haven't designed yet. Phase 2.5 follow-up.
- **No SVC.** SVC (single-bitstream multi-resolution / temporal)
  is the AV1 native answer to simulcast; the encoder factory's
  `QueryCodecSupport(format, scalability_mode)` (T19 §contract) is
  the future seam for it. Not in v1.
- **No per-layer encoder type heterogeneity.** All N inner encoders
  are the same codec (VP9 ladder = three Vp9Encoders). Mixing —
  e.g., HW NVENC top layer + SW VP9 lower layers — is a Phase 4.5
  follow-up that the inner-encoder-builder closure already supports
  by construction (the builder receives `SimulcastLayer` and could
  return a different VideoEncoder per layer index).
- **No mid-stream layer addition / removal.** The ladder is fixed at
  InitEncode. libwebrtc renegotiates simulcast via a full SDP roll
  — we Release() and re-InitEncode().

## 8. Failure-mode strategy

One layer's `Encode` failure does **not** abort the others. We log a
warning and continue; libwebrtc's RTP layer simply produces a gap on
that SSRC. This is the same shape libwebrtc's
SimulcastEncoderAdapter uses for the same reason — taking down the
top layer because the bottom layer's encoder hiccupped is a worse
user experience than a single-layer drop.

Per-layer encoder Init failure during `InitEncode` does abort
everything: we tear down any encoders that succeeded so far and
return the error code. Half-initialized state is worse than failing
loudly.

## 9. Cross-cutting integration

- **BweAdapter (T58).** The adapter's `RegisterEncoder` is called by
  `WrapWithBweAdapter`, which wraps the *outer* SimulcastEncoder
  (not each inner). The adapter sees one SimulcastEncoder per
  session; bitrate updates flow through SetRates and are split
  internally. Phase 2.5 stretch: register each inner separately so
  per-layer metrics can break out, but that requires changing the
  decorator's contract.
- **EncoderInfo.** `is_hardware_accelerated = false` always — this
  is the outer wrapper's view; some inners may be HW. Phase 4.5
  follow-up: surface "mixed HW" properly.
- **T77 streamer.** The streamer's `?simulcast=true` query param
  (`SIMULCAST_ENABLED` in streamer.js) flips the encoder factory's
  `Config::enable_simulcast`. SDP carries `a=rid` lines + a
  `simulcast` attribute; libwebrtc populates
  `VideoCodec::simulcastStream[]` from those. Our factory wraps in
  SimulcastEncoder when the streamer asked for it; the deferred-
  ladder path picks up the per-layer dimensions from the negotiated
  SDP.

## 10. Open items

- Bench the libyuv `kFilterBox` choice against `kFilterBilinear`
  on real browser content. (re-validate)
- Decide ladder defaults beyond T77's 1×/0.5×/0.25× — many remote-
  desktop products use 1×/0.5× only. The third layer's marginal
  utility on cloud-browser content is unmeasured.
- Add `SimulcastH264Encoder` typedef-style helpers if call sites
  benefit from named types — currently the factory uses the same
  generic SimulcastEncoder for both VP9 and H.264 with a different
  `codec_name` tag, which is the simplest possible API.

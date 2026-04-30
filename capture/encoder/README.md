# `capture/encoder/` — encoder factory extension point

This directory holds the single seam through which video encoders plug
into the libwebrtc pipeline that powers our cloud browser. Both the v1
software encoders (libvpx VP9, x264) and the Phase 4 hardware encoders
(NVENC, VAAPI) attach here. The interface does not change between
phases; only the implementations behind it do.

## Where this sits in libwebrtc

libwebrtc instantiates a `webrtc::PeerConnectionFactory` at process
startup, and that factory is configured with a `VideoEncoderFactory`.
For every offered SDP, libwebrtc asks the factory for supported formats,
constructs the SDP from them, and on the answer it asks the factory to
build the actual `webrtc::VideoEncoder` for the negotiated codec.

```
                +--------------------------+
                | PeerConnectionFactory    |
                |   (libwebrtc)            |
                +-----------+--------------+
                            |
                            | owns
                            v
              +------------------------------+
              | VideoEncoderFactory          |
              | (us:                         |
              |  CloudBrowserVideoEncoderFactory)
              +-------+-----------+----------+
                      |           |
       GetSupported   |           |  CreateVideoEncoder(format)
       Formats()      |           |
                      v           v
                  SDP layer    VideoEncoder
                               (VP9 / H264 / NVENC / VAAPI / ...)
```

We register exactly one factory per Chromium / streamer process; the
factory itself routes to the right encoder based on `SdpVideoFormat`.

## SW-then-HW plug-in path

| Phase | What plugs in here | Notes |
|-------|--------------------|-------|
| v1 (Phase 1) | libvpx VP9, x264 (SW) | Stub today; real wrappers land in follow-up tasks. Both run zero-latency, no B-frames, intra-refresh. |
| Phase 2 | Same SW encoders, now driven by Chromium-internal capture (`FrameSinkVideoCapturer`) instead of `getDisplayMedia`. | Encoder factory is unchanged — the swap is behind the capturer. See `docs/capture/path-of-least-resistance.md`. |
| Phase 4 | NVENC, VAAPI (HW) | Add new branches in `CreateVideoEncoder` and entries in `GetSupportedFormats`; `QueryCodecSupport` flips `is_power_efficient=true` so libwebrtc's encoder selector prefers HW. |

The contract pinned in `encoder_factory.h` is what makes this a
plug-in path and not a rewrite each phase. Don't change the contract
without writing it down in `docs/internal/encoder-factory-design.md`.

## Runtime SW-vs-HW selection (T63)

`Config` exposes `prefer_nvenc_{h264,hevc,av1}` flags. Each is
evaluated **per `CreateVideoEncoder` call** as:

```
hand back HW (NvencEncoder) IF
    Config::prefer_nvenc_<codec> is true
    AND NvencEncoder::ProbeAvailable(<codec>) succeeded
ELSE
    hand back SW (Vp9Encoder / H264Encoder)
```

The probe is a cheap session-open + immediate-close on a tiny
session; it runs once per process per codec and the result is cached
(`MutableNvencCache()` in `encoder_factory_stub.cc`). A failed probe
**silently falls back to SW** — every host always has a working
encoder for every codec in our SDP. This matters because cloud GPU
fleets are mixed: per `docs/research/av1-encoders.md` an AWS L4
supports NVENC AV1 but a T4 / A10 does not.

The factory is correct on a host with **no GPU at all** — every
probe returns false, every encoder is SW, and we simply lose the
HW fast path.

## Files

- `encoder_factory.h` — the interface and the `Config` knobs (enable
  flags + latency tuning + HW preferences). Reference base class:
  `third_party/webrtc/api/video_codecs/video_encoder_factory.h`.
- `encoder_factory_stub.cc` — production stub that returns supported
  formats, runs the NVENC probe cache, and routes to SW or HW.
- `vp9_encoder.{h,cc}` (T35), `h264_encoder.{h,cc}` (T36),
  `nvenc_encoder.{h,cc}` (T63) — the per-codec implementations.
- `bwe_adapter.{h,cc}` (T58) — central BWE-update fan-out wrapped
  around every encoder created by the factory.

## Build

This code requires libwebrtc headers, which only exist inside a
checked-out Chromium tree (see `docs/build/chromium-from-source.md`).
Until our Chromium build environment is up, treat these files as
**design artifacts that compile in the future build**, not as something
you can `cmake` here today.

The first task that actually compiles them will land alongside the
Phase 2 prototype branch when the from-source build is provisioned.

## Design rationale

See `docs/internal/encoder-factory-design.md`.

## Prior art

Selkies hard-codes its encoder list in environment variables
(`SELKIES_ENCODER`) and selects between GStreamer elements
(`nvh264enc`, `vah264enc`, `x264enc`, …). We adopt the same
selection model — one knob, one process, no recompile to switch
encoders — but expose it as a `Config` struct on the factory rather
than as an env var, because the libwebrtc-side wiring is C++. See
`docs/prior-art/selkies.md`.

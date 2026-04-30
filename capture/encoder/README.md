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

## Files

- `encoder_factory.h` — the interface and the `Config` knobs (enable
  flags + latency tuning). Reference base class:
  `third_party/webrtc/api/video_codecs/video_encoder_factory.h`.
- `encoder_factory_stub.cc` — minimal stub that returns supported
  formats and hands back placeholder encoders. Real encoder bodies
  arrive in follow-up tasks.

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

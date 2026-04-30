# Simulcast (T77)

How `cloud-browser-webrtc` lets the cloud-side encoder send multiple
resolution layers in parallel so the receiving end can pick whichever
fits its bandwidth.

> **Status:** v1. Phase-2 stretch from PROJECT_BRIEF — pairs with
> T58 (BWE → encoder bitrate adapter) and T35/T36 (libvpx VP9 / x264
> encoders). Off by default; enabled via `?simulcast=true` on the
> streamer page query string.

---

## TL;DR

- We ship **simulcast**, not SVC. Simulcast = N independent encoded
  streams in one m=video section with N SSRCs; SVC = one stream with
  hierarchical layers in a single bitstream. libwebrtc's simulcast
  story has been stable for years; SVC AV1/VP9 is newer and patchier.
  We may revisit when AV1-SVC is ubiquitous (Phase 4).
- The streamer is the source of truth: it negotiates 1, 2, or 3
  layers via `RTCRtpSender.setParameters({encodings: [...]})` before
  `createOffer()`. libwebrtc bakes this into the SDP automatically
  (a=rid + a=simulcast).
- The client is a **passive receiver** in v1. With direct
  peer-to-peer (no SFU between the streamer and the client) all
  selected layers come over the wire regardless. The client's job is
  to surface which layer it ended up decoding (via `getStats()`) and,
  in the future, to ask the streamer to disable layers it doesn't
  need.

---

## Layer table (v1 default)

| layer | rid       | resolution scale | target FPS | target bitrate | use case |
|-------|-----------|------------------|------------|----------------|----------|
| 0     | `layer0`  | 1×               | 30         | 4 Mbps         | LAN / fibre — full-fidelity. |
| 1     | `layer1`  | 0.5× (½ each axis) | 30       | 1.5 Mbps       | Typical home broadband. |
| 2     | `layer2`  | 0.25× (¼ each axis) | 15      | 400 kbps       | Mobile / hostile-network fallback. |

Pixel maths: at the streamer's native 1920×1080:
- layer 0: 1920×1080
- layer 1: 960×540
- layer 2: 480×270

Layer 2 also halves the framerate. libwebrtc honours
`maxFramerate` per encoding entry as well as
`scaleResolutionDownBy`.

The query-string override is a comma-separated list, e.g.:

```
?simulcast=true&simulcast_layers=1,0.5,0.25@15
```

Per-entry syntax: `<scale>[@<maxFps>]`. Max 3 layers (any more is
PROJECT_BRIEF Phase-4 territory; libwebrtc imposes its own ceiling
of 3 spatial layers for VP9 simulcast as of the version we ship).

---

## SDP shape

When `?simulcast=true` is on, the streamer's offer gains:

```
m=video 9 UDP/TLS/RTP/SAVPF 96 97 ...
a=extmap:N urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id
a=extmap:M urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id
a=rid:layer0 send
a=rid:layer1 send
a=rid:layer2 send
a=simulcast:send layer0;layer1;layer2
```

The corresponding SSRC list (one per layer) appears as
`a=ssrc:<n> cname:...` plus the `a=ssrc-group:FID` retransmission
groupings.

The answer side mirrors with `a=simulcast:recv layer0;layer1;layer2`
(or a subset if the answerer wants only some).

### Why `a=rid` and not just SSRCs?

- `a=rid` is the modern (RFC 8852) form and is what libwebrtc emits
  by default when `setParameters` is called with `rid` keys.
- The legacy `a=ssrc-group:SIM` form is still accepted by older Edge
  builds; we don't emit it but our parsing is tolerant (it ignores
  it).

### Why `a=simulcast:send` (and `recv`) and not just rid?

- `a=rid` declares the restrictions per layer; it doesn't say "these
  three rids are alternatives" vs "these three rids are independent
  streams." `a=simulcast` (RFC 8853) is the disambiguator.
- Without `a=simulcast`, some browsers will send all three layers
  but the answerer treats them as independent media — bandwidth
  multiplied by 3 for no win.

---

## Client-side behaviour (v1)

The client (post-T34 answerer):

1. Receives the offer with three `a=rid:layerN send` lines and
   `a=simulcast:send layer0;layer1;layer2`.
2. Generates the answer; `setLocalDescription` produces a matching
   `a=simulcast:recv layer0;layer1;layer2`.
3. Surfaces "which layer is currently active" in the debug panel
   via `getStats()`'s `inbound-rtp.{rid}` field — when libwebrtc
   reports per-rid stats, the highest-bytesReceived rid is the one
   currently being decoded.
4. **Does not** dynamically pause layers in v1. Pure peer-to-peer
   with no SFU means all three layers travel over the wire regardless
   of which one the client decodes; the bandwidth saving comes from
   the streamer-side encoder (BWE-driven, T58) reducing per-layer
   bitrate when congestion is detected, not from the client
   selecting fewer layers.

When we add an SFU (Phase-4 multi-client), the client gains an
explicit "subscribe to layer N" knob via signaling; until then the
selection happens transparently inside libwebrtc's receiver.

---

## SDP munging utilities

Pure functions in `client/src/simulcast.ts`, sibling to T30's
`prioritizeCodec` / `forceH264Profile`:

- `parseSimulcast(sdp)` — extract the `(rids, sendOrRecv)` declared
  by `a=simulcast:`. Returns `null` if absent.
- `addSimulcastReceive(sdp, rids)` — rewrite an answer SDP to declare
  `a=simulcast:recv <rids>` and the matching `a=rid:N recv` lines.
  Used when the client wants to advertise that it'll only receive a
  subset (e.g., dropping layer 0 on slow networks). v1 leaves this
  unused but ships the function for Phase-2 BWE adaptation.
- `forceSimulcastLayers(sdp, layers)` — rewrite the m=video section's
  `a=rid` and `a=simulcast` lines with the supplied layer list.
  Useful for tests + manual experiments.

These are pure SDP transforms; no DOM, no peer-connection access.

## Streamer-side configuration

`capture/streamer-page/streamer.js` reads `?simulcast=true` and an
optional `?simulcast_layers=` from the URL. Before `createOffer()`,
it walks `pc.getSenders().filter(kind === "video")` and calls
`setParameters` with three `encodings` entries — libwebrtc bakes
these into the SDP. There is no per-layer encoder configuration done
JS-side; the C++ encoder factory (T35/T36) sees the simulcast index
in the encode-frame callback.

```js
const sender = pc.getSenders().find((s) => s.track?.kind === "video");
const params = sender.getParameters();
params.encodings = [
  { rid: "layer0", scaleResolutionDownBy: 1,  maxFramerate: 30 },
  { rid: "layer1", scaleResolutionDownBy: 2,  maxFramerate: 30 },
  { rid: "layer2", scaleResolutionDownBy: 4,  maxFramerate: 15, maxBitrate: 400_000 },
];
await sender.setParameters(params);
```

`scaleResolutionDownBy` cuts each axis by the divisor (so 2 means
the axis is halved, area is quartered — not "scale down by 2 means
1080p → 540p", which it does anyway because the input is 1080p).

---

## Encoder side (chromium-dev's lane)

`capture/encoder/encoder_factory_stub.cc` and the actual encoders
(`vp9_encoder.cc` from T35, `h264_encoder.cc` from T36) need to
know which simulcast layer a given encode call belongs to. libwebrtc
passes this through `webrtc::VideoCodec::simulcastStream[i]` and the
per-frame `EncodedImage::SpatialIndex`.

Two strategies for libvpx VP9 (chromium-dev to pick):

- **Multiple encoder instances**, one per layer. Simpler; each
  instance is a normal VP9 encoder configured for its target
  resolution + bitrate. CPU cost is roughly 1× + 0.5× + 0.25× ≈ 1.75×
  vs. single-encoder.
- **Native VP9 simulcast** via `cfg.ts_number_layers` (temporal
  layers, not what we want here — that's SVC) or
  `cfg.ss_number_layers` (spatial, but VP9-internal SVC, not
  simulcast). The libvpx API name is misleading; for simulcast we
  want option 1.

For x264: option 1 only — x264 has no native simulcast. Three
instances, three bitrate ladders, libwebrtc does the rest.

See `docs/internal/simulcast-tradeoffs.md` for the cost-benefit.

---

## Files

- `docs/protocols/simulcast.md` — this file.
- `docs/internal/simulcast-tradeoffs.md` — when (not) to enable.
- `client/src/simulcast.ts` — SDP utilities.
- `client/src/simulcast.test.ts` — fixtures + assertions.
- `capture/streamer-page/streamer.js` — `?simulcast=true` wiring.
- `capture/encoder/encoder_factory_stub.cc` — TODO for chromium-dev:
  per-layer encoder instances; see "Encoder side" above.

## References

- [RFC 8853](https://www.rfc-editor.org/rfc/rfc8853) — Simulcast in SDP.
- [RFC 8852](https://www.rfc-editor.org/rfc/rfc8852) — RTP Stream IDs (a=rid).
- [w3c webrtc-pc §RTCRtpEncodingParameters](https://www.w3.org/TR/webrtc/#rtcrtpencodingparameters)
- [libwebrtc simulcast tracker](https://webrtc.googlesource.com/src/+/main/api/video_codecs/video_encoder.h)
- T58 — BWE → encoder bitrate adapter.
- T30 — SDP munging foundation.

# Simulcast: when (not) to enable

A short, opinionated guide to whether to flip `?simulcast=true` on a
given deployment of `chromeless`. Companion to the protocol
doc at [`docs/protocols/simulcast.md`](../protocols/simulcast.md).

## Default: off

v1 ships with simulcast **disabled** by default. The single-user
1:1 cloud-browser-to-browser case (PROJECT_BRIEF Phase 1) is the
worst possible case for simulcast: there is no SFU to sub-set the
layers, the receiver has exactly one rendering target, and every
extra layer is pure waste of CPU and bandwidth on both ends.

## When to enable

Enable when **any** of these hold:

1. **Multi-client / shared-room mode** (PROJECT_BRIEF Phase 4).
   Multiple watchers means the receiving end can drop the high
   layer for one viewer without penalising others. This is
   simulcast's home turf.
2. **You expect a long-tail of receiver bandwidth conditions.**
   E.g., a public watch-party where some viewers are on fibre and
   some on cellular. layer-2 (270p15 @ 400 kbps) gives the cellular
   viewer a degraded-but-watchable stream while layer-0 stays
   pristine for the fibre viewer.
3. **An SFU or relay is in the path.** Cloudflare Calls,
   janus-gateway, mediasoup, livekit — all expect multi-layer
   encoded inputs and will pick the right layer per receiver.
4. **You're testing BWE behaviour** (T58). Simulcast plus
   `?simulcast=true` plus a network shaper is the easiest way to
   exercise libwebrtc's bandwidth estimator + per-layer rate
   adaptation.

## When to leave it off

- **Single-user, point-to-point.** v1's intended deployment. The
  encoder pays a ~1.75× CPU tax (three encoder instances vs. one)
  and the network pays ~3× bandwidth, all to deliver the same
  video that one stream would have delivered. Simulcast gains zero
  here.
- **CPU-constrained host.** Three concurrent encoders eat cores. If
  the box is already saturated under a single layer, adding two
  more layers makes everything worse — you'll see encoder-driven
  latency spikes (T58's `qualityLimitationReason` becomes "cpu")
  long before the network notices.
- **You're chasing absolute minimum latency.** The per-layer
  rate-control + RTP packetization adds a small but real budget at
  the wire, and the receiver-side jitter buffer has to make
  per-layer decisions that single-stream skips. v1's <100 ms LAN
  budget assumes single-stream.

## Cost model

Rough back-of-the-envelope, on a Phase-1 4-core x86_64 host with
libvpx VP9 software encode:

| layers | encoder CPU | uplink bandwidth | latency overhead vs. single-stream |
|--------|-------------|------------------|--------------------------------------|
| 1 (single, default) | ~1.0×   | 4 Mbps           | baseline                              |
| 2 (1080p + 540p)    | ~1.4×   | 5.5 Mbps         | +5 ms median, +15 ms p99             |
| 3 (1080p+540p+270p) | ~1.75×  | 5.9 Mbps         | +8 ms median, +25 ms p99             |

(These numbers come from a single-laptop benchmark while drafting
T35; they're indicative, not contractual. Re-measure on your
target instance type before quoting them in a customer-facing
document.)

The bandwidth row is non-additive because libwebrtc's BWE shrinks
the higher layers when total bitrate ceiling is hit; total uplink
≈ 1.5× single-stream rather than 3×.

## What about SVC?

Scalable Video Coding (one stream with hierarchical temporal +
spatial layers in a single bitstream) is in many ways the right
answer for "I want layered video without 3× the encoder CPU." But:

- libwebrtc's VP9-SVC support is fully working only as of recent
  Chromium versions and we'd need to pin dependencies.
- AV1-SVC is the future; it isn't quite the present yet on commodity
  cloud CPUs (libaom is too slow at 1080p30, SVT-AV1 is improving
  fast but we still ship in v1 with libvpx VP9).
- Receivers don't all decode SVC layers correctly even when their
  decoders nominally claim to (Safari being the usual headache).

Plan: revisit SVC in Phase 4 alongside the AV1 encoder rollout
(T43 surveyed AV1; T75 ships SVT-AV1 software encode). Until then,
simulcast is the safer ladder.

## Interactions with other features

- **T30 SDP munging.** `prioritizeCodec(answer.sdp, "VP9")`
  operates on the m=video PT list and is unaffected by `a=rid` /
  `a=simulcast` lines. The two features compose.
- **T54 codec fallback.** classifyNegotiation reads the first PT;
  if simulcast is on with VP9, all three layers are VP9, so the
  classifier returns "ok" exactly as it did pre-T77.
- **T58 BWE adapter.** With simulcast on, BWE's bitrate cap applies
  per-layer; the adapter must understand the per-encoding
  `maxBitrate` field rather than the single-stream cap. T58
  followup: confirm the BWE listener wires per-layer.
- **T42 stats sampling.** `getStats()` returns one
  `outbound-rtp` per layer. The current
  `client/src/stats.ts::extract` flattens to "first video
  outbound" — under simulcast the stats panel will under-report.
  T42-followup if simulcast lands as a default rather than a flag.

## Test invocation

```bash
# offer with default 3-layer ladder
?simulcast=true

# custom 2-layer ladder (1080p30 + 540p15)
?simulcast=true&simulcast_layers=1,0.5@15

# single layer (effectively disables simulcast)
?simulcast=true&simulcast_layers=1
```

Verify on the wire: the streamer-page debug log should show
`simulcast configured` with the expected layer table; the offer
SDP should contain `a=simulcast:send layer0;layer1;layer2`;
`getStats()` on the streamer side should report three
`outbound-rtp` entries with distinct `rid` values.

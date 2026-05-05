# BWE → encoder bitrate adapter — design rationale

**Status:** Phase 2 prep (T58). Code at
`capture/encoder/bwe_adapter.{h,cc}`.
**Cross-references:** `capture/encoder/encoder_factory.h` (T19),
`capture/encoder/vp9_encoder.h` (T35), `capture/encoder/h264_encoder.h`
(T36), `docs/internal/encoder-factory-design.md`,
`PROJECT_BRIEF.md` Phase 2 ABR.

## 1. Why this layer exists

libwebrtc already runs a bandwidth estimator (GoogCC by default) and
already calls `webrtc::VideoEncoder::SetRates()` on each active
encoder when its target bitrate changes. The signal reaches the
encoders end-to-end without our help. So why a new layer?

- **Observability.** Without an explicit central observer, the only
  way to log every BWE update is to add logging to every per-codec
  wrapper (`Vp9Encoder::SetRates`, `H264Encoder::SetRates`, etc.).
  That is a duplicate-knowledge problem. The adapter is the one
  place to log, tag, and emit the bitrate decisions to the
  chromeless-metrics-sidecar (T38).
- **Coordination.** ABR (per `PROJECT_BRIEF.md` Phase 2) is
  cross-encoder: a target-bitrate drop sometimes argues for cutting
  resolution or framerate rather than just lowering the encoder
  bitrate. That decision lives **above** the encoder, not inside
  it. Putting it in the adapter — the layer that already sees every
  BWE update and every active encoder — is the natural home.
- **Simulcast / SVC layer mapping.** When we ship simulcast (Phase 2
  stretch), one BWE update has to be split across spatial layers
  according to layer index. That logic also wants to live above the
  per-encoder wrappers.

In short: encoders care about "given a target bitrate, encode well."
The adapter answers "given the link state, what should each encoder's
target bitrate be?" — and Phase 2.x onward, "should we change
resolution?" These are different questions; this is the layer that
asks them.

## 2. Where it slots into libwebrtc

libwebrtc's BWE plumbing (sketched, names from recent main):

```
   Transport feedback (transport-cc / REMB)
                 |
                 v
   GoogCC inside RtpTransportControllerSend
                 |
                 v   target_bitrate, link_capacity, frac_loss, rtt
   RtpVideoSender::OnBitrateUpdated
                 |
                 v
   webrtc::VideoStreamEncoder::OnBitrateUpdated
                 |
                 v   builds RateControlParameters
   webrtc::VideoEncoder::SetRates(...)            <-- existing
                                                       wire
```

Our adapter inserts as a **central observer** above the per-encoder
SetRates calls:

```
   webrtc::VideoStreamEncoder::OnBitrateUpdated  (libwebrtc)
                 |
                 +----> cloud_browser::BweAdapter::OnBitrateUpdated
                                  |
                                  +-- MetricsSink (-> chromeless-metrics-sidecar)
                                  |
                                  v
                           builds RateControlParameters,
                           fans out to every registered encoder
                                  |
                                  v
                       Vp9Encoder::SetRates / H264Encoder::SetRates
```

There are two ways to wire the entry into the adapter:

1. **Direct call from a `VideoStreamEncoder` subclass** we own. This
   is the cleanest seam if we end up wrapping `VideoStreamEncoder`
   for any other reason. Highest coupling.
2. **Patch a small hook** into the existing `VideoStreamEncoder` so
   it consults `ContentBrowserClient::GetWebRtcBweObserver()` (or a
   sibling of patch `0001`'s encoder-factory hook). Lowest coupling
   per call site, one more entry in `patches/`.

For Phase 2 we go with option 2: a sibling to `patches/0001-...`
that adds a single observer hook on the libwebrtc side. Patch lands
when the build env runs (T17). Until then the adapter is callable
directly from tests (which is what `bwe_adapter_test.cc` does).

## 3. Encoder registration model

Encoders register themselves with the adapter on `InitEncode` and
unregister on `Release`. We do **not** modify `Vp9Encoder` or
`H264Encoder` to do this; the adapter coordination lives in a
**decorator wrapper** (`BweRegisteringEncoder`, internal to
`bwe_adapter.cc`) that the factory inserts via `WrapWithBweAdapter`.
Reasons:

- Per-codec wrappers stay small and codec-focused — no
  conditional-on-adapter branches in their hot path.
- Adapter coordination is in exactly one place; future changes
  (different registration semantics, layer-index re-shuffles,
  per-encoder rate-rewriting) live in the decorator and don't ripple
  out.
- The decorator's destructor defensively unregisters in case
  `Release` was skipped — libwebrtc's contract says it must be, but
  belt-and-braces is cheap.

## 4. Phase 2 stretch — simulcast / SVC layer mapping

Today the adapter fans the same `RateControlParameters` to every
encoder. For simulcast, we will:

1. Track each encoder's registered `layer_index` (already in
   `EncoderEntry`).
2. Use libwebrtc's `BitrateAllocator` distribution shape (or a
   simpler bitrate-per-layer table) to split the total bitrate.
3. Build a **per-encoder** `VideoBitrateAllocation` rather than the
   single-layer `(0,0)` set used today.

The hook is the inner loop of `OnBitrateUpdated` — when we add
multi-layer logic, it lives there. The encoder-side contract does
not change: each encoder still receives one `SetRates` per BWE update
with its own allocation. Tests for the simulcast path will mock two
encoders at different layer indices and assert the bitrate split.

## 5. Phase 2 stretch — ABR (resolution / framerate scaling)

`PROJECT_BRIEF.md` Phase 2: *"Adaptive bitrate: hook libwebrtc's
bandwidth estimator, scale resolution and framerate on congestion."*

The adapter is the natural owner of this decision. Sketch:

- Track a sliding window of the last N BWE updates.
- Apply a hysteresis policy: drop resolution when stable_bitrate
  stays under a threshold for ≥ T seconds; raise it again only when
  the bitrate stays above an upper threshold for ≥ T seconds.
  Hysteresis avoids resolution oscillation under jitter.
- Drive resolution changes via the source side
  (`VideoCaptureTarget` reconfiguration on the
  `FrameSinkVideoCapturer` from T55) plus a forced IDR via
  `RequestRefreshFrame`.
- Framerate scaling is similar: `SetMinCapturePeriod` change on the
  capturer.

This logic does not exist in v1; the adapter shape just doesn't
foreclose it. The thing that *would* foreclose it is putting the
BWE-to-encoder routing inside per-codec wrappers — exactly what we
are not doing.

## 6. Threading and lock-order

`OnBitrateUpdated` runs on libwebrtc's worker thread. Encoder
methods run on libwebrtc's encoder thread (different one). The
adapter's mutex protects the encoder registry only; it is **dropped
before** calling `SetRates` on each encoder, because:

- `SetRates` may acquire encoder-internal locks (e.g.,
  `Vp9Encoder` re-pushes vpx_codec_enc_config_set under its own
  state lock).
- Holding `BweAdapter::mu_` across `SetRates` would establish a
  lock-order edge from `BweAdapter::mu_` to encoder locks — a
  classic deadlock recipe in libwebrtc encoder factories.

The adapter takes a `std::vector<EncoderEntry>` snapshot under the
lock, releases it, then iterates the snapshot. The cost is one
allocation per BWE update — measured in micros, irrelevant at
BWE update cadence (~1 Hz steady state, 10 Hz under congestion).

## 7. Metrics

`MetricsSink` is `std::function<void(const BweUpdate&)>`. The
adapter calls it on every update from the same thread that fired
`OnBitrateUpdated`. The sidecar's job is to translate that into
Prometheus counters / gauges and ship to the existing scrape
endpoint (T38).

The minimum set we want exported:

| Metric                              | Type    | What                              |
|-------------------------------------|---------|-----------------------------------|
| `bwe_target_bitrate_bps`            | gauge   | total target bitrate              |
| `bwe_stable_bitrate_bps`            | gauge   | smoothed estimate                 |
| `bwe_fraction_loss`                 | gauge   | 0..255                            |
| `bwe_rtt_ms`                        | gauge   | round-trip                        |
| `bwe_active_encoders`               | gauge   | how many encoders fanout reaches  |
| `bwe_updates_total`                 | counter | for rate-of-update health checks  |

Wire-up of the sink to the metrics sidecar is a small follow-up,
filed when the libwebrtc build env exists.

## 8. What this design explicitly does *not* do

- **Doesn't replace `webrtc::BitrateAllocator`.** That remains
  upstream's job. We observe its output (target bitrate) and route
  it; we do not re-allocate across audio/video/data streams.
- **Doesn't drop encoders behind the libwebrtc back.** If libwebrtc
  thinks an encoder is alive and we don't, we deadlock our own
  pipeline. The decorator's contract guarantees register/unregister
  always corresponds to InitEncode/Release.
- **Doesn't fight the pacer.** libwebrtc's pacer is the right place
  for backpressure; the adapter only changes target bitrate, never
  drops frames itself.

## 9. Open items

- File a sibling patch to `patches/0001-...` exposing the
  `OnBitrateUpdated` callback hook to the embedder. Until that
  patch exists, the adapter is reachable from tests but not from
  production libwebrtc. (Tracked under T58.)
- Decide simulcast policy before adding the layer-index branch in
  `OnBitrateUpdated`. (Phase 2 stretch.)
- Decide ABR thresholds + hysteresis windows. (Phase 2 stretch.)

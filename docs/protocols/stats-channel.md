# Stats channel protocol (v1)

How the client publishes WebRTC `getStats()` samples both into its own
debug panel and out to the server for Prometheus scraping (T38).

> **Status:** v1, frozen for Phase 0 / Phase 1.
>
> **Phase 2 reconsider:** if the server gains direct access to the
> peer connection's stats (libwebrtc native stats callbacks), the
> data-channel hop becomes redundant. We may keep this protocol as a
> client-only debug surface and replace the server-bound emission
> with a native scrape.

---

## Transport

- **Channel:** `RTCDataChannel` named `"stats"`, ordered, reliable.
- **Direction:** client → server only.
- **Cadence:** one frame per sample interval (default 1 Hz). The client
  should NOT emit on every `getStats()` call faster than that — the
  server has no use for sub-second granularity at this stage.
- **Encoding:** UTF-8 JSON, one envelope per `RTCDataChannel.send()`.

The streamer offers the channel along with the existing `"input"`
channel; the client receives it via `pc.ondatachannel` and binds the
sampler's emit hook to it.

## Envelope

```jsonc
{
  "v": 1,
  "t": 1730290000123,        // client wall-clock ms (Date.now())
  "sample": <StatsSample>
}
```

`v` and the StatsSample shape are versioned together. Adding fields to
StatsSample is non-breaking; removing or renaming bumps `v`.

## StatsSample shape

Mirrors `client/src/stats.ts::StatsSample` exactly. Field documentation
is on the TS interface; the wire form is just the JSON serialization.

```jsonc
{
  "v": 1,
  "t": 1730290000123,
  "inbound": [
    {
      "trackId": "RTCInboundRtp_video_1",
      "kind": "video",
      "bytesReceived": 12345678,
      "packetsReceived": 9876,
      "packetsLost": 4,
      "jitter": 0.005,
      "framesPerSecond": 30,
      "framesDropped": 0,
      "framesReceived": 1500,
      "totalDecodeTime": 21.4
    }
  ],
  "outbound": [],
  "remoteInbound": [
    {
      "trackId": "RemoteInboundRtp_video_1",
      "kind": "video",
      "roundTripTime": 0.062,
      "packetsLost": 4,
      "fractionLost": 0.0004
    }
  ],
  "candidatePair": {
    "currentRoundTripTime": 0.062,
    "availableOutgoingBitrate": 1500000,
    "availableIncomingBitrate": 4000000,
    "localCandidateType": "host",
    "remoteCandidateType": "srflx"
  }
}
```

## Field selection rationale

We intentionally do NOT forward the full `RTCStatsReport`. Reasons:

- It is large (Chromium emits 20+ stat objects per second, ~5–10 KB
  serialized). Multiplied by N concurrent sessions, this becomes
  meaningful bandwidth on the signaling / metrics path.
- Most fields churn in ways that matter only to the client (e.g.,
  per-codec `payloadType`). The server cares about the distilled
  health: bitrate, RTT, FPS, loss.
- A flat shape is cheap to map into Prometheus labels (`tenant_id`,
  `track_kind`) without fighting cardinality.

The selected fields cover the diagnostic axes from
`docs/v1-success-criteria.md`:

- Latency budget (<100 ms LAN / <200 ms regional) → `currentRoundTripTime`.
- Quality target (30 fps, 4 Mbit/s peak) → `framesPerSecond`,
  `availableOutgoingBitrate`.
- Reliability → `packetsLost`, `framesDropped`, `qualityLimitationReason`.

## Backpressure

- If the data channel's `bufferedAmount` exceeds 256 KiB, the client
  drops the sample (it logs locally; doesn't queue). At 1 Hz with the
  shape above, this should never trigger absent a stuck downstream.
- If the channel is not yet `"open"` at sample time, the sample stays
  in the debug panel only. Future Phase-2 server-side scrape doesn't
  depend on the data-channel path.

## Versioning

- `v: 1` today.
- Adding new optional fields to inbound/outbound/remoteInbound/candidatePair
  does NOT bump `v`. The server MUST ignore unknown fields.
- Adding new top-level keys to the envelope or to StatsSample is also
  non-breaking. Servers MUST tolerate them.
- Removing or renaming a field bumps `v`.

## Files

- `client/src/stats.ts` — `StatsSampler`, types, `formatSummary`.
- `client/src/stats.test.ts` — extraction + interval scheduling tests
  with fake `RTCStatsReport`.
- `client/main.ts` — wires the sampler in, logs the summary line per
  sample, forwards each frame over the `stats` data channel when the
  streamer offers it.
- Phase-2 server-side consumer: see T38 follow-up.

## Cross-references

- T38 — server-side Prometheus endpoint that this protocol feeds.
- T20 — input data channel (sibling protocol; same envelope-style).
- T34 — role flip; the streamer offers `stats` like it offers
  `input`.

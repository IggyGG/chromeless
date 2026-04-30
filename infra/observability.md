# Observability

Per-container metrics for cloud-browser-webrtc. The shape of the
observability story is fixed in Phase 0 so latency tuning (Phase 2) and
capacity planning (Phase 3) have signal from day one rather than being
retrofitted onto a system that's already in production.

## What's exposed

Two Prometheus endpoints, both on plain HTTP:

| Endpoint | Service | Port | What |
|---|---|---|---|
| `/metrics` | signaling (`signaling/`) | `8080` | Session counts, message types, websocket close codes, active connections by role. |
| `/metrics` | cb-metrics-sidecar (`capture/cb-metrics-sidecar/`) | `9100` | Aggregate Chromium CPU/RSS, per-track WebRTC bitrate / fps / qp / dropped frames / RTT / remote-side packet loss. |

### Signaling metrics

| Name | Type | Labels | Notes |
|---|---|---|---|
| `cb_signaling_sessions_total` | counter | — | Lifetime session creations. |
| `cb_signaling_messages_total` | counter | `type` | One series per envelope `type` we forward (`offer`, `answer`, `ice`, `bye`). Capacity-planning signal: the ratio of `offer` to `answer` is a leading indicator of failed handshakes. |
| `cb_signaling_close_total` | counter | `code` | Websocket close codes (1000 = normal, 1001 = going away, 1006 = abnormal, etc.). Incident-response signal: a spike in 1006 means the network or a load balancer is killing connections. |
| `cb_signaling_active_sessions` | gauge | — | Currently live sessions. |
| `cb_signaling_active_connections` | gauge | `role` | One per `client` / `browser` peer. Should track `2 × active_sessions` in steady state; divergence means a peer is slow to disconnect. |

### Sidecar metrics

| Name | Type | Labels | Notes |
|---|---|---|---|
| `cb_chromium_cpu_pct` | gauge | — | Aggregate CPU% across all Chromium processes, expressed as percent of one core. 100 == one fully busy core; 200 == two; on an N-core box the practical ceiling is 100·N. |
| `cb_chromium_rss_bytes` | gauge | — | Aggregate resident set size of all Chromium processes. |
| `cb_webrtc_outbound_bitrate_bps` | gauge | `kind` | Outbound RTP bps for `video` / `audio`, computed from `bytesSent` deltas over the polling interval. |
| `cb_webrtc_outbound_frames_per_second` | gauge | — | Encoder fps (video only). |
| `cb_webrtc_outbound_dropped_frames_total` | counter | — | Frames dropped by the outbound video pipeline. |
| `cb_webrtc_outbound_qp` | gauge | — | Average video encoder quantizer (`qpSum / framesEncoded`). Lower = higher quality. Phase 2 latency-vs-quality tuning watches this. |
| `cb_webrtc_remote_inbound_packets_lost_total` | counter | — | Packets the remote peer reported as lost on inbound. |
| `cb_webrtc_round_trip_time_ms` | gauge | — | Selected ICE candidate-pair RTT, in milliseconds. The single best leading indicator of perceived interactivity. |

## How the sidecar gets WebRTC numbers

The streamer page (T23) exposes `window.pc` — the active
`RTCPeerConnection`. The sidecar:

1. `GET http://127.0.0.1:9222/json` to enumerate DevTools targets.
2. Picks the page whose title contains `streamer`.
3. Opens a fresh DevTools websocket per poll and sends a single
   `Runtime.evaluate` invoking `window.pc.getStats()`, deserialized by
   value.
4. Walks the result; classifies entries by `type` (`outbound-rtp`,
   `remote-inbound-rtp`, `candidate-pair`); updates Prometheus
   metrics.

This keeps the sidecar dep-free of any custom Chromium build and lets
`window.pc`'s contract (already a public surface for T24's audio-presence
smoke and T31's idle watchdog) be the single integration point.

## Scrape config

Sample Prometheus job for both endpoints. Adjust the static targets to
the host/port your dev stack publishes.

```yaml
scrape_configs:
  - job_name: cloud-browser-signaling
    metrics_path: /metrics
    static_configs:
      - targets: ['localhost:8080']
        labels:
          service: signaling

  - job_name: cloud-browser-chromium
    metrics_path: /metrics
    static_configs:
      - targets: ['localhost:9100']
        labels:
          service: chromium-sidecar
```

For a real multi-tenant Phase 3 deployment, replace the static targets
with Kubernetes service discovery and add `session_id` as a label
sourced from a pod annotation.

## Adding a new metric

1. **In the signaling server**: declare the metric in
   `signaling/metrics.go` with `promauto.New*`. Add a
   `recordXxx` helper alongside the existing ones. Call the helper at
   the relevant hook point in `signaling/server.go`. Keep the helper
   single-call and the call site one line — server.go's structure is
   webrtc-dev's territory and we want the diff to stay surgical.

2. **In the sidecar**: declare the metric in `main.go`'s `var ( … )`
   block at the top. Update `statsState.update()` (for WebRTC stats),
   `procReader.sample()` (for /proc-derived metrics), or add a new
   probe function for any new source. Bump the polling interval if a
   new source materially increases per-iteration cost.

3. **Document it here**, including the unit and what it's for. A
   metric without a documented purpose ends up panel-orphaned in
   Grafana and alert-orphaned in your pager.

## What these signals are FOR

### Phase 0–1: prove the boring path works

The latency target is the product. Grafana panels we expect to live by:

- `cb_webrtc_round_trip_time_ms` p50/p99 over time, with the v1 LAN /
  regional targets drawn as horizontal lines (100 ms / 200 ms). When
  this plot stays under the lines, we ship.
- `cb_webrtc_outbound_qp` over time, alongside
  `cb_webrtc_outbound_bitrate_bps{kind="video"}`. Sustained QP > ~38
  with bitrate already at ceiling means the encoder is saturated — the
  user sees pixelation. This pair of signals tells us whether to
  invest the next hour in encoder tuning, capture optimization, or
  more CPU.
- `rate(cb_signaling_messages_total{type="offer"}[5m])` vs
  `rate(cb_signaling_messages_total{type="answer"}[5m])`. They should
  match in steady state. They don't when ICE is failing, which they
  will, often.

### Phase 2: tune capture + encoder

`cb_chromium_cpu_pct` and `cb_chromium_rss_bytes` show whether software
encode is bumping CPU into the red. Cross-correlate with
`cb_webrtc_outbound_dropped_frames_total` rate to confirm
encoder-saturation-as-cause vs encoder-saturation-as-effect.

### Phase 3: capacity planning

`cb_signaling_active_sessions` becomes the per-host concurrency
ceiling. `cb_chromium_cpu_pct / cb_signaling_active_sessions` is the
"CPU per session" datum we publish in the README.

## Limitations

- **Polling cadence is 10 s by default.** Burst latency events shorter
  than the poll interval show up only as small offsets in
  monotonic counters. For real per-frame analysis the latency harness
  (`harness/`) is the right tool, not /metrics.
- **The sidecar reads from `window.pc`** as exposed by T23. If a
  future streamer page renames the variable, all WebRTC metrics go to
  zero. Phase 2 will hoist this contract into a shared header file
  consumed by all in-container probes.
- **No per-session labels yet.** Single-tenant v1 means a single
  RTCPeerConnection at a time, so labelling everything with a session
  id would be redundant. When Phase 3 introduces multi-tenancy,
  metrics gain a `session_id` label, sourced from
  `/run/cb-session/id` (T31 cold-start).
- **No alerts here.** Alerts and recording rules belong in the deploy
  repo, not in this image. Phase 3 task.

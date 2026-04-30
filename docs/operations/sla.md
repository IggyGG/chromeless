# Cloud-browser-webrtc SLOs and alert thresholds

This is what we promise. Each SLO is paired with an alert in
[`infra/observability/alerts/cb-alerts.yaml`](../../infra/observability/alerts/cb-alerts.yaml);
breaching the alert threshold means the SLO is at risk and a human
should look. Breaching the SLO itself in steady state is a postmortem
([template](./postmortem-template.md)).

## SLO matrix

| SLO | Target (steady state) | Alert at | Window | Why |
|---|---|---|---|---|
| Glass-to-glass latency p95 (LAN) | < 100 ms | 120 ms (1.2×) | 5 min | The product. v1 success criteria sets this; PROJECT_BRIEF.md § Phase 0 calls it non-negotiable. |
| Glass-to-glass latency p95 (regional) | < 200 ms | 240 ms (1.2×) | 10 min | v1 contract for cross-region users. |
| Session assignment latency p95 (warm path) | < 1 s | 1.5 s | 10 min | The warm-pool whole point. Slower than this means we're effectively cold-starting per session. |
| Session assignment latency p95 (cold path) | < 30 s | 45 s | 15 min | Matches Pod `start_period` in `infra/k8s/cloud-browser-session.yaml`; slower means the readiness probe is racing the controller. |
| Connection success rate | ≥ 99% | < 95% (5 min rate) | 5 min | An aggregate health signal pulled from `cb_signaling_close_total{code=1006|1011}` ratio + assignment failures. |
| Encoder QP (avg, video) | < 38 | sustained > 38 | 10 min | At QP 38+ the user sees pixelation. Cross-correlate with bitrate before declaring "encoder problem." |
| Outbound dropped frames rate | < 0.5/s | > 1/s | 5 min | Order-of-magnitude above noise floor for 30 fps. |
| Remote inbound packet loss rate | < 1/s | > 5/s | 5 min | Sustained packet loss is the network signal. |
| Auth failure rate (any reason) | < 1/min in steady state | > 1/s for 5 min | 5 min | Sustained means token-signing key drift or an active attack. |
| Pool warm-replica fill | ≥ 80% of target | < 50% of target | 10 min | Below threshold the assignment loop is cold-starting. |

## How "p95 RTT" is measured

The metric is `cb_webrtc_round_trip_time_ms` — a per-session gauge
sampled by the cb-metrics-sidecar (T38) every 10 s. We don't have a
true histogram on this yet; the alert uses `quantile(0.5,
cb_webrtc_round_trip_time_ms)` across all live sessions as a
defensible approximation.

Glass-to-glass latency *strictly* speaking includes encode + jitter
buffer + display, none of which RTT measures. But:
- Encode is roughly constant (~30 ms for software VP9 zero-latency).
- Jitter buffer adapts to RTT variance.
- Display-side latency is the user's monitor (we can't measure it
  remotely).

So `RTT + 50ms` is a good proxy for glass-to-glass on top of the
client's local rendering cost. The harness ([`harness/`](../../harness/),
T10/T11) does the *real* glass-to-glass measurement; runs of that
land in the deployment-readiness checklist as the v1-acceptance
gate, not the per-tick alert.

## Why a stub for `cb_session_assignment_seconds`

T50's design defines `cb_session_assignment_seconds_bucket` (a
histogram) and T71's controller is the producer. The metric is
**reserved**: until the controller's reconcile loop instruments its
assignment hot path with `prometheus.NewHistogramVec`, the rule
silently has nothing to fire on. That's better than removing the
alert and adding it back later.

When the controller starts emitting these series, the alerts here
become live without code changes here.

## Severity routing

We split alerts into `severity: warning` and `severity: critical`.
- **warning** → channel notification (Slack #alerts), no page.
- **critical** → page (PagerDuty / OpsGenie equivalent).

Mapping in `cb-alerts.yaml` is encoded in the `labels.severity`
field; AlertManager routes from there.

## Postmortem-triggering SLO breaches

A postmortem is written when:
- A `critical` alert fires longer than its `for:` duration.
- Any user-visible regression (latency target missed for > 1 hour
  during business hours; > 0.1% of sessions failing to connect for
  > 30 min).
- Anything the on-call thinks is worth a postmortem.

Use [`postmortem-template.md`](./postmortem-template.md).

# Distributed tracing — operator guide

T99 wired OpenTelemetry across the four services that handle a session:

```
client (browser)  →  signaling  →  controller  →  chromeless-metrics-sidecar
   chromeless.client.*       chromeless.signaling.*   chromeless.controller.*    chromeless.client.stats.received
```

A single trace ID flows from the user's `Connect` button click through
WebSocket signaling, BrowserSession assignment, and the first
`/stats-update` POST landing on the sidecar. The result, viewed in
Jaeger, looks like one timeline with four service swimlanes — the
operator can see *where* a slow connect is being spent without
correlating four different log streams by hand.

This document is the operator's reference: span catalog, how to enable
tracing, how to debug latency hotspots, and the sample-rate policy.

---

## Quick links

- Compose dev backend: `http://localhost:16686` after
  `OTEL_EXPORTER_OTLP_ENDPOINT=jaeger:4317 docker compose -f infra/compose.yaml --profile observability up`.
- K8s manifest: `infra/k8s/observability-jaeger.yaml` (apply
  separately; not in the base kustomize bundle).
- Helm chart: production deployments use Tempo / Grafana Cloud Traces
  rather than Jaeger; see [Production backend](#production-backend).
- Sample-rate env: `CHROMELESS_TRACE_SAMPLE_RATIO=0.1` (default 0.1; set
  to 1.0 for debug runs).

## Span catalog

The four services share one tracer per subsystem so spans group
naturally in Jaeger's UI.

### Client (`chromeless-client`)

| Span | Where opened | Attributes | Notes |
|------|-------------|------------|-------|
| `chromeless.client.connect` | Page load → `Connect` button click in `client/src/main.ts` | `tenant.id`, `session.id`, `region.preference` | Root span for the entire session lifecycle |
| `chromeless.client.signaling.connect` | `WebSocket(wss://signaling/ws/...)` open | `signaling.url`, `tenant.id` | Child of `chromeless.client.connect`; ends on `onopen` |
| `chromeless.client.peer.negotiate` | First `pc.createOffer` / setRemoteDescription | `peer.role`, `codec.preferred[]` | Wraps the ICE + SDP dance; ends when `connectionstate=connected` |
| `chromeless.client.track.received` | `pc.ontrack` for each track | `track.kind`, `track.id` | One span per track; child of `chromeless.client.peer.negotiate` |
| `chromeless.client.stats.emit` | Each `getStats` → data-channel send | `sample.t` | Stamps `cb_trace` on the envelope before send |

### Signaling (`signaling`)

| Span | Where opened | Attributes |
|------|-------------|------------|
| `chromeless.signaling.ws.session` | `/ws/{session_id}` upgraded | `session.id`, `tenant.id`, `region` |
| `chromeless.signaling.envelope.relay` | Per inbound envelope | `envelope.type` (`offer`/`answer`/`candidate`), `direction` |
| `chromeless.signaling.auth.verify` | Token verification (T48) | `auth.subject`, `auth.scheme` |

(The full set is webrtc-dev's territory; this table is what to expect
when reading the Jaeger waterfall.)

### Controller (`browser-session-controller`)

| Span | Where opened | Attributes |
|------|-------------|------------|
| `chromeless.controller.session.assign` | `tryAssign` in `pkg/reconciler/session.go` | `session.name`, `tenant.id`, `pool.name`, `region` |
| `chromeless.controller.pool.replenish` | `Reconcile` in `pkg/reconciler/pool.go` | `pool.namespace`, `pool.name` |

The `assign` span is the high-value one — when a connect is slow,
this is where the wait usually lives (warm-pool exhaustion, replenish
in progress, ScrubAndReturn cycling).

### Sidecar (`chromeless-metrics-sidecar`)

| Span | Where opened | Attributes |
|------|-------------|------------|
| `chromeless.client.stats.received` | `/stats-update` POST handler | `tenant.id`, `session.id`, `envelope.version`, `client.event?` |

When the envelope carries `cb_trace.traceparent`, this span continues
the client-rooted trace. When absent, a new root is opened (sample
rate decides whether it ships).

## Enabling tracing

### Local compose

```bash
OTEL_EXPORTER_OTLP_ENDPOINT=jaeger:4317 \
  CHROMELESS_TRACE_SAMPLE_RATIO=1.0 \
  docker compose -f infra/compose.yaml --profile observability up
```

The `--profile observability` flag brings up Prometheus, Grafana, and
Jaeger together. Sample-rate 1.0 is fine for dev — volume is small
and you want every trace.

Open `http://localhost:16686`, pick `chromeless-client` from the service
dropdown, click **Find Traces**, drill into the most recent. You
should see a four-lane waterfall:

```
chromeless-client          ╞═════════ chromeless.client.connect ═════════╡
signaling          ╞═ ws.session ═╡ ╞═ envelope.relay ═╡
controller         ╞ session.assign ╡
chromeless-metrics-sidecar ╞ stats.received ╡
```

If the controller lane is empty, the trace didn't propagate through
signaling → admission webhook. Check that signaling has
`OTEL_EXPORTER_OTLP_ENDPOINT` set and that the controller container
sees it too. (This is the most common confusion — the env *must* be
set on every pod that's expected to ship spans, even if Jaeger is up.)

### Kubernetes (Phase 1)

```bash
kubectl apply -f infra/k8s/observability-jaeger.yaml
kubectl set env deploy/signaling \
  OTEL_EXPORTER_OTLP_ENDPOINT=jaeger.chromeless.svc.cluster.local:4317 \
  CHROMELESS_TRACE_SAMPLE_RATIO=0.1 \
  -n chromeless
kubectl set env deploy/browser-session-controller ... # same values
kubectl set env deploy/chromeless-metrics-sidecar ...        # in the chromium pod template
```

UI: `kubectl port-forward -n chromeless svc/jaeger 16686:16686`.

### Helm chart (production-shaped)

The chart exposes a top-level `tracing:` block:

```yaml
# values.yaml
tracing:
  enabled: true
  endpoint: tempo.observability.svc.cluster.local:4317
  sampleRatio: "0.1"
  insecure: true   # set false when fronted by a sidecar with TLS
```

Per-service overrides (e.g., crank signaling to 1.0 during an
incident) live under `signaling.tracing.sampleRatio` etc. — same
shape as `chromeless.region` from T94.

## Sample-rate policy

The SDK ships with `CHROMELESS_TRACE_SAMPLE_RATIO=0.1` as the in-code
default — that's the **production** number. Dev and staging should
explicitly override; the compose file already does (defaulting the
env to `1.0`), but K8s and Helm callers must set it themselves.

| Environment | Setting | Why |
|-------------|---------|-----|
| Local compose | **1.0** (compose default) | Dev volume is tiny; you want every trace. Without it, debugging "why doesn't my span show up" turns into a sample-rate hunt. |
| Staging | 0.5 (operator-set) | Catch regressions without flooding the backend |
| Prod (steady-state) | 0.1 (in-code default) | Cost / volume balance; 1 in 10 sessions |
| Prod (incident) | 1.0 (operator-set) | `kubectl set env deploy/... CHROMELESS_TRACE_SAMPLE_RATIO=1.0` for the duration |

> **Heads-up for operators:** if you bring up the observability
> profile locally and don't see every connect, double-check
> `CHROMELESS_TRACE_SAMPLE_RATIO`. The compose default is `1.0`, but a
> stale shell with the prod value exported will silently drop 90% of
> spans.

The sampler is `ParentBased(TraceIDRatioBased(R))`, so:

- A client that started a span at 1.0 keeps the trace through every
  hop regardless of downstream sample rates. This is how an operator
  can run a "force-traced" session by toggling
  `localStorage.cb_trace_sample_ratio = "1"` in the browser.
- New roots created server-side (e.g., a controller reconcile that
  isn't in response to a client signal) sample at the local rate.

## Debugging walkthroughs

### "Slow connect" investigation

1. User reports connect taking >5 s.
2. Open Jaeger, search `service=chromeless-client operation=chromeless.client.connect duration > 5s`.
3. Pick a trace. The waterfall reveals where the time is spent:
   - **Most of it in `chromeless.client.signaling.connect`** → DNS or TLS.
     Check signaling's load balancer health and `cb_signaling_*` rates.
   - **Most of it in `chromeless.controller.session.assign`** → warm-pool
     exhausted. Check `cb_pool_warm_replicas{pool=...}`. If ~0 and
     replenishment is happening, `MaxAgeSeconds` may be cycling pods
     too aggressively (see runbook section "Warm pool churn").
   - **Most in `chromeless.client.peer.negotiate`** → ICE / SDP path.
     Check `cb_webrtc_ice_failure_total` (T80 alert series). Likely
     a TURN issuer issue (T76 secret rotation grace expired?).
4. Drill into the long span; tag-filter on it (`tenant.id="x"`) to
   see whether it's tenant-specific.

### "Sidecar not receiving stats" (no client → server visibility)

1. Search Jaeger for `service=chromeless-client operation=chromeless.client.stats.emit`.
2. If those exist but no `chromeless.client.stats.received` is a child:
   - Streamer page is buffering / dropping (check
     `dataChannel.bufferedAmount` — see stats-channel.md backpressure).
   - The streamer is forwarding to the wrong sidecar URL (a misconfig
     of `STATS_RELAY_URL` in the streamer page).
3. If `chromeless.client.stats.emit` doesn't exist either, tracing was off
   on the client. Check the browser console for "tracing disabled".

### Cross-region trace lookup

Phase 1: not supported. Each region has its own Jaeger. The operator
identifies the region from `cb_signaling_region_used` (T94) and
opens that region's Jaeger UI.

Phase 2 (when Tempo arrives): one global Tempo with
`tenant_id=region` so trace IDs route to the right region's storage.

## Production backend

The all-in-one Jaeger is fine for dev and ad-hoc staging. Production
should run one of:

- **Grafana Tempo** — same OTLP/gRPC ingest, scales to billions of
  spans, integrates with the existing Grafana T66 panels (the
  chromeless-session-detail dashboard has a "View trace" link that
  templatizes against Tempo's URL pattern).
- **Grafana Cloud Traces** — managed Tempo; same ingest URL pattern.
- **Honeycomb / Lightstep / Datadog APM** — set OTLP endpoint to
  their collector address. All four services use OTLP/gRPC so any
  vendor with an OTel collector works.

Switching backends is a values.yaml change in the Helm chart — no
code edits needed.

## Span-naming conventions

Reading these docs and adding new spans? Follow:

1. **Prefix every span with `cb.`** — separates our spans from
   library-emitted ones in Jaeger's operation dropdown.
2. **`cb.<service>.<subsystem>.<verb>`** — e.g.,
   `chromeless.controller.session.assign`. The dotted hierarchy is what the
   Jaeger UI uses to group related operations.
3. **Verbs are present-tense imperatives**: `assign`, `replenish`,
   `relay`, `connect`. Past-tense (`assigned`) implies the span
   wraps the result, not the work.
4. **Attributes use `<noun>.<field>`** — `session.name`, `tenant.id`,
   `pool.namespace`. Match Prometheus label names where they exist
   so a drilldown from a metric panel keeps the label semantics.
5. **One root per request boundary** — the client's `chromeless.client.connect`
   is the root for a session lifecycle; subsequent spans are
   children. Don't open a fresh root mid-flow; that breaks the
   distributed trace.

## Files

- `signaling/tracing.go` — webrtc-dev; OTel SDK init + propagator
- `infra/controllers/browser-session-controller/pkg/tracing/tracing.go`
  — same shape, controller copy
- `capture/chromeless-metrics-sidecar/tracing.go` — same shape, sidecar copy
- `capture/chromeless-metrics-sidecar/stats_handler.go` — extracts
  `cb_trace.traceparent` from envelope, opens
  `chromeless.client.stats.received` span
- `client/src/tracing.ts` — browser-side OTel via
  `@opentelemetry/sdk-trace-web` + OTLP/HTTP exporter
- `infra/compose.yaml` — `jaeger:` service, observability profile
- `infra/k8s/observability-jaeger.yaml` — K8s deployment
- `docs/protocols/stats-channel.md` — `cb_trace` envelope field (v1.2)

## Cross-references

- T48 — auth tokens (`chromeless.signaling.auth.verify` carries the verified
  subject)
- T82 — tenant / session labels propagated alongside trace attributes
- T87 / T94 — multi-region; `region` is both a span attribute and a
  metric label
- T66 — Grafana dashboards; chromeless-session-detail has a "View trace"
  link templated against the active Jaeger / Tempo URL

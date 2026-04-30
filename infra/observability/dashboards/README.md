# Grafana dashboards for cloud-browser-webrtc

T66 ships two Grafana 11+ dashboards that turn the metrics from T38 +
T48 into something humans can read.

| Dashboard | UID | What it's for |
|---|---|---|
| `cb-cluster-overview.json` | `cb-cluster-overview` | Operator's first stop. Active sessions, session lifecycle rates, signaling close codes, assignment latency (T50, empty until controller ships), aggregated outbound bitrate, auth failures. |
| `cb-session-detail.json` | `cb-session-detail` | Drill-down for one chromium pod / sidecar instance. Pick the instance via the `$instance` template variable. Per-session bitrate / fps / dropped frames / QP / RTT / packet loss / CPU / RSS, plus a placeholder panel for `cb_webrtc_quality_limitation_fraction` (reserved). |

## Quickstart — run with the included observability stack

The compose stack ships an opt-in `observability` profile that brings up
Prometheus + Grafana auto-wired against this repo's metrics
endpoints. From the repo root:

```
docker compose -f infra/compose.yaml --profile observability up
```

Then:

- Grafana: <http://localhost:3001> (admin / admin; disable-anonymous in
  prod, obviously).
- Prometheus: <http://localhost:9090>.
- The two cb-* dashboards land under
  *Dashboards → cloud-browser-webrtc/*.

The default profile (no `--profile`) skips Prometheus/Grafana so day-to-day
local dev isn't paying for them.

## Manual import (existing Grafana)

The dashboards are plain Grafana 11 schema-39 JSON. Either:

1. **Grafana UI**: *Dashboards → New → Import* → upload the JSON →
   pick your Prometheus datasource.
2. **CLI** (`grafana-cli` against an HTTP API):
   ```
   curl -fsS -u "$GRAFANA_USER:$GRAFANA_PASS" \
     -H "Content-Type: application/json" \
     -d @infra/observability/dashboards/cb-cluster-overview.json \
     "$GRAFANA_URL/api/dashboards/db"
   ```
   (Wrap each JSON file in `{"dashboard": ..., "overwrite": true}` if
   your Grafana rejects bare-dashboard payloads.)
3. **Provisioning** in your own deploy: copy the JSON files into the
   path your `dashboards.yaml` provider points at, plus a datasource
   provisioning entry for the Prometheus this repo's metrics flow to.

## Prometheus scrape config

If you use your own Prometheus rather than the in-profile one, the
two scrape targets are:

```yaml
- job_name: cb-signaling
  metrics_path: /metrics
  static_configs:
    - targets: ['<signaling-host>:8080']
- job_name: cb-chromium
  metrics_path: /metrics
  static_configs:
    - targets: ['<chromium-host>:9100']
```

Mirror this in K8s with a `ServiceMonitor` (Prometheus Operator) or
`prometheus.io/scrape: "true"` annotations + Pod-discovery scrape
config — see `infra/observability.md` (T38) for the full reference and
`infra/k8s/signaling-deployment.yaml` for how those annotations land on
the K8s side.

## Adding a new panel

1. Edit the dashboard in the Grafana UI, save it. Note the dashboard
   has `editable: true` so this works against the provisioned copy
   too.
2. *Dashboard settings → JSON model → Copy*. Paste over the file in
   `infra/observability/dashboards/`.
3. Bump `version` (Grafana increments automatically when you save in
   the UI; bumping again post-export keeps the hand-edited copy
   distinguishable).
4. If you added a metric that didn't exist before, also update
   `infra/observability.md` so the metric reference there stays in
   sync. Two places will diverge otherwise.

## What's deliberately not in this set yet

- **Alert rules** for any of the panels. Phase 3 task; alerts belong
  in the deploy repo, not in the image.
- **PromQL recording rules** for high-cardinality joins
  (e.g. session-by-tenant). Those want session_id labels, which the
  T50 controller will mint. Once that lands, we add them here.
- **A network-path dashboard** (TURN throughput, candidate-pair stats
  beyond RTT). Need TURN-side metrics first — separate task.

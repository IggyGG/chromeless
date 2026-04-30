# Multi-region deployment + signaling federation (Phase 3+ design)

> **Status:** design only. Phase 1 is single-region by deliberate
> choice. This doc fixes the shape Phase 3+ takes when we have to
> cross geographies — so when that work starts, it can move fast
> rather than re-litigating the architecture mid-implementation.

[`docs/operations/runbook.md`](./runbook.md) covers single-cluster
ops; this doc covers everything that changes when we have *more
than one* cluster, in different regions, serving different users.

## 1. Why multi-region

The v1 latency contract from
[`PROJECT_BRIEF.md`](../../PROJECT_BRIEF.md) and
[`docs/v1-success-criteria.md`](../v1-success-criteria.md) is:

| Path | Target | Implication |
|---|---|---|
| Glass-to-glass, LAN | < 100 ms | one cluster, one rack |
| Glass-to-glass, regional | < 200 ms | one cluster per ~3000 km radius |

A single-cluster deploy can't meet 200 ms regional for users far
from the cluster. Round-figure numbers from typical IaaS providers
(AWS, GCP, Azure, in 2026) for cross-region RTT to a US-east-1
cluster:

| User location | RTT (one way × 2) | Verdict |
|---|---|---|
| US-east-1 (same region) | ~5 ms | LAN-tier, well inside budget. |
| US-west-2 | ~70 ms | Regional-tier, fine. |
| eu-west-1 | ~85 ms | Regional-tier, near the budget edge for everything but the LAN-tier user. |
| eu-central-1 | ~110 ms | Tight; encode + jitter eats the rest. |
| ap-northeast-1 (Tokyo) | ~165 ms | Past 200 ms with even modest encode delay. |
| ap-southeast-2 (Sydney) | ~195 ms | Effectively dead — interactive use is unpleasant. |

So: **for a global Phase 3+ user base, we need clusters in at least
North America, Europe, and Asia-Pacific.** Anything less and we cede
chunks of the user base to "the cloud-browser is laggy" — which
violates the project's latency-is-the-product guiding principle.

## 2. Region selection — client-side

Three approaches, evaluated against the v1 emphasis on simple paths
first:

### Approach A — GeoDNS (recommended default)

`browser.cloud-browser-webrtc.io` resolves via GeoDNS / Anycast to
the nearest regional ingress. The signaling Ingress in each
region's [`infra/k8s/signaling-deployment.yaml`](../../infra/k8s/signaling-deployment.yaml)
takes the same hostname, terminated regionally.

- **Pros:** transparent to the client; works with vanilla
  fetch + WS code; no UI to maintain.
- **Cons:** GeoDNS isn't perfect (mobile carriers route oddly,
  some users sit behind corporate proxies in a different
  continent than their browser).
- **Implementation:** Cloudflare GeoSteering, Route 53 latency-based
  routing, or NS1 Filter Chain — all support this shape.

### Approach B — explicit region selection in the UI

The user picks `[US-East / US-West / EU / APAC]` in a dropdown. The
client passes that to `client/main.ts` as a config knob (the
existing `client/dist/config.js` template can carry a `region:` field).

- **Pros:** transparent intent; useful when the user *knows* their
  GeoDNS is wrong.
- **Cons:** another UI element to localise + train users on.

### Approach C — probe-based selection

Client fires HEAD requests to `<region>.cloud-browser-webrtc.io`
endpoints, measures round-trip times, picks the fastest. T82's
client-side stats sampling is adjacent infrastructure.

- **Pros:** adapts to actual network reality (mobile carriers,
  CDNs, BGP weirdness).
- **Cons:** N HEAD requests of latency before the first session;
  the user experience suffers if N > 3.

### Recommendation

**A as the default + B as an opt-out.** Approach C goes on the
Phase 4+ wishlist when we have telemetry showing GeoDNS is mis-routing
> 5% of users.

The same DNS records double as `signaling.<region>.cloud-browser-webrtc.io`
for diagnostic per-region URLs and for B's UI dropdown.

## 3. Signaling federation

**Sessions are region-local.** Once a user picks (or is GeoDNS'd to)
a region, their entire session — signaling WS, browser-side WebRTC
peer, TURN allocation — stays in that region.

- **No cross-region signaling.** Each region runs its own signaling
  cluster ([`infra/k8s/signaling-deployment.yaml`](../../infra/k8s/signaling-deployment.yaml)),
  its own controller, its own session pool. The signaling tier in
  one region knows nothing about sessions in another.
- **No cross-region session migration.** A user who roams from
  US-East to US-West mid-session loses their session and reconnects
  fresh. Reasoning: the `RTCPeerConnection` has region-locked SSRCs,
  ICE candidates, and TURN allocations; "moving" a session means
  rebuilding all three, which is what a fresh connection already
  does. Defer real migration to Phase 4+.
- **Tenant pubkey is global.** A central auth service issues
  Ed25519 keypairs (T48 contract); the same pubkey is configured
  in every region's signaling Secret + every region's TURN issuer
  Secret. This is fine because:
  - The pubkey is a *verifier*; rotation can roll across regions
    independently with the existing
    [`infra/turn-issuer/secret-rotation.md`](../../infra/turn-issuer/secret-rotation.md)
    process scoped per region.
  - Tokens carry an `aud` (audience) claim listing allowed regions
    (see § 5 below).

The federation between signaling clusters is *minimal*: a shared
auth pubkey + shared image registry. Anything beyond that is
operational coordination (alerts, dashboards) handled at the
observability layer (§ 6).

## 4. TURN topology

TURN is the relay between the cloud-browser pod and the user's
browser. Both ends benefit from regional placement, and the gain
is asymmetric:

- **Cloud-browser pod ↔ TURN relay**: same region, ~5 ms RTT.
  Encoding overhead dominates.
- **TURN relay ↔ user**: depends on user network. Regional
  placement gets us to ~30-100 ms for in-region users.

So: **per-region TURN clusters, each with their own
[`infra/turn-issuer`](../../infra/turn-issuer/) and coturn
StatefulSet.** The per-region issuer holds a per-region HMAC
shared secret; coturn validates against that secret only. **No
secret sharing across regions.**

Why no shared coturn:
- TURN allocations carry per-allocation UDP socket state. UDP
  doesn't TCP-style "redirect" mid-flow — moving an allocation
  across regions means dropping the relay and re-allocating, at
  which point the user's browser sees ICE failure and reconnects
  through the new region's TURN. That's exactly what a fresh
  connection does already.
- A shared coturn would force every region's traffic through one
  cluster's network, completely defeating the regional placement.

T76's [`secret-rotation.md`](../../infra/turn-issuer/secret-rotation.md)
runs per-region. Operationally: rotate one region at a time during
business hours for that region.

### Managed-TURN consideration

Cloudflare TURN, metered.ca, and Twilio all advertise their own
PoPs and route automatically. If we adopt one of these instead of
self-hosted coturn, we sidestep the per-region operational burden
— but we accept a third-party dependency. Phase 3 design can defer
this choice; the same `turn-issuer` daemon (T76) issues credentials
either way (the issued credentials are valid against any HMAC-REST
TURN implementation).

## 5. Tenant onboarding + region-locking

Phase 3 multi-tenancy (T67 namespacing + T48 auth) extends with a
**region-locking** concept:

- **Token claim**: tokens issued by the central auth service carry
  `aud: ["us-east-1", "us-west-2"]` listing the regions a tenant is
  allowed in. Each region's signaling verifies that its own region
  is in `aud`; rejects with `region_not_allowed` reason on the
  `cb_signaling_auth_failures_total{reason}` metric.
- **Default-region claim**: tokens also carry `dr: "us-east-1"`
  (default region). Approach A's GeoDNS still routes to nearest;
  the default-region claim is a hint for Approach B's UI dropdown
  ("you're in EU-West but your tenant default is US-East — switch?").
- **Compliance / data residency**: a tenant with EU-only data
  residency requirements gets `aud: ["eu-west-1", "eu-central-1"]`
  only; their token won't be accepted by US-region signaling at
  all. Per-tenant region-allowlist enforcement is the central
  policy hook for GDPR / SCC compliance.

Implementation hook:
- Extend [`signaling/auth.go`](../../signaling/auth.go)'s `Claims`
  struct with `Aud []string` and `DefaultRegion string` fields.
- Add `CBWRTC_REGION` env to signaling + turn-issuer; signaling's
  verifier rejects when `claims.Aud` doesn't include
  `os.Getenv("CBWRTC_REGION")`.
- Surface `region` as a label on `cb_signaling_*` metrics.

These are two-line changes per service; the design is intended to
land cleanly when Phase 3 multi-region work begins.

## 6. Operational complexity

Multi-region multiplies the operational surface area. Choose where
to centralise carefully:

### Observability — federate

**Per-region Prometheus + per-region Grafana would force operators
to context-switch every time they investigate.** Instead:

- Per-region Prometheus *as today* (each region scrapes its own
  signaling + chromium sidecars + turn-issuer + controller).
- One global Prometheus that *federates* (`/federate` query) the
  per-region servers. The federate query pulls a curated subset
  (the `cb_*` metrics, not the full controller-runtime suite) so
  the global server doesn't choke.
- One global Grafana with the cb dashboards from
  [`infra/observability/dashboards/`](../../infra/observability/dashboards/).
  Every metric carries a `region` external label
  (set in each region's prometheus config); dashboards add a
  `$region` template variable filtering all queries.
- Per-region Grafana *also* deployed for the in-region on-call who
  doesn't need the cross-region view.

This shape doesn't require new code — it's a Prometheus federation
config change + a `region` external label on each
[`infra/observability/prometheus/prometheus.yml`](../../infra/observability/prometheus/prometheus.yml).

### Alerting — regional routing

[`infra/observability/alerts/cb-alerts.yaml`](../../infra/observability/alerts/cb-alerts.yaml)
already labels alerts with `severity`. Add `region` from the
external label so AlertManager's routing tree can route per-region:

```
route:
  group_by: [alertname, region]
  routes:
    - match: { region: us-east-1 }
      receiver: pager-us-east-oncall
    - match: { region: eu-west-1 }
      receiver: pager-eu-oncall
    - match: { region: ap-northeast-1 }
      receiver: pager-apac-oncall
```

PagerDuty rotations follow the user-facing time-of-day for that
region — APAC pager wakes APAC on-call, not US.

### Image rollout — canary per region

Rollout pattern, top-down:

1. Push to one canary region (typically the one with the smallest
   user base — often `eu-central-1` or `ap-southeast-2` for us).
2. Watch the canary's `cb_*` SLO metrics for 30+ minutes against
   the per-region Grafana dashboard.
3. If clean, ripple to the second region. Watch.
4. Continue. Total rollout: ~3-4 hours for a global ripple, vs
   ~10 minutes for a single-region all-at-once.

The CI/CD pipeline ([`.github/workflows/release.yml`](../../.github/workflows/release.yml))
pushes the same image to a multi-region registry mirror; per-region
Argo CD / Flux applications pull at the canary's pace. Image
**signing + SBOM** stays at one image (one digest), regions are
identical bits.

### Cluster autoscaling — independent

Each region's autoscaler is independent. Don't try to "balance"
warm-pool counts across regions automatically; the regional
demand profiles are different and the cross-region traffic has
already been shed at GeoDNS.

## 7. Explicitly out of scope (Phase 4+)

- **Active session migration.** A user roaming from US-East to
  US-West keeps their session. Requires solving WebRTC SSRC
  rebinding and TURN allocation re-issue mid-session. Hard problem;
  worth it only after we've measured how often roaming actually
  happens in practice. Phase 4+.
- **Cross-region TURN failover.** A region's TURN cluster fails;
  in-flight allocations *should* migrate to a healthy region
  without the user reconnecting. Same SSRC-rebinding problem,
  same Phase 4+.
- **Edge POP placement.** TURN at every CDN edge (~50 POPs)
  instead of regional (~3 regions). Reduces user-side latency by
  10-30 ms but multiplies the operational matrix by ~15×. Worth
  benchmarking when we're measurably losing users to last-mile
  network hiccups, not before.
- **Multi-cloud federation.** Half the regions on AWS, half on GCP,
  failing over between providers. Phase 5+ at minimum; the cross-
  cloud egress costs alone make this a different conversation.

## 8. Cross-references and follow-up tasks

When Phase 3 multi-region work starts, these are the integration
points:

| Existing artifact | Multi-region change |
|---|---|
| [T48 auth (`signaling/auth.go`)](../../signaling/auth.go) | add `Aud []string` + `DefaultRegion string` to `Claims`; reject when `os.Getenv("CBWRTC_REGION")` not in `Aud`. |
| [T67 tenant labels (`signaling/metrics.go`)](../../signaling/metrics.go) | add `region` label to all metrics; emit via `external_labels` in Prometheus config. |
| [T76 TURN issuer (`infra/turn-issuer`)](../../infra/turn-issuer/) | per-region deploy; per-region HMAC secret; the issuer's existing `CBWRTC_TURN_SHARED_SECRET` becomes a per-region value. |
| [T50 Helm chart (`infra/helm/`)](../../infra/helm/cloud-browser-webrtc/) | per-region overlay/values file (`values-<region>.yaml`); CI installs each region with that file. |
| [T80 runbook](./runbook.md) | per-region drill-down sections; "rotating secrets" gains a per-region step. |
| [T84 release pipeline](../../.github/workflows/release.yml) | push to a multi-region registry mirror; canary rollout via Argo CD per region. |
| [T66 Grafana dashboards](../../infra/observability/dashboards/) | add `$region` template variable; queries get `{region=~"$region"}` filter. |
| AlertManager rules | route by `region` label to regional rotations. |

Phase 3 implementation tasks (filed when we start):
1. **T-MR1**: extend `Claims` with `Aud` + `DefaultRegion`; signaling verifier honours them.
2. **T-MR2**: per-region Helm values + per-region image rollout pipeline.
3. **T-MR3**: federated Prometheus topology + `$region` template variable across dashboards.
4. **T-MR4**: GeoDNS provisioning (Cloudflare or Route 53 — pick at deploy time).
5. **T-MR5**: tenant region-allowlist policy hook + admin UI.

Each is on the order of a week of work. The total Phase 3
multi-region story is therefore in the 4-6 week range from the
first start to a global-ready system, *without* implementing any
of the Phase 4+ items (roaming, TURN failover, edge POPs).

---

This document is design only. The goal is that when team-lead
schedules Phase 3 multi-region work, the implementation tasks above
read like obvious next steps rather than open questions.

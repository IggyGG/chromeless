# Session controller design (Phase 3)

The session controller is a Kubernetes operator that brokers user
requests for cloud-browser sessions onto Pods built from
[`cloud-browser-session.yaml`](./cloud-browser-session.yaml). This
doc fixes the design; the binary is a follow-up task and is
deliberately out of scope for T50.

## Why an operator and not raw Deployments

A `Deployment` of N session pods would be the wrong shape:

- Sessions are **single-tenant**: one user occupies one Pod for
  potentially the whole session lifetime. We don't want the
  Deployment's reconciler treating Pods as fungible replicas.
- We need **claim semantics** ("give me a session now"), not just
  capacity management. That's an operator-shaped problem.
- Idle eviction is **per-Pod**, not "scale down the deployment to N-1
  pods (which one?)".

The operator also lets us evolve the assignment policy — round-robin
to least-loaded node, affinity to the user's region, GPU vs CPU pool
selection — without churning the user-facing API.

## Custom resources

Two CRDs.

### `BrowserSession` — one per active session

```yaml
apiVersion: cloud-browser-webrtc.example.com/v1alpha1
kind: BrowserSession
metadata:
  name: session-2026-04-30-abc123
  namespace: cloud-browser-webrtc
spec:
  tenantId: "user-42"
  region: "eu-west-1"
  idleTimeoutSeconds: 600
  resources:
    cpuRequest: "2"
    memRequest: "4Gi"
status:
  phase: "Assigned"   # Pending | Warming | Assigned | Draining | Ended
  podName: "cb-session-pool-7"
  signalingURL: "wss://signaling.example.com/ws/session-2026-04-30-abc123"
  startedAt: "2026-04-30T09:30:00Z"
  lastActivityAt: "2026-04-30T09:42:18Z"
  endedAt: null
  endReason: null
```

The controller reconciles `spec.phase` toward `Assigned`. Status
fields are write-by-controller, read-by-clients (the API gateway
returns these to the user as connection info).

### `BrowserSessionPool` — the warm pool

```yaml
apiVersion: cloud-browser-webrtc.example.com/v1alpha1
kind: BrowserSessionPool
metadata:
  name: default-pool
  namespace: cloud-browser-webrtc
spec:
  warmReplicas: 5
  maxSessions: 100
  reuseStrategy: "ScrubAndReturn"   # see § Warm vs cold path
  template:
    # Inline reference to a Pod template — same shape as
    # cloud-browser-session.yaml. The controller materialises Pods
    # from this template, labelled cb.session/state=warm.
    spec: { ... }
status:
  warm: 5
  active: 17
  draining: 1
  totalEverProvisioned: 4218
```

A cluster could have multiple pools (e.g. one per region, one for
GPU-enabled tenants).

## Architecture

```
┌────────────────────────────────────────────────────────────────┐
│  API gateway  (cloud-browser-api / outside scope of this task) │
│  - authenticates user                                          │
│  - POST /sessions  → creates BrowserSession CR                 │
│  - GET  /sessions/{id} → reads status, returns to user         │
└──────────────────────────┬─────────────────────────────────────┘
                           │
                           ▼
┌────────────────────────────────────────────────────────────────┐
│  session-controller (this design)                              │
│  - watches BrowserSession + BrowserSessionPool                 │
│  - assignment loop:                                            │
│      pick a warm Pod -> bind to BrowserSession ->              │
│      label it cb.session/state=assigned ->                     │
│      patch status with podName + signalingURL                  │
│  - replenishment loop:                                         │
│      keep len(warm) >= spec.warmReplicas                       │
│  - eviction loop:                                              │
│      session status.lastActivityAt older than                  │
│      spec.idleTimeoutSeconds → mark Draining                   │
│  - drain loop:                                                 │
│      Draining sessions get a SIGTERM via DevTools              │
│      Page.close + supervisorctl shutdown; pod removed          │
└──────────────────────────┬─────────────────────────────────────┘
                           │
                           ▼
┌────────────────────────────────────────────────────────────────┐
│  Pods labeled cb.session/state ∈ {warm, assigned, draining}    │
│  Each pod is the cloud-browser-session.yaml shape (T50).       │
└────────────────────────────────────────────────────────────────┘
```

## Assignment flow

1. **API gateway** receives `POST /sessions` (auth provided by T48
   signed session tokens once that lands).
2. Gateway creates a `BrowserSession` CR with `spec.tenantId` and
   defaults; the CR's `status.phase` starts as `Pending`.
3. **Controller assignment loop**:
   - List Pods in the target pool with `cb.session/state=warm`,
     filtered by node region/zone matching the session's region
     spec.
   - Pick one (round-robin within the candidate set; in v1 we don't
     need anything cleverer).
   - Atomically: label the pod `cb.session/state=assigned`, set
     `cb.session/owner=<session-name>`; patch the CR's
     `status.podName` and `status.signalingURL`.
   - On failure (no warm pod available), fall through to a cold
     start: create a fresh Pod from the pool template, wait for it
     to pass readiness, then proceed as above. Cold start adds
     ~30-45 s for first-boot grace per the chromium-container
     start_period.
4. Gateway polls / watches the CR's status until `phase=Assigned`,
   then returns the connection info to the user.

The whole flow is < 1 s when the warm pool has capacity, ~30 s on
cold start. We expose this latency on
`cb_session_assignment_seconds` (Prometheus), bucketed by
warm-vs-cold path.

## Replenishment

After every assignment, the controller checks the warm pool size
against `spec.warmReplicas`. If short, it creates new Pods from the
template up to the target. New Pods come up labeled
`cb.session/state=warm` and are visible to the next assignment loop
once they pass readiness.

Replenishment runs in an independent goroutine on a 5 s tick so
it doesn't queue behind a slow assignment.

## Warm vs cold path on session end

Two policies, picked per pool:

### `ScrubAndReturn` (warm path)

- Session ends → controller calls Pod's `restart.sh`
  (`supervisorctl restart chromium`), which kills Chromium with
  group/process cleanup and supervisord restarts it under
  `launch-chromium.sh` with a fresh user-data-dir (cold-start.sh
  also runs again at the next pod boot if the whole supervisord
  comes down, though restart of just chromium is faster).
- Label flips back to `cb.session/state=warm`.
- Pros: ~5-15 s back to ready vs 30-45 s for full pod recreation.
  Massive cost saving at scale.
- **Cons (security):** Chromium and the kernel both leak state.
  After a renderer-side compromise has broken out of Chromium's
  sandbox, "scrubbing" the user-data-dir is not enough; we are
  trusting a runc + AppArmor + Seccomp boundary to contain the
  attacker between tenants. Per [`docs/research/sandbox-isolation.md`](../../docs/research/sandbox-isolation.md)
  this is exactly the threat we don't trust runc to defend against.

`ScrubAndReturn` is **only** safe under one of:
1. The session controller is single-tenant (every BrowserSession is
   the same tenant; e.g., enterprise deploy where one company
   shares a pool).
2. The pod runs under gVisor/Kata + the sandbox is the trust
   boundary, not the container runtime.

### `RecreatePod` (cold path)

- Session ends → controller deletes the Pod. K8s schedules a fresh
  one in its place via the replenishment loop.
- Pros: clean isolation. Each tenant gets a fresh kernel-level
  view.
- Cons: cold start eats the latency budget.

**Default for v1: `RecreatePod`.** We optimize the cold path (image
size, prewarming, snapshot/restore via CRIU as a future stretch)
before we reuse pods across tenants.

## Idle eviction

Two layers cooperate:

1. **Pod-level** (T31's `idle-watchdog.sh`): runs *inside* the pod,
   polls the streamer page's `window.pc.connectionState`, and
   asks supervisord to shut down after `IDLE_TIMEOUT_S` of idle.
   This is what kills a pod when the user closes their tab.

2. **Cluster-level** (this controller): tracks
   `BrowserSession.status.lastActivityAt`, refreshed via signaling
   server heartbeats (Phase 3 follow-up: signaling pushes a
   per-session timestamp into the CR via a small webhook). If
   `now - lastActivityAt > spec.idleTimeoutSeconds`, mark the
   session `Draining`.

When the pod-level watchdog hits first, the pod terminates; K8s
notifies the controller via Pod status, which transitions the CR to
`Ended` with `endReason=PodEviction`. When the cluster-level loop
hits first, the controller initiates a graceful shutdown via
DevTools `Page.close` + supervisord shutdown, then deletes the pod.

The two layers exist for different failure modes:
- **Pod-level only** misses the case where Chromium is alive but
  the user is gone (e.g., they tabbed away and closed the laptop;
  pod stays running until the watchdog's grace expires; the pod
  doesn't know about user-side activity at the cloud edge).
- **Cluster-level only** misses the case where the pod is wedged
  (Chromium frozen, signaling alive but stale); pod-level catches
  these via process-state checks.

For the K8s controller, the pod-level watchdog should be set to
`IDLE_TIMEOUT_S` 1.5x to 2x the cluster-level value. The
cloud-browser-session.yaml manifest in this directory disables the
pod-level watchdog (`IDLE_TIMEOUT_S=999999`) entirely and lets the
controller own idle: simpler, no race.

## Failure modes and reconciliation

| Failure | Detection | Reconciliation |
|---|---|---|
| Pod evicted by node | Pod status changes / disappears | CR → `Ended(reason=Evicted)`; gateway returns new connection on next user action |
| Node becomes NotReady | Node status | Drain warm pods on that node first; fall through to cold start |
| Controller pod crashes | K8s liveness | Deployment restarts; on startup the controller re-syncs all CRs from the API server (no in-memory state) |
| Controller flapping (split brain) | Lease (`coordination.k8s.io`) | Use a lease per controller pool; only the leader runs the assignment loop |
| User abandons session mid-handshake | `status.phase=Assigned` but no signaling activity within 60 s | Mark `Draining` on a fast-path timeout |

## Observability

The controller exposes its own `/metrics`:

- `cb_session_pool_warm_count{pool}` — gauge, current warm pods.
- `cb_session_pool_active_count{pool}` — gauge, currently assigned.
- `cb_session_pool_draining_count{pool}` — gauge, draining.
- `cb_session_assignment_seconds_bucket{pool, path}` — histogram,
  warm vs cold latency.
- `cb_session_assignment_failures_total{pool, reason}` — counter.
- `cb_session_evictions_total{pool, reason}` — counter (idle vs
  pod-failure vs explicit).

These compose with the per-session metrics from T38 (`cb_chromium_*`
and `cb_webrtc_*` from `cb-metrics-sidecar`) and the signaling
metrics (`cb_signaling_*`) for a full pipeline view.

## Cluster-level network isolation

Independent of the controller, every session pod gets a
`NetworkPolicy` that:

- Allows egress to `signaling.cloud-browser-webrtc.svc` on 8080.
- Allows egress to `coturn.cloud-browser-webrtc.svc` on 3478/5349
  and to the public TURN range.
- Allows egress to the public internet *except* RFC1918 ranges and
  cloud metadata IPs (`169.254.169.254`, `fd00:ec2::254`).
- Disallows pod-to-pod traffic within the namespace (sessions are
  isolated from each other and from other sessions).
- Allows ingress only from the API gateway's CIDR (for DevTools
  port-forwards from the controller).

These policies live alongside the session manifest; they are
omitted from this task scope but tracked as a follow-up.

## Open questions deferred to implementation

1. **CRD vs ConfigMap for pool config.** A CRD is more idiomatic
   but requires a CRD bundle install. ConfigMap is simpler if the
   pool is fixed.
2. **Lease topology.** One leader for all pools, or one per pool?
   Per-pool gives isolation; one global is simpler.
3. **Fast-path on scale-down.** When the warm pool is over-target,
   should we kill warmest-first (LIFO) or coldest-first (FIFO)?
   Warmer pods have hotter caches but also more accumulated
   process state.
4. **Snapshot/restore (CRIU).** Real cold-path mitigation. Not for
   this design; tracked for Phase 4.

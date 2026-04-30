# browser-session-controller (T71)

The Kubernetes operator that turns T50's design doc into a running
control plane. Watches `BrowserSession` and `BrowserSessionPool`
custom resources and reconciles them onto Pods built from
`infra/k8s/cloud-browser-session.yaml`.

## What it does

- **Maintains a warm pool** of pre-created Chromium pods. When a
  pool's `spec.warmReplicas` is N and the live count is less, the
  controller materialises new Pods from `pool.spec.template`.
- **Assigns a Pod to a session** on `BrowserSession` create. Picks
  a `cb.session/state=warm` Pod that's region-compatible and Ready,
  relabels it `assigned`, sets `cb.session/owner` to the session
  name, and surfaces connection info on
  `BrowserSession.status.connection` (signaling URL + Pod name +
  Pod IP).
- **Cold-starts a fresh Pod** when the warm pool is empty;
  transitions through `Pending → Warming → Ready` as the Pod boots.
- **Idle-evicts** sessions whose `status.lastActivityAt` exceeds
  `spec.idleTimeoutSeconds`. Marks them `Draining`, deletes the
  bound Pod, terminates with `phase: Ended` + `endReason: IdleTimeout`.
- **Drains over-aged warm pods** (`maxAgeSeconds`) so image rolls
  cycle through naturally.
- **Validates tenant claims** via an admission webhook (`pkg/server/admission.go`).
  Phase 3 wiring; the handler is shipped, the
  ValidatingWebhookConfiguration + TLS cert lands per deploy.

## Layout

```
infra/controllers/browser-session-controller/
├── cmd/controller/main.go      controller-runtime Manager wiring
├── pkg/apis/v1/                CRD types + DeepCopy
├── pkg/reconciler/
│   ├── session.go              BrowserSession reconcile loop
│   ├── pool.go                 BrowserSessionPool reconcile loop
│   ├── session_test.go
│   └── pool_test.go
├── pkg/server/
│   ├── admission.go            ValidateBrowserSession webhook
│   └── admission_test.go
├── config/crd/                 hand-authored CRD YAMLs
├── Dockerfile                  multi-stage; distroless final
├── go.mod / go.sum
└── README.md                   (this file)
```

The matching K8s manifest (Deployment + RBAC + Service) lives at
`infra/k8s/controller-deployment.yaml`.

## Build

```
cd infra/controllers/browser-session-controller
go build ./...
go test ./...
```

Container image:

```
docker build -t ghcr.io/iggy/cloud-browser-webrtc/browser-session-controller:dev \
    -f infra/controllers/browser-session-controller/Dockerfile \
    infra/controllers/browser-session-controller
```

## Deploy

The controller and its CRDs are part of the kustomize bundle at
`infra/k8s/`:

```
kubectl apply -k infra/k8s/
```

For a focused redeploy of just the controller layer:

```
kubectl apply -f infra/controllers/browser-session-controller/config/crd/
kubectl apply -f infra/k8s/controller-deployment.yaml
```

## Test locally with kind / k3d

The fastest path on a developer laptop:

```
# 1. Spin up a cluster
kind create cluster --name cb

# 2. Install CRDs
kubectl apply -f infra/controllers/browser-session-controller/config/crd/

# 3. Build + load the controller image into kind
docker build -t cb-controller:dev \
    -f infra/controllers/browser-session-controller/Dockerfile \
    infra/controllers/browser-session-controller
kind load docker-image cb-controller:dev --name cb

# 4. Deploy controller (override image to the locally-loaded tag)
kubectl create namespace cloud-browser-webrtc
kubectl apply -f infra/k8s/controller-deployment.yaml
kubectl -n cloud-browser-webrtc set image deployment/browser-session-controller \
    controller=cb-controller:dev

# 5. Apply a sample pool + session
kubectl apply -f docs/k8s/sample-pool.yaml      # see below
kubectl apply -f docs/k8s/sample-session.yaml

# 6. Watch
kubectl -n cloud-browser-webrtc get browsersession -w
```

`sample-pool.yaml` and `sample-session.yaml` are not in this repo;
build them ad-hoc from the `BrowserSessionPool` and `BrowserSession`
shapes in `pkg/apis/v1/types.go`. A real deploy will template these
from your platform's CD pipeline.

For tighter iteration, run the controller out-of-cluster against
your kind cluster:

```
KUBECONFIG=$(kind get kubeconfig --name cb) \
    LEADER_ELECT=false \
    go run ./cmd/controller
```

## RBAC

The controller's ServiceAccount needs (full list in
`infra/k8s/controller-deployment.yaml`):

- `cloud-browser-webrtc.example.com/browsersessions[/status,/finalizers]` — get/list/watch/CRUD
- `cloud-browser-webrtc.example.com/browsersessionpools[/status]` — get/list/watch + status update
- `core/pods[/status]` — get/list/watch/CRUD on Pods (cluster-wide)
- `core/events` — create/patch (audit trail in `kubectl describe`)
- `coordination.k8s.io/leases` — for leader election

`ClusterRole` rather than `Role` because session Pods may land in
namespaces other than the controller's; if you restrict sessions to
one namespace, downgrade to a Role.

## Observability

The controller exposes Prometheus metrics on `:8080/metrics`,
including the standard controller-runtime suite
(`controller_runtime_reconcile_total{controller}`,
`workqueue_*`, etc.).

`/healthz` and `/readyz` on `:8081` are wired to the manager's
default `healthz.Ping` so the K8s probes fire on the lease + cache
status.

## Status of features

| Feature | State | Notes |
|---|---|---|
| Empty-pool cold start | ✓ | Tested; `TestSession_EmptyPool_ColdStart`. |
| Warm-pool fast assignment | ✓ | Tested; `TestSession_WarmPool_FastAssignment`. |
| Idle eviction | ✓ | Tested; `TestSession_IdleEviction`. Activity timestamp comes from session's status; Phase 3 will wire the signaling server's per-session activity webhook to push into `status.lastActivityAt`. |
| Pool replenishment | ✓ | Tested; `TestPool_Replenishes`. |
| Pool aging / drain | ✓ | Tested; `TestPool_DrainsOverAged`. |
| ScrubAndReturn recycle | – | Wire next; today every recycle goes through `RecreatePod`. Documented as the secure-default in T50. |
| Admission webhook | scaffold | Logic + tests landed; TLS + ValidatingWebhookConfiguration are deploy-side. |
| Snapshot-aware fast path (T68) | – | The pod's `cb.io/snapshot-id` annotation is recognised; the controller does not yet route to a snapshot-restore helper. Phase 3 follow-up. |

## Limitations

- **Activity timestamps come from `status.lastActivityAt`,** which
  no one writes today. Until the signaling server pushes activity
  via a webhook (Phase 3), idle eviction effectively measures
  *time since assignment*, not *time since user last did something*.
  Alternative: extend the controller to poll
  `cb_signaling_active_connections{tenant}` and back-fill, but that
  ties the controller to a specific scrape topology — not preferred.
- **Round-robin assignment** within a candidate set is implicit (the
  fake/real Lister doesn't guarantee order). Adequate for v1; add a
  ring-buffer / least-recently-assigned heuristic if hot-spotting
  becomes visible in dashboards.
- **Pod-level idle watchdog (T31)** still runs by default, with
  `IDLE_TIMEOUT_S=999999` set in the K8s manifest. The controller
  owns idle eviction in cluster contexts; the in-pod watchdog is
  the fallback for `docker run` / non-K8s deploys.

## Cross-references

- T50 — design doc this implementation tracks.
- T68 — CRIU snapshot/restore; the controller will eventually drive
  the fast-path on `cb.io/snapshot-id`.
- T48 — signaling auth; admission webhook reads tenant claims that
  T48's tokens carry.
- T57 — security hardening; the controller pod itself runs under
  RuntimeDefault seccomp + readOnlyRootFilesystem.

# Chromeless operational runbook

Step-by-step recipes for the operational scenarios you'll hit in
production. Pairs with [`sla.md`](./sla.md) (which alert means what)
and [`phase1-deployment-checklist.md`](./phase1-deployment-checklist.md)
(what to verify before a fresh deploy).

## Table of contents

- [Deploying](#deploying) — fresh K8s cluster to running stack
- [Rolling back](#rolling-back) — revert a bad image push
- [Scaling](#scaling) — add or remove capacity
- [Debugging a slow session](#debugging-a-slow-session) — latency triage
- [Checking encoder health](#checking-encoder-health) — quality triage
- [Rotating secrets](#rotating-secrets) — TURN secret, auth pubkey, TLS
- [Draining a node](#draining-a-node) — graceful node maintenance
- [Snapshot management](#snapshot-management) — T68 CRIU operations

---

## Deploying

From a fresh K8s cluster (kind, k3d, GKE, EKS, AKS — whatever) to a
running chromeless.

### 0. Prerequisites

- A cluster ≥ K8s 1.27.
- Container registry credentials (push images you build locally
  to `ghcr.io/<org>/chromeless/*` and pull from there).
- `kubectl`, `kustomize`, `docker buildx`.
- DNS records for `signaling.<your-domain>`, `turn.<your-domain>`.
- TLS issuer (cert-manager + Let's Encrypt is the default the
  manifests assume).
- Persistent storage class if you're enabling [snapshot
  management](#snapshot-management).

### 1. Build + push images

The browser image is a from-source Chromium build and cannot be produced with a
plain `docker build`: run `build/chromeless-build.sh` (4–8 h cold, ~1 h warm;
`build/README.md`), which stages a context for `build/Dockerfile.runtime`, then
push it with `infra/k8s/chromeless-build/chromeless-kaniko-push.sh`. The tag is
`cr<chromium-branch>-<repo-sha>` and the result is recorded in
`build/guest-release.json`. The `infra/Dockerfile` this section used to name
was the pre-M7 stock-Chromium image and no longer exists.

The Go services build normally:

```
docker buildx build --platform linux/amd64 \
    -t ghcr.io/<org>/chromeless/gateway:<tag> \
    -f infra/gateway/Dockerfile . --push          # context is the repo root

docker buildx build --platform linux/amd64 \
    -t ghcr.io/<org>/chromeless/signaling:<tag> \
    -f signaling/Dockerfile signaling/ --push

docker buildx build --platform linux/amd64 \
    -t ghcr.io/<org>/chromeless/browser-session-controller:<tag> \
    -f infra/controllers/browser-session-controller/Dockerfile \
    infra/controllers/browser-session-controller --push

docker buildx build --platform linux/amd64 \
    -t ghcr.io/<org>/chromeless/turn-issuer:<tag> \
    -f infra/turn-issuer/Dockerfile infra/turn-issuer --push
```

The `input-bridge`, `cursor-watcher`, `clipboard-bridge` and `file-bridge`
sidecars this section used to list were retired in M7: input, cursor, clipboard
and file transfer now ride data channels terminated inside the browser process.
`chromeless-metrics-sidecar` still exists and runs inside the worker image under
supervisord.

### 2. Install seccomp profile on every node

The `chromeless` Pod references a Localhost seccomp profile
(`infra/seccomp/chromeless.json`, T57). The kubelet only finds it
if it's at `/var/lib/kubelet/seccomp/chromeless.json` on each
node. Use the [security-profiles-operator](https://github.com/kubernetes-sigs/security-profiles-operator)
or a privileged DaemonSet that drops the file in place.

### 3. Stage cluster-specific values

Create an overlay under `infra/k8s/overlays/<env>/`:
- replace image tags in `kustomization.yaml`.
- patch `signaling-deployment.yaml` Ingress hostname.
- patch `turn-deployment.yaml` realm + external-ip.
- supply `turn-issuer-secrets` Secret values: `CHROMELESS_AUTH_PUBKEY`,
  `CHROMELESS_TURN_SHARED_SECRET`, TURN URLs.
- supply `turn-rest-secret` Secret value: `static-auth-secret`
  (must match `CHROMELESS_TURN_SHARED_SECRET` above).

### 4. Apply

```
kubectl apply -f infra/controllers/browser-session-controller/config/crd/
kubectl apply -k infra/k8s/overlays/<env>/
```

### 5. Verify

Run `phase1-deployment-checklist.md` end-to-end. The smoke tests
under `tests/smoke/` cover the basic image-up-correctly path; the
dashboards under `infra/observability/dashboards/` show the metric
plumbing is alive.

---

## Rolling back

### Image rollback (controller-managed pods)

Active sessions are bound to specific Pod replicas. A bad image push
shouldn't cycle them all at once.

```
# 1. Find the bad image tag.
kubectl -n chromeless get pods -o jsonpath='{.items[*].spec.containers[*].image}' | tr ' ' '\n' | sort -u

# 2. Patch the previous tag back into the Deployments / pool.
kubectl -n chromeless set image deployment/signaling signaling=<good-tag>
kubectl -n chromeless set image deployment/browser-session-controller controller=<good-tag>

# 3. For session pods, patch the pool template.
kubectl -n chromeless patch browsersessionpool default-pool \
    --type=json \
    -p='[{"op":"replace","path":"/spec/template/spec/containers/0/image","value":"<good-tag>"}]'

# 4. Active sessions stay on their current image. New warm pods
#    materialise on the rolled-back image; the warm pool naturally
#    cycles via spec.maxAgeSeconds.
```

If rollback urgency demands cycling active sessions:
```
kubectl -n chromeless delete browsersession --all
```
This evicts every user. Use sparingly.

### Controller rollback

```
kubectl -n chromeless rollout undo deployment/browser-session-controller
```

The controller is stateless (CRDs hold the world); rolling back
is safe.

---

## Scaling

### Add signaling capacity

The signaling tier is stateless behind a ClientIP-affinity Service:

```
kubectl -n chromeless scale deployment/signaling --replicas=10
```

HPA from `infra/k8s/signaling-deployment.yaml` will also auto-scale
on CPU.

### Add session capacity

Edit the BrowserSessionPool's `spec.warmReplicas` upward. The pool
reconciler (T71) materialises new Pods to the new target.

```
kubectl -n chromeless patch browsersessionpool default-pool \
    --type=merge -p '{"spec":{"warmReplicas":50}}'
```

Watch:
- `cb_session_pool_warm_count` rises to the new target.
- `cb_chromium_cpu_pct` per host stays under 80% (else node pressure).
- `cb_session_assignment_seconds_bucket{path="warm"}` p95 stays < 1 s.

### Add capacity to a specific node pool

Standard cluster autoscaler. Watch
`kube_pod_status_phase{phase="Pending"}` to confirm Pods are landing.

### Watch when scaling

- Node CPU saturation. Software encode is CPU-hot; running > 1 session
  per core is risky.
- /dev/shm. Each session consumes 1 GiB tmpfs (T57 emptyDir
  `medium=Memory`). Sum your concurrency × 1 GiB against node memory.
- Egress UDP bandwidth. WebRTC outbound bitrate gauge multiplied by
  concurrency; spike-loadscaling beyond NIC line rate becomes packet
  loss.

---

## Debugging a slow session

Triggered by: `CBHighRoundTripTime` alert, user complaint, or a
spike in `cb_webrtc_round_trip_time_ms` on the cluster overview
dashboard.

### Step 1: identify the session

```
# From the alert, the `instance` label points at the metrics sidecar
# inside the slow Pod. The Pod name maps 1:1.
kubectl -n chromeless get pod -o wide | grep <instance-shortprefix>
```

Or open Grafana `chromeless-cluster-overview` → click the slow session →
deep-link to `chromeless-session-detail` filtered by `$instance`.

### Step 2: classify the slowdown

On `chromeless-session-detail`, look at the panels in this order:

1. **ICE RTT** (`cb_webrtc_round_trip_time_ms`). If high, problem is
   network or candidate-pair pathological (TURN relay falling over,
   peer NAT changed).
2. **Packet loss** (`cb_webrtc_remote_inbound_packets_lost_total`
   rate). High → network layer.
3. **Encoder QP** (`cb_webrtc_outbound_qp`). High → encoder is
   starving, see [Checking encoder health](#checking-encoder-health).
4. **CPU%** (`cb_chromium_cpu_pct`). > 90% → encoder CPU starvation,
   bigger node or fewer concurrent sessions per node.
5. **Memory** (`cb_chromium_rss_bytes`). Climbing > 1 MiB/s → leak.
   See `CBChromiumRSSGrowth` alert; bounce the Pod via
   `kubectl exec ... /usr/local/bin/restart.sh`.

### Step 3: per-session container logs

```
kubectl -n chromeless logs <pod> -c chromeless --tail=200
kubectl -n chromeless logs <pod> -c chromeless-metrics-sidecar --tail=100
kubectl -n chromeless logs <pod> -c input-bridge --tail=100
```

Common signals:
- `chromium.err.log`: GPU init failures, page crashes.
- `idle-watchdog.err.log`: `window.pc not found` means streamer page
  isn't mounting — go to chromium logs first.
- `input-bridge`: `dispatch failed` lines correlate with input lag.

### Step 4: harness numbers

For real glass-to-glass latency (rather than RTT proxy), run the
harness from a synthetic client:
```
cd harness/latency
python3 reconcile.py --camera /dev/video0 --duration 120
```

The harness produces a single number (ms p50/p95/p99). Compare
against [`docs/v1-success-criteria.md`](../v1-success-criteria.md).

---

## Checking encoder health

Triggered by: `CBEncoderSaturated` (QP > 38 sustained),
`CBDroppedFramesElevated` (> 1 dropped frame/s).

### Step 1: is it CPU or bandwidth?

| Signal | Interpretation |
|---|---|
| QP high + bitrate at ceiling | Encoder is at quality floor for the bandwidth budget. Either raise the bandwidth target or accept the quality. |
| QP high + bitrate well below ceiling + CPU < 80% | BWE is throttling unnecessarily. Suspect sender-side congestion control thrashing. |
| QP high + CPU at 100% on one core | Software encoder is single-thread bound. The libvpx VP9 path (T35) and x264 (T36) both hit this. Solutions: simulcast off, lower resolution, or HW encode (T63 NVENC, T69 VAAPI). |
| Dropped frames + low CPU + low loss | Encoder timing out. Check Chromium logs for `Encoder failed to encode` lines. |

### Step 2: cross-check the BWE adapter (T58)

The bandwidth-estimator → encoder adapter logs in
`chromeless/chromium.log`:

```
kubectl -n chromeless logs <pod> -c chromeless | grep "BWE"
```

A healthy session shows BWE estimates climbing on connection then
stabilising. If you see oscillation (every few seconds the estimator
flips wildly), that's a bug — file under T58.

### Step 3: codec mismatch

Was the negotiated codec what you expected? T54 implements VP9 → H.264
fallback. Check the SDP:

```
kubectl -n chromeless exec <pod> -c chromeless -- \
  curl -s http://127.0.0.1:9222/json/version
# Then via DevTools, getStats() → look at the codec field on
# outbound-rtp{kind=video}.
```

If the user's browser doesn't advertise VP9, the fallback path
should kick in — verify with `cb_webrtc_codec_negotiated{codec=...}`
when that metric ships.

---

## Rotating secrets

### TURN shared secret (T76)

Follow [`infra/turn-issuer/secret-rotation.md`](../../infra/turn-issuer/secret-rotation.md)
end-to-end. Summary: coturn first (with fallback enabled), then
issuer (with `_PREV` set), wait the longest TTL, drop fallbacks.

### Signaling auth pubkey (T48)

The pubkey is the verifier; rotation means a new keypair is issued
upstream and the new pubkey rolls in:

```
# 1. Generate new keypair upstream (in your auth issuer, not here).
# 2. Run the issuer to mint *both old and new* tokens for a brief
#    window. (How depends on your issuer.)
# 3. Roll the signaling pubkey:
kubectl -n chromeless patch secret signaling-auth \
    --type=merge -p '{"stringData":{"CHROMELESS_AUTH_PUBKEY":"<new-base64>"}}'
kubectl -n chromeless rollout restart deployment/signaling
# 4. Same for the turn-issuer:
kubectl -n chromeless patch secret turn-issuer-secrets \
    --type=merge -p '{"stringData":{"CHROMELESS_AUTH_PUBKEY":"<new-base64>"}}'
kubectl -n chromeless rollout restart deployment/turn-issuer
```

Active sessions on the old pubkey continue working until their token
exp; new connects use the new pubkey.

### TLS certificates

cert-manager auto-renews via the Ingress's `cert-manager.io/cluster-issuer`
annotation. Manual force-renewal:

```
kubectl -n chromeless delete certificate signaling-tls
# cert-manager re-creates it within 30 s.
```

---

## Draining a node

For node maintenance (kernel update, hardware swap).

```
# 1. Cordon — no new pods scheduled here.
kubectl cordon <node>

# 2. Mark the warm pods on this node as draining via the
#    BrowserSessionPool reconciler. They'll be deleted; replenishment
#    creates new ones on healthy nodes.
kubectl -n chromeless label pod \
    -l chromeless.session/state=warm \
    --field-selector spec.nodeName=<node> \
    chromeless.session/state=draining --overwrite

# 3. Wait for active sessions on this node to end naturally.
#    Check:
kubectl -n chromeless get pod -o wide \
    --field-selector spec.nodeName=<node>

# 4. Force-evict if needed (this terminates active user sessions —
#    accept the user-visible disconnect).
kubectl drain <node> --ignore-daemonsets --delete-emptydir-data
```

Phase 3 stretch: snapshot active sessions before drain (T68 CRIU)
and restore on a different node. Until that's wired into the
controller, drain == disconnect for active users.

---

## Snapshot management

Post-T68; assumes the snapshot helper is deployed on each node.

### Creating a snapshot

```
ssh <node>
sudo /opt/chromeless-snapshots/snapshot.sh chromeless-blank-$(date +%Y%m%d)
# Prints a sha. Note it.
```

Distribute the snapshot to all nodes (rsync / object storage /
DaemonSet pulling at boot).

### Validating a snapshot

```
sudo /opt/chromeless-snapshots/restore.sh <sha>
# Prints time_to_ready_ms. Should be < 2000.
```

Run `tests/smoke/snapshot-restore.sh` against a Pod that started
from this snapshot to verify functional health.

### Switching the pool to a new snapshot

```
kubectl -n chromeless patch browsersessionpool default-pool \
    --type=merge -p '{"spec":{"template":{"metadata":{"annotations":{"chromeless.io/snapshot-id":"<new-sha>"}}}}}'
```

Existing warm Pods stay on the old snapshot; new ones use the new.
The pool's `maxAgeSeconds` cycles them all over the lifetime of
that setting (default 1 hour).

### Garbage-collecting old snapshots

Snapshots that no Pod references can be deleted. List currently-in-use:

```
kubectl -n chromeless get pods \
    -o jsonpath='{.items[*].metadata.annotations.cb\.io/snapshot-id}' \
    | tr ' ' '\n' | sort -u
```

Compare against `/var/lib/chromeless-snapshots/shared/*` on each node;
delete the ones not in the live set after ≥ 24 h grace (longest
session lifetime).

Phase 3 follow-up: a small DaemonSet that does this automatically
with content-addressing as the index.

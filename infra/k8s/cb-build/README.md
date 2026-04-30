# cb-build — Chromium build environment on triform-6 (T112)

Realises T17 + T101's plan: a K8s Job pinned to triform-6 (32 CPU /
128 GiB) that runs `cb-build.sh` (T113) to produce
`cloud_browser_worker` and the encoder unit-test binary from a
Chromium source checkout + our patch series.

## Layout

| File | Purpose |
|------|---------|
| `namespace.yaml` | `cb-build` ns. Isolated from the runtime ns so build-only quotas / network policies / labels don't leak. |
| `pvc-chromium-src.yaml` | 500 GiB RWO openebs-hostpath PVC. Holds the Chromium tree, our repo as `//cloud-browser`, and `out/cb-release/`. |
| `pvc-sccache.yaml` | 100 GiB RWO openebs-hostpath PVC. Local-disk sccache; production swap to S3 backend documented in T17 §5. |
| `build-job.yaml` | The Job. nodeName=triform-6, 30 CPU / 120 GiB, OnFailure / backoffLimit=2 / 8h activeDeadline. |
| `Dockerfile.build-runner` | Build-runner image: Debian 12 + depot_tools + sccache + git + python3 + sudo. Pushed to forgejo. |
| `kustomization.yaml` | `kubectl apply -k .` entry. |

## Prereqs (one-time, per-cluster)

### 1. Build and push the runner image

```bash
# From the repo root.
docker build \
  -t registry.triform.cloud/cloud-browser-webrtc/cb-build-runner:0.1.0 \
  -f infra/k8s/cb-build/Dockerfile.build-runner \
  infra/k8s/cb-build/

# Push (requires forgejo creds; see ~/.docker/config.json or `docker login`).
docker push registry.triform.cloud/cloud-browser-webrtc/cb-build-runner:0.1.0
```

If you don't have a local docker, push from a cluster node:

```bash
# On triform-1 or any node with crictl:
ssh triform-6 \
  'sudo ctr -n k8s.io images pull registry.triform.cloud/cloud-browser-webrtc/cb-build-runner:0.1.0'
```

Bump the image tag in `build-job.yaml` (`image:` line) when you roll
the runner — never re-push `:0.1.0` after the first build, otherwise
nodes with cached layers can serve stale binaries.

### 2. Registry pull secret

`build-job.yaml` references `imagePullSecrets: [registry-pull]`.
Create it once per cluster lifetime:

```bash
# Easiest: copy from an existing namespace that already has it.
kubectl get secret -n default registry-pull -o yaml \
  | sed 's/namespace: default/namespace: cb-build/' \
  | kubectl apply -f -

# Or create from scratch:
kubectl -n cb-build create secret docker-registry registry-pull \
  --docker-server=registry.triform.cloud \
  --docker-username='<forgejo-user>' \
  --docker-password='<forgejo-token>'
```

The secret is intentionally NOT committed to git. See
`docs/security/auth.md` for the full secret-management posture.

## Apply

```bash
kubectl apply -k infra/k8s/cb-build/
```

This creates the namespace, both PVCs, and the Job. The PVCs bind
when the Job's Pod is scheduled (openebs-hostpath uses
WaitForFirstConsumer); since the Job is `nodeName: triform-6`, the
PVs land on triform-6's local disk.

Validate without applying:

```bash
kubectl apply -k infra/k8s/cb-build/ --dry-run=server -o yaml | head -40
```

## Watch the build

```bash
# Follow logs.
kubectl logs -n cb-build job/cb-build -f

# Tail a specific container (initContainer vs main).
kubectl logs -n cb-build job/cb-build -c bootstrap-our-repo -f
kubectl logs -n cb-build job/cb-build -c build               -f

# Pod status.
kubectl get pods -n cb-build -l job-name=cb-build -w

# Retry count.
kubectl get job cb-build -n cb-build -o jsonpath='{.status}' | jq
```

If the Pod is `Pending` for >2 minutes, check that triform-6 has
schedulable resources (`kubectl describe node triform-6`) and that
the PVCs are `Pending` waiting for `WaitForFirstConsumer` (this is
expected before the Pod schedules; abnormal once the Pod is running).

## Retrieve the binary after success

The Job exits 0 once `autoninja` finishes. The binary lives on the
chromium-src PVC at `/work/src/out/cb-release/cloud_browser_worker`.
Three retrieval paths, ordered by preference:

### Option A — kubectl cp (small artifacts)

```bash
# Spin up a debug Pod attached to the same PVC.
kubectl run -n cb-build cb-build-debug \
  --image=registry.triform.cloud/cloud-browser-webrtc/cb-build-runner:0.1.0 \
  --restart=Never \
  --overrides='{"spec":{"nodeName":"triform-6","volumes":[{"name":"src","persistentVolumeClaim":{"claimName":"chromium-src"}}],"containers":[{"name":"cb-build-debug","image":"registry.triform.cloud/cloud-browser-webrtc/cb-build-runner:0.1.0","command":["sleep","3600"],"volumeMounts":[{"name":"src","mountPath":"/work/src"}]}],"imagePullSecrets":[{"name":"registry-pull"}]}}' \
  -- sleep 3600

kubectl cp -n cb-build \
  cb-build-debug:/work/src/out/cb-release/cloud_browser_worker \
  ./cloud_browser_worker

kubectl delete pod -n cb-build cb-build-debug
```

### Option B — push as a container layer (recommended for the runtime image)

The natural place for the binary is the runtime container image
(`infra/Dockerfile`). The runtime Dockerfile's `chromium` package
install will eventually be replaced by a `COPY --from=cb-build` of
`cloud_browser_worker`. Until that's wired (Phase 2.5), Option A is
the operational path.

### Option C — rsync to a pod with cluster shell

```bash
kubectl exec -n cb-build cb-build-debug -- \
  tar -czf - -C /work/src/out/cb-release cloud_browser_worker \
  | tar -xzf -
```

Option C is faster than `kubectl cp` for multi-GB artifacts (skips
the per-file overhead of the cp implementation).

## Iterate on the build

Once the first build is green, **don't** delete + re-apply the Job
unless you need to start from scratch. The PVCs survive Job
deletion, so successive builds reuse the synced Chromium tree +
sccache. The fast-path is:

```bash
# Re-run the same Job on the same PVCs.
kubectl delete job cb-build -n cb-build  # PVCs survive
kubectl apply  -k infra/k8s/cb-build/    # recreates Job
```

`cb-build.sh` (T113) is responsible for distinguishing first-run
(needs gclient sync) from subsequent runs (incremental autoninja).

## cb-build.sh contract (T113)

T113 lives at `build/cb-build.sh` in our repo. The Job mounts the
repo at `/workspace` and invokes `bash /workspace/build/cb-build.sh`
directly. The brief originally proposed a handoff at
`/work/src/build/cb-build.sh`; T113 placed the script in our repo
and the contract aligned around that — `CB_REPO=/workspace` is
T113's expected default.

### Inputs (env, set by build-job.yaml)

| Var | Value | Purpose |
|-----|-------|---------|
| `CB_REPO` | `/workspace` | Our repo root. Read-only emptyDir mount populated by the bootstrap initContainer. |
| `CB_WORK_ROOT` | `/work` | Work-tree root. Holds `src/chromium/` (Chromium checkout), `artifacts/` (output binaries), `logs/` (timestamped log files). On the chromium-src PVC. |
| `CHROMIUM_BRANCH_NUMBER` | `7727` | Plain integer, NOT `refs/branch-heads/...`. Script constructs the full ref. M147 stable; verify on chromiumdash before each roll. |
| `CB_BUILD_TARGETS` | `cloud_browser_worker cloud_browser_encoder_unittests cloud_browser_framesink_capturer_unittests` | autoninja targets. |
| `SCCACHE_DIR` | `/sccache` | Local-disk cache (sccache PVC mount). |
| `SCCACHE_CACHE_SIZE` | `80G` | sccache eviction trigger. |
| `SCCACHE_IDLE_TIMEOUT` | `0` | Keep sccache server alive for full build. |
| `NINJA_PARALLELISM` | `28` | Capped under the 30-CPU container cap; gives lto + sccache server headroom. |
| `SKIP_FETCH` | `""` | Set to `"1"` on Job retries that don't need a fresh gclient sync. The bootstrap initContainer doesn't set this — flip via `kubectl set env` if needed. |
| `STUB_MODE` | `""` | Set to `"1"` for plumbing-only validation (skips heavy steps). DoD smoke run uses this. |
| `OUR_REPO`, `OUR_REPO_REF` | (forgejo URL, `main`) | Diagnostic logging; the initContainer has already synced. |

### Outputs (all under `/work` on the chromium-src PVC)

- `/work/src/chromium/src/out/cb-release/cloud_browser_worker` —
  the worker binary.
- `/work/src/chromium/src/out/cb-release/cloud_browser_encoder_unittests`
  — encoder gtest binary.
- `/work/artifacts/cloud_browser_worker-${CB_GIT_SHA}.tar.zst` —
  packaged output (T113's step 8 does this).
- `/work/logs/cb-build-<UTC-timestamp>.log` — full step-by-step log;
  also tee'd to stdout so `kubectl logs` captures the same content.

## Failure-mode runbook

| Symptom | Diagnosis | Fix |
|---------|-----------|-----|
| Pod stuck `Pending` | Triform-6 oversubscribed; PVC PV not provisioned | `kubectl describe pod -n cb-build` — read the events. Resize / drain another workload off triform-6. |
| `bootstrap-our-repo` fails on git clone | OUR_REPO unreachable / wrong ref | Check forgejo.triform.dev reachability from triform-6; verify `OUR_REPO_REF` exists. |
| `bootstrap-our-repo` reports "build/cb-build.sh missing" | T113 hasn't landed the script | Check the `OUR_REPO_REF` branch HEAD. Block until T113. |
| Main container OOMKilled | Linker peak exceeded 120 GiB | Reduce parallelism: set `NINJA_PARALLEL` env (cb-build.sh honours per T17 §4). Or split: build `cloud_browser_worker` first, then the unit-test target. |
| `gclient sync` HTTPS errors | Egress to chromium.googlesource.com blocked | Confirm cluster egress NetworkPolicies don't drop the cb-build ns. (Default: no policy applied; this should be open.) |
| Build wall-clock > 8h | Pod hits `activeDeadlineSeconds` | First build with cold sccache is the slow one — bump deadline once for the cold run, or warm the cache by mounting an existing sccache PVC. |
| Same patch fails to apply repeatedly | Patch context drift after a Chromium roll | Rebase patches/ against `${CHROMIUM_BRANCH}` HEAD; tracking-issue the rebase. |

## Cross-references

- `docs/build/chromium-from-source.md` — T17, the build env spec
  this manifest realises.
- `docs/internal/phase2-unlock-plan.md` — T101, what becomes
  buildable once this Job runs green.
- `capture/build-integration/build.sh` — T49, the in-tree
  apply-patches+gen+ninja wrapper that cb-build.sh (T113) is
  expected to call into.
- `infra/Dockerfile` — T7 + T28 + T57; the runtime image that will
  eventually `COPY --from=...` the binary this Job produces.
- `infra/k8s/observability-jaeger.yaml` — T99; same standalone-
  manifest pattern (opt-in, not in the runtime kustomize bundle).

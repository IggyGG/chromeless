# cb-build — Chromium build environment on triform-6 (T112 + T119)

Realises T17 + T101's plan: a K8s Job pinned to triform-6 (32 CPU /
128 GiB) that runs `cb-build.sh` (T113) to produce
`cloud_browser_worker` and the encoder unit-test binary from a
Chromium source checkout + our patch series.

## Why hostPath instead of PVC (T119)

Originally this Job used PVCs (chromium-src 500 GiB + sccache 100 GiB
on `openebs-hostpath`). The first live attempt to bring it up on the
Triform cluster hit a **provisioner regression**: openebs-localpv-provisioner
was silent on every new claim across `openebs-hostpath`,
`triform-data`, and `ceph-rbd`. PVCs from 8+ days earlier remained
healthy; only new claims failed. Not disk-space (triform-6 has 5.4T
free on `/data`), not RBAC.

The pragmatic workaround: pin the build state to triform-6's
`/data/cb-build/{chromium-src,sccache}` via hostPath. Trade-offs:

- **PRO:** works today, no provisioner involvement.
- **PRO:** /data is /dev/md5 with 5.4T free — bigger than the PVC
  sizing.
- **CON:** hostPath isn't portable. The dirs are tied to triform-6;
  rebuild the node and you lose the build state.
- **CON:** needs an init container running as root (with `CHOWN` cap)
  to chown the hostPath for the unprivileged builder user. The
  `cb-build` ns is labelled `triform.ai/purpose=build` so this kind
  of thing is scoped.

When the provisioner regression is fixed cluster-wide, revert to the
pre-T119 PVC version — `git log -p infra/k8s/cb-build/build-job.yaml`
shows the diff, and resurrecting `pvc-chromium-src.yaml` +
`pvc-sccache.yaml` from the same commit takes a minute.

## Layout

| File | Purpose |
|------|---------|
| `namespace.yaml` | `cb-build` ns. Isolated from the runtime ns so build-only quotas / network policies / labels don't leak. |
| `build-job.yaml` | The Job. nodeName=triform-6, 30 CPU / 120 GiB, OnFailure / backoffLimit=2 / 8h activeDeadline. hostPath volumes per T119. |
| `cleanup-job.yaml` | **Manual** Job to wipe `/data/cb-build/` on triform-6. NOT in the kustomize bundle — operator runs explicitly when starting from scratch. |
| `Dockerfile.build-runner` | Build-runner image: Debian 12 + depot_tools + sccache + git + python3 + sudo + zstd. Pushed to forgejo. |
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

Once the first build is green, **don't** delete `/data/cb-build/` on
triform-6 unless you need to start from scratch. The hostPath dirs
survive Job deletion, so successive builds reuse the synced
Chromium tree + sccache. The fast-path is:

```bash
# Re-run the same Job on the same hostPath dirs.
kubectl delete job cb-build -n cb-build   # hostPath dirs survive
kubectl apply  -k infra/k8s/cb-build/     # recreates Job
```

For incremental retries that should skip the multi-hour gclient
sync entirely:

```bash
# Tell cb-build.sh to assume the tree is already populated.
kubectl -n cb-build delete job cb-build
kubectl -n cb-build apply -k infra/k8s/cb-build/
kubectl -n cb-build set env job/cb-build SKIP_FETCH=1
```

`cb-build.sh` (T113) is responsible for distinguishing first-run
(needs gclient sync) from subsequent runs (incremental autoninja).

### Start fresh (wipe hostPath state)

If a build wedged the tree in a state cb-build.sh can't recover from,
or you want to validate from a totally clean baseline:

```bash
# Apply the manual cleanup Job. NOT in the kustomize bundle.
kubectl apply -f infra/k8s/cb-build/cleanup-job.yaml
kubectl logs -n cb-build job/cb-build-cleanup -f
kubectl delete job -n cb-build cb-build-cleanup     # auto-cleans after ttl, but explicit is fine

# Then re-apply the build Job — hostPath DirectoryOrCreate recreates the dirs empty.
kubectl apply -k infra/k8s/cb-build/
```

To wipe selectively (e.g. preserve sccache for a fast next build),
edit `cleanup-job.yaml`'s `command` to remove only the directory you
want gone before applying.

Direct ssh to triform-6 also works — `sudo rm -rf /data/cb-build/*` —
but only if you're already comfortable opening that shell. The
cleanup Job is the kubectl-only path.

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
| `CB_WORK_ROOT` | `/work` | Work-tree root. Holds `src/chromium/` (Chromium checkout), `artifacts/` (output binaries), `logs/` (timestamped log files). hostPath: `/data/cb-build/chromium-src` on triform-6. |
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
| Pod stuck `Pending` | Triform-6 oversubscribed | `kubectl describe pod -n cb-build` — read the events. Resize / drain another workload off triform-6. |
| Pod stuck `ContainerCreating` with hostPath error | `/data` missing on triform-6, or kubelet refused the hostPath | ssh triform-6, confirm `/data` is mounted (`df -h /data`). hostPath `DirectoryOrCreate` will create `/data/cb-build/{chromium-src,sccache}` but the parent must exist. |
| `host-permissions` initContainer fails with "operation not permitted" on chown | Cluster PSP / pod-security blocks the CHOWN cap | The cb-build ns is labelled `triform.ai/purpose=build` to allow this. If a tighter cluster policy lands, scope the policy to exclude this ns or replace this initContainer with a privileged hostPath chown via a DaemonSet. |
| `bootstrap-our-repo` fails on git clone | OUR_REPO unreachable / wrong ref | Check forgejo.triform.dev reachability from triform-6; verify `OUR_REPO_REF` exists. |
| `bootstrap-our-repo` reports "build/cb-build.sh missing" | T113 hasn't landed the script | Check the `OUR_REPO_REF` branch HEAD. Block until T113. |
| Main container OOMKilled | Linker peak exceeded 120 GiB | Reduce parallelism: set `NINJA_PARALLELISM` env to 16 (default is 28). Or split: build `cloud_browser_worker` first, then the unit-test target. |
| `gclient sync` HTTPS errors | Egress to chromium.googlesource.com blocked | Confirm cluster egress NetworkPolicies don't drop the cb-build ns. (Default: no policy applied; this should be open.) |
| Build wall-clock > 8h | Pod hits `activeDeadlineSeconds` | First build with cold sccache is the slow one — bump deadline once for the cold run, or wipe sccache only via `cleanup-job.yaml` selectively to keep the chromium tree. |
| Same patch fails to apply repeatedly | Patch context drift after a Chromium roll | Rebase patches/ against `${CHROMIUM_BRANCH_NUMBER}` HEAD; tracking-issue the rebase. |
| Free space on `/data` shrinking faster than expected | Stale build state from a previous roll | `kubectl logs -n cb-build job/cb-build-cleanup` after running cleanup-job; or ssh triform-6 and `du -sh /data/cb-build/*`. |

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

# cb-tests — runtime validation Job manifests

Two K8s Jobs that validate the deployed `cb-chromium` binary against
direct CDP traffic, bypassing the platform layer entirely.

| Job | Image | Target | Wall-clock | Wired into |
|---|---|---|---|---|
| `cb-cdp-validation` | `python:3.11-slim` | `cb-browserless.triform-wtf.svc.cluster.local:9222` | <30s | `build/cb-build.sh` Step 10 (auto) |
| `cb-webrtc-validation` | `cb-chromium` + `node:20-bookworm-slim` (two-container Pod) | `localhost:9222` (in-pod) | ~30s | manual apply |

Test source code is **not** baked into the manifests — both Jobs use
a git-clone initContainer that pulls `forgejo.triform.dev/triform/
chromeless` at apply-time and runs whatever's on `main` at
`tests/cdp/` and `tests/webrtc/`. This keeps the YAML stable as the
test suites evolve and avoids ConfigMap-size limits.

## One-time setup

Both Jobs need two Secrets in `cb-tests` namespace:

```bash
# Create the namespace (covered by `apply -k`, but harmless to run early)
kubectl apply -f infra/k8s/tests/namespace.yaml

# Mirror image-pull + forgejo creds from cb-build → cb-tests
for s in registry-pull forgejo-credentials; do
  kubectl get secret "$s" -n cb-build -o yaml \
    | sed 's/namespace: cb-build/namespace: cb-tests/' \
    | kubectl apply -f -
done
```

## Manual invocation

```bash
# Apply everything (idempotent)
kubectl apply -k infra/k8s/tests/

# Or one Job at a time
kubectl apply -f infra/k8s/tests/cb-cdp-validation.yaml
kubectl apply -f infra/k8s/tests/cb-webrtc-validation.yaml

# Watch
kubectl -n cb-tests get jobs,pods -w

# Logs (multi-container pods need -c)
kubectl -n cb-tests logs -l app.kubernetes.io/variant=cdp-smoke --tail=-1
kubectl -n cb-tests logs -l app.kubernetes.io/variant=webrtc-encoder-identity \
  -c test-driver --tail=-1
kubectl -n cb-tests logs -l app.kubernetes.io/variant=webrtc-encoder-identity \
  -c cb-chromium --tail=-1

# Re-run (Jobs are immutable, so delete + apply)
kubectl -n cb-tests delete job cb-cdp-validation
kubectl apply -f infra/k8s/tests/cb-cdp-validation.yaml
```

## Wiring into `cb-build.sh` (Step 10)

After kaniko pushes `registry.triform.cloud/cloud-browser-webrtc/
cb-chromium:cr7727-sw`, Step 10 auto-applies `cb-cdp-validation.yaml`
with the just-pushed tag injected via env-var. Build promotion
gates on the Job exiting 0.

The CDP Job does **not** apply the WebRTC Job — that one stays
manual until encoder-assertions stabilises and the harness has a few
green runs against `cr7727-sw`.

`cdp-test-author` owns the Step 10 invocation; this README documents
the contract so an operator can re-run the smoke independently of
the build pipeline.

## Related

- `infra/k8s/cb-build/build-job.yaml` — image-build Job. Pattern
  reference for the git-clone bootstrap initContainer used here.
- `infra/k8s/cb-build/cb-kaniko-push.yaml` — pushes the runtime image
  this validation tests against.
- `tests/smoke/container-boot.sh` — host-driven CDP smoke for local
  dev. The k8s `cb-cdp-validation` Job is the in-cluster equivalent.
- `tests/cdp/`, `tests/webrtc/` — the test suites these Jobs run
  (owned by cdp-test-author and webrtc-harness-author respectively).

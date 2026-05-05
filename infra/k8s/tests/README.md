# chromeless-tests — runtime validation Job manifests

Two K8s Jobs that validate the deployed `chromeless` binary against
direct CDP traffic, bypassing the platform layer entirely.

| Job | Image | Target | Wall-clock | Wired into |
|---|---|---|---|---|
| `chromeless-cdp-validation` | `python:3.11-slim` | `chromeless-browserless.triform-wtf.svc.cluster.local:9222` | <30s | `build/chromeless-build.sh` Step 10 (auto) |
| `chromeless-webrtc-validation` | `chromeless` + `node:20-bookworm-slim` (two-container Pod) | `localhost:9222` (in-pod) | ~30s | manual apply |

Test source code is **not** baked into the manifests — both Jobs use
a git-clone initContainer that pulls `forgejo.triform.dev/triform/
chromeless` at apply-time and runs whatever's on `main` at
`tests/cdp/` and `tests/webrtc/`. This keeps the YAML stable as the
test suites evolve and avoids ConfigMap-size limits.

## One-time setup

Both Jobs need two Secrets in `chromeless-tests` namespace:

```bash
# Create the namespace (covered by `apply -k`, but harmless to run early)
kubectl apply -f infra/k8s/tests/namespace.yaml

# Mirror image-pull + forgejo creds from chromeless-build → chromeless-tests
for s in registry-pull forgejo-credentials; do
  kubectl get secret "$s" -n chromeless-build -o yaml \
    | sed 's/namespace: chromeless-build/namespace: chromeless-tests/' \
    | kubectl apply -f -
done
```

## Manual invocation

```bash
# Apply everything (idempotent)
kubectl apply -k infra/k8s/tests/

# Or one Job at a time
kubectl apply -f infra/k8s/tests/chromeless-cdp-validation.yaml
kubectl apply -f infra/k8s/tests/chromeless-webrtc-validation.yaml

# Watch
kubectl -n chromeless-tests get jobs,pods -w

# Logs (multi-container pods need -c)
kubectl -n chromeless-tests logs -l app.kubernetes.io/variant=cdp-smoke --tail=-1
kubectl -n chromeless-tests logs -l app.kubernetes.io/variant=webrtc-encoder-identity \
  -c test-driver --tail=-1
kubectl -n chromeless-tests logs -l app.kubernetes.io/variant=webrtc-encoder-identity \
  -c chromeless --tail=-1

# Re-run (Jobs are immutable, so delete + apply)
kubectl -n chromeless-tests delete job chromeless-cdp-validation
kubectl apply -f infra/k8s/tests/chromeless-cdp-validation.yaml
```

## Wiring into `chromeless-build.sh` (Step 10)

After kaniko pushes `registry.triform.cloud/chromeless/
chromeless:cr7727-sw`, Step 10 auto-applies `chromeless-cdp-validation.yaml`
with the just-pushed tag injected via env-var. Build promotion
gates on the Job exiting 0.

The CDP Job does **not** apply the WebRTC Job — that one stays
manual until encoder-assertions stabilises and the harness has a few
green runs against `cr7727-sw`.

`cdp-test-author` owns the Step 10 invocation; this README documents
the contract so an operator can re-run the smoke independently of
the build pipeline.

## Related

- `infra/k8s/chromeless-build/build-job.yaml` — image-build Job. Pattern
  reference for the git-clone bootstrap initContainer used here.
- `infra/k8s/chromeless-build/chromeless-kaniko-push.yaml` — pushes the runtime image
  this validation tests against.
- `tests/smoke/container-boot.sh` — host-driven CDP smoke for local
  dev. The k8s `chromeless-cdp-validation` Job is the in-cluster equivalent.
- `tests/cdp/`, `tests/webrtc/` — the test suites these Jobs run
  (owned by cdp-test-author and webrtc-harness-author respectively).

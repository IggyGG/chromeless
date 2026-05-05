# Triform K8s deploy runbook (Phase 2)

Concrete operator runbook for the chromeless Helm release on
the Triform K8s cluster. Pairs with `docs/operations/runbook.md`
(general / cluster-agnostic) and `docs/operations/phase1-deployment-checklist.md`
(pre-Phase-2). T115 owns this document.

The chart itself lives at `infra/helm/chromeless/`. The
Triform values overlay lives at `infra/k8s/chromeless/values-triform.yaml`.
The install wrapper is `infra/k8s/chromeless/install.sh`.

## Cluster facts (verified live, pin to chart-installation date)

| Node | Role | CPU/RAM/Disk | Notes |
|------|------|--------------|-------|
| triform-1 | control-plane | 64/230Gi/916Gi | infra label |
| triform-2 | worker | 64/263Gi/342Gi | signaling target |
| triform-3 | worker | 64/263Gi/342Gi | signaling target, role=infra |
| triform-4 | worker | 20/65Gi/1.8Ti | GPU-tainted but no GPU label (small box) |
| triform-5 | worker | 48/263Gi/920Gi | **8× RTX PRO 6000 Blackwell** (driver 590.48.01, compute 12.0). Tainted `nvidia.com/gpu=present:NoSchedule` |
| triform-6 | worker | 32/131Gi/102Gi | Build node (T112) |

**Cluster services already running:**

- ingress-nginx (`nginx` IngressClass)
- cert-manager + ClusterIssuer `letsencrypt-production` (preferred) and `letsencrypt-prod`
- nvidia-device-plugin DaemonSet (selector `nvidia.com/gpu.present=true`)
- kube-prometheus-stack (Grafana + Prometheus in `monitoring` ns)
- forgejo at `forgejo.triform.dev`; container registry at `registry.triform.cloud`
- DNS: `*.triform.dev` for prod-shaped hosts, `*.triform.cloud` for cloud-fronted, `*.triform.wtf` for staging

**NOT running:**
- NVIDIA GPU operator / RuntimeClass (the chart's `defaultPool.runtimeClassName` stays empty)
- sealed-secrets / external-secrets-operator (secrets are plain `kubectl create secret`)
- gVisor RuntimeClass (T44 / T95 design lands later)

## Pre-deploy checklist

1. **Build artifacts present.** T112's K8s Job has produced
   `cloud_browser_worker` + companion service images, kaniko-pushed
   to `registry.triform.cloud/chromeless/cb-*:<tag>`. Note
   the tag — you'll pass it as `CHROMELESS_IMAGE_TAG`.
2. **DNS record exists.** `chromeless-signaling.triform.dev` → cluster
   ingress IP. Add to Cloudflare (the registrar for `triform.dev`) as
   an A record pointing at the ingress-nginx LoadBalancer IP. Same
   pattern as existing apps (forgejo.triform.dev, coder.triform.dev,
   etc.). Cloudflare proxy: **off** for WebSocket apps; the orange
   cloud strips the Upgrade header by default.
3. **TURN configuration decided.** Phase 2 default is Cloudflare TURN
   (managed). Generate the credentials per `docs/security/auth.md`
   §"TURN issuer". Self-hosted coturn is disabled in
   `values-triform.yaml`.
4. **Auth pubkey decided.** Generate the Ed25519 keypair per
   `docs/security/auth.md`. The pubkey goes into the secrets overlay
   (below); the privkey goes to whoever issues session tokens (the
   dev-issuer in Phase 1; in Phase 2 a real auth service).
5. **Secrets overlay file authored**, ungitted, at
   `~/.chromeless-secrets-triform.yaml`:

```yaml
# ~/.chromeless-secrets-triform.yaml — DO NOT commit. Provided to install.sh
# via CHROMELESS_SECRETS_FILE.
signaling:
  authPubkey: "<base64 32-byte raw Ed25519 public key>"
turnIssuer:
  secrets:
    authPubkey: "<same value as signaling.authPubkey>"
    sharedSecret: "<openssl rand -hex 32>"
    sharedSecretPrev: ""
    turnURLs: "turn:turn.cloudflare.com:3478?transport=udp,turns:turn.cloudflare.com:5349?transport=tcp"
    stunURLs: "stun:stun.cloudflare.com:3478,stun:stun.l.google.com:19302"
```

## Install

```bash
export CHROMELESS_IMAGE_TAG="m147-roll1-abc1234"   # tag from T113's build
export CHROMELESS_SECRETS_FILE="$HOME/.chromeless-secrets-triform.yaml"

# Dry-run first to see what's about to land.
DRY_RUN=1 ./infra/k8s/chromeless/install.sh

# If the dry-run looks right, apply for real.
./infra/k8s/chromeless/install.sh
```

`install.sh`:

1. Validates the kubectl context is the Triform cluster (checks for
   triform-5 + triform-6 nodes).
2. Creates the `chromeless` namespace if absent.
3. Copies the `registry-pull` secret from the `default` ns into
   `chromeless` if absent.
4. Renders the chart with values + secrets overlay + per-image tag
   `--set` for every cb-* image.
5. Runs `helm upgrade --install --wait --atomic --timeout 10m`.
6. Smokes `signaling /healthz` from inside the cluster.
7. Lists the warm session Pods.

## Verify

```bash
# Pods, services, ingress.
kubectl -n chromeless get all

# Warm pool reached its target?
kubectl -n chromeless get browsersessionpool default-pool -o jsonpath='{.status}' | jq

# Signaling reachable from outside?
curl -fsS https://chromeless-signaling.triform.dev/healthz

# Metrics scraped by kube-prometheus-stack?
kubectl -n monitoring port-forward svc/kube-prometheus-stack-prometheus 9090:9090
# then http://localhost:9090/targets — look for cb_signaling_* and cb_chromium_* targets

# Grafana dashboards (T66) — auto-discovered via the configmap dashboard provider:
kubectl -n monitoring port-forward svc/kube-prometheus-stack-grafana 3000:80
# default password is in secret kube-prometheus-stack-grafana
```

## GPU resource quota planning

Triform-5 has 8 allocatable GPUs. The values overlay caps:

- `defaultPool.warmReplicas: 2` — 2 GPUs reserved for warm Pods
- `defaultPool.maxSessions: 6` — 8 - 2 warm = 6 assignable

If you need to run more than 6 concurrent sessions, the right play is
to add a second GPU node, NOT to bump maxSessions on a single node.
WebRTC encoder under-provisioning shows up as user-visible quality
collapse (per T63 NVENC tuning rationale: each session pins ~1 SM
and ~1.5 GiB VRAM during encode). The 6-session cap is conservative
but correct.

To temporarily allow more (e.g. for a load test):

```bash
helm upgrade cb infra/helm/chromeless/ \
  -f infra/k8s/chromeless/values-triform.yaml \
  -f ~/.chromeless-secrets-triform.yaml \
  --set defaultPool.maxSessions=8 \
  --set defaultPool.warmReplicas=0 \
  -n chromeless
```

## Rollback

The install runs with `--atomic`, so a failed `helm upgrade` rolls
back automatically. For an explicit roll-back to a previous good
revision:

```bash
helm history cb -n chromeless
helm rollback cb <revision> -n chromeless --wait --timeout 5m
```

Image-only roll-back (without changing values) is a single
`--set images.<x>.tag=<previous>` re-run of `install.sh`.

If the controller has cycled the BrowserSessionPool template since
the rollback target, **expect the warm Pods to be torn down and
recreated** to match the rolled-back template. New sessions land on
the rolled-back image; existing sessions run to completion on
whatever image they started with.

## First-deploy checklist

Print and tick:

- [ ] Build artifact tag captured (`CHROMELESS_IMAGE_TAG`)
- [ ] DNS record `chromeless-signaling.triform.dev` resolves (`dig +short`)
- [ ] Ingress IP reachable on TCP 443 from outside the cluster
- [ ] Auth keypair generated; pubkey in secrets overlay; privkey safe
- [ ] TURN shared secret generated; in secrets overlay
- [ ] TURN URLs set in secrets overlay
- [ ] DRY_RUN=1 install.sh output reviewed
- [ ] install.sh exit 0
- [ ] `kubectl -n chromeless get all` shows expected pods Ready
- [ ] `curl https://chromeless-signaling.triform.dev/healthz` returns 200
- [ ] Grafana dashboard `chromeless-cluster-overview` shows the new region
      (cb_signaling_active_sessions, cb_chromium_cpu_pct...)
- [ ] At least one BrowserSession can be created via the controller
      (manual `kubectl apply -f` of a sample CR or via a real
      client connect — that's T116's territory)

## Failure-mode runbook

| Symptom | Diagnosis | Fix |
|---------|-----------|-----|
| `helm install` exits with `values.signaling.authPubkey is required` | Secrets overlay missing or wrong path | `ls -la ${CHROMELESS_SECRETS_FILE}`; reset CHROMELESS_SECRETS_FILE to the right path; re-run |
| Signaling Pod stuck `ContainerCreating` | registry-pull secret missing in chromeless | `kubectl -n chromeless get secret registry-pull` — if absent, install.sh's auto-copy didn't run; manually `kubectl get secret -n default registry-pull -o yaml \| sed 's/namespace: default/namespace: chromeless/' \| kubectl apply -f -` |
| Signaling Pod CrashLoopBackOff | Bad authPubkey (not 32-byte raw key, base64 mis-padded) | `kubectl -n chromeless logs deploy/signaling`; regenerate keypair, update secrets overlay, re-install |
| Session Pod `Pending` with "Insufficient nvidia.com/gpu" | More sessions requested than GPUs free | `kubectl -n chromeless describe pod` confirms; either wait, or `kubectl delete browsersession <stale>` |
| Session Pod `Pending` with "didn't tolerate the taint" | tolerations / nodeSelector mismatch in the pool template | check `kubectl -n chromeless get browsersessionpool default-pool -o yaml` matches the values overlay; the controller may need a restart if a pool template update didn't propagate |
| Ingress 404 / cert-manager not issuing | DNS not resolving yet, or wrong ClusterIssuer | `kubectl describe ingress -n chromeless signaling`; check Events; verify `cert-manager.io/cluster-issuer: letsencrypt-production` annotation matches a ready ClusterIssuer |
| WebSocket connects then disconnects after seconds | nginx ingress proxy timeouts | confirm `proxy-read-timeout` and `proxy-send-timeout` annotations are in the rendered Ingress; bump if user sessions exceed 1h |
| Per-session GPU contention symptoms (encoder underrun, FPS collapse) | maxSessions too high for the node | drop `defaultPool.maxSessions`; correlate with `nvidia-smi` on triform-5 |

## Cross-references

- `infra/helm/chromeless/` — the chart
- `infra/k8s/chromeless/values-triform.yaml` — Triform overlay
- `infra/k8s/chromeless/install.sh` — install wrapper
- `docs/security/auth.md` — auth keypair + TURN credentials lifecycle
- `docs/operations/runbook.md` — general operator runbook
- `docs/operations/multi-region.md` — when to add a second region
- `docs/operations/tracing.md` — Jaeger setup (T99); applies same shape
- `infra/k8s/chromeless-build/` — T112 build environment
- `docs/operations/encoder-factory-deploy.md` — T106; encoder-side
  deploy concerns once the real factory is shipping

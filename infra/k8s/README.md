# `infra/k8s/` — Kubernetes manifests for chromeless

> **Status:** Phase 3 prep (T50). The manifests apply cleanly with
> `kubectl --dry-run=client` against any K8s 1.27+ API. A real cluster
> bring-up depends on a few cluster-specific values being filled in
> first; see [§ Per-cluster checklist](#per-cluster-checklist).

## Layout

| File | Purpose |
|---|---|
| `namespace.yaml` | The `chromeless` namespace with Pod Security Standards labels. |
| `signaling-deployment.yaml` | Stateless signaling tier: Deployment + Service + Ingress + HPA + ServiceAccount. |
| `turn-deployment.yaml` | coturn StatefulSet with TURN-REST credentials Secret + ConfigMap + headless Service. **Production should prefer a managed TURN service** (Cloudflare TURN, metered.ca, Twilio) — see comments in that file. |
| `cloud-browser-session.yaml` | Pod template for one session — three containers (chromeless, chromeless-metrics-sidecar, clipboard-bridge) sharing a network and PID namespace. M7 R4: input-bridge + cursor-watcher sidecars retired; the native peer (M4+M5) delivers their function from inside the browser process. |
| `session-controller-design.md` | Design doc for the warm-pool / assignment / eviction operator. **Read this** before touching the manifests in anger. |
| `kustomization.yaml` | `kubectl apply -k .` entry point. |

## Deploy

```
kubectl apply -k infra/k8s/
```

This applies the namespace, signaling tier, TURN tier, and one
example session Pod. Production deploys would split the example Pod
out and let the session controller manage Pod lifecycles instead.

## Per-cluster checklist

Before applying to a real cluster, fill these in (probably via a
kustomize overlay under `infra/k8s/overlays/<env>/`):

- **Container images.** All manifests reference `ghcr.io/iggy/...`
  placeholders. Replace with your registry.
- **Ingress.**
  - `signaling-deployment.yaml` → `spec.ingressClassName` (default
    `nginx`); change to `alb`, `gce`, `traefik`, etc. as needed.
  - `spec.rules[].host` → your real hostname.
  - `spec.tls[].secretName` → your TLS secret. The default
    `cert-manager.io/cluster-issuer: letsencrypt-prod` annotation
    assumes cert-manager is installed; remove and pre-create the
    secret if not.
- **TURN secret.**
  `turn-rest-secret.stringData.static-auth-secret` is a placeholder.
  Replace with `openssl rand -hex 32` output and seal in your
  secret-management tool (sealed-secrets, External Secrets, Vault,
  SOPS).
- **TURN node placement.** coturn uses `hostNetwork: true`. Pin to a
  dedicated node pool via `nodeSelector` + tolerations in your
  overlay. Make sure the pool's security groups / firewall rules
  allow UDP 3478, 5349, and 49152-65535.
- **Storage class** is unused today (everything is `emptyDir`). When
  Phase 3 adds session snapshot/restore, this section grows.
- **NetworkPolicy.** Not in this base. Add one in your overlay that
  matches your cluster's CNI and your tenancy model. The shape is
  outlined in `session-controller-design.md` § "Cluster-level
  network isolation".

## Cloud-specific gotchas

### GKE

- gVisor is first-party. Once T44's decision is made, install via
  `gcloud container node-pools create --sandbox type=gvisor` and
  uncomment `runtimeClassName: gvisor` in
  `cloud-browser-session.yaml`.
- UDP LoadBalancer for TURN works via the GCP cloud provider; you
  may prefer that over `hostNetwork=true`.
- `pod-security.kubernetes.io/enforce: baseline` is OK on GKE; bump
  to `restricted` once `runAsNonRoot: true` is honoured by every
  container (chromeless needs it).

### EKS

- AWS NLB supports UDP since 2020 — viable alternative to
  `hostNetwork=true` for TURN. Use the AWS Load Balancer Controller
  with `service.beta.kubernetes.io/aws-load-balancer-type: nlb` and
  `service.beta.kubernetes.io/aws-load-balancer-nlb-target-type: ip`.
- ALB Ingress is the default; the
  `ingressClassName: nginx` line in `signaling-deployment.yaml`
  needs swapping. If you keep nginx-ingress, install it explicitly.
- gVisor is **not** supported on EKS. The Phase 3 sandbox decision
  there is between Kata + Cloud Hypervisor (third-party) and
  staying on `runc` + AppArmor.

### AKS

- AKS-managed nginx-ingress and cert-manager are available as
  add-ons; if you use them, drop the `cert-manager.io` annotation
  and let the AKS add-on do TLS.
- Azure LB supports UDP for TURN.
- Confidential Containers (Kata) has Azure-specific tooling (CoCo).
  This is the most production-ready path to per-session VM
  isolation on AKS today.

## Validation

The CI pipeline runs `kubectl --dry-run=client apply -k infra/k8s/`
against a recent K8s API to catch schema errors. Locally:

```
kubectl --dry-run=client apply -k infra/k8s/ | head -30
kustomize build infra/k8s/ | kubectl --dry-run=server apply -f -   # against a real cluster
```

`--dry-run=server` is stricter (admission controllers see the
manifest) but requires a real cluster connection.

## Out of scope for T50

- The session controller binary (designed in
  `session-controller-design.md`; built in a follow-up).
- NetworkPolicy and PodDisruptionBudget (production hygiene; layer
  in your overlay).
- ServiceMonitor / PodMonitor (Prometheus Operator-specific; we use
  the `prometheus.io/scrape` annotation pattern that works without
  the Operator).
- HorizontalPodAutoscaler for session pods (sessions are not
  fungible replicas; the controller manages capacity, not an HPA).
- Snapshot / restore via CRIU (Phase 4 stretch).

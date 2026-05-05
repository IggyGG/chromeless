# chromeless Helm chart

Phase 1 deployment of chromeless onto Kubernetes. Wraps
the kustomize bundle in `infra/k8s/` plus the CRDs from the session
controller into a single `helm install`.

## Quickstart

```sh
helm install chromeless \
    oci://ghcr.io/iggy/chromeless/helm/chromeless \
    --version 0.1.0 \
    --namespace chromeless \
    --create-namespace \
    -f my-values.yaml
```

`my-values.yaml` at minimum sets:

```yaml
signaling:
  authPubkey: <base64 ed25519 pubkey>     # optional; auth-disabled if omitted
turnIssuer:
  secrets:
    authPubkey: <same as above>
    sharedSecret: <openssl rand -hex 32>
    turnURLs: turn:turn.example.com:3478?transport=udp,turns:turn.example.com:5349?transport=tcp
coturn:
  enabled: false                          # use a managed TURN service in production
```

See [`values.yaml`](./values.yaml) for every knob.

## What's installed

| Resource | When |
|---|---|
| Namespace + Pod Security Standards labels | always (unless `namespace.create: false`) |
| BrowserSession + BrowserSessionPool CRDs | always (chart `crds/` directory; helm installs them on first install only) |
| Signaling Deployment + Service + Ingress + HPA + ServiceAccount | `signaling.enabled: true` (default) |
| BrowserSession controller Deployment + RBAC + Service | `controller.enabled: true` (default) |
| TURN issuer Deployment + Secret + Service | `turnIssuer.enabled: true` (default) |
| Default BrowserSessionPool with chromeless + chromeless-metrics-sidecar containers | `defaultPool.enabled: true` (default) |
| coturn StatefulSet + Secret + ConfigMap | **`coturn.enabled: true` (default false)** |

## Production guidance

- **TLS**: bring cert-manager and a ClusterIssuer to your cluster
  before installing. The chart's Ingress annotations assume cert-manager
  by default.
- **TURN**: prefer a managed TURN service (Cloudflare TURN /
  metered.ca / Twilio) over the in-chart coturn. The chart's coturn
  needs `hostNetwork: true` on a dedicated node pool with the right
  firewall — operationally painful. See
  [`infra/k8s/turn-deployment.yaml`](../../k8s/turn-deployment.yaml)
  for the full rationale.
- **Sandbox**: `gvisor.enabled: true` flips on
  `runtimeClassName: gvisor` for session pods. Ensure the
  RuntimeClass is installed cluster-wide first
  (see [`docs/research/sandbox-isolation.md`](../../../docs/research/sandbox-isolation.md)).
- **Seccomp**: the chart references a `Localhost: chromeless.json`
  seccomp profile. Each kubelet must have
  `/var/lib/kubelet/seccomp/chromeless.json` installed before the
  session pod will start. See `infra/security-hardening.md` and the
  `security-profiles-operator` recipe in
  [`docs/operations/runbook.md`](../../../docs/operations/runbook.md).

## Validate locally

```sh
cd infra/helm/chromeless
helm lint .
helm template . \
    --set turnIssuer.secrets.authPubkey="dGVzdA==" \
    --set turnIssuer.secrets.sharedSecret="x" \
    --set turnIssuer.secrets.turnURLs="turn:example:3478"
```

`helm template` resolves placeholders and renders the full manifest
set. The required-value checks for `turnIssuer.*` fail with a clear
error when omitted; they're the same `chromeless.requireValue` helper used
across the chart.

## Upgrade

```sh
helm upgrade chromeless ... -f my-values.yaml
```

The chart's CRDs are *not* upgraded by `helm upgrade` (helm's
deliberate design: CRD upgrades are a hand operation because
breaking-changes can wreck data). To upgrade CRDs:

```sh
kubectl apply -f infra/controllers/browser-session-controller/config/crd/
```

before bumping the chart's `appVersion`.

## Uninstall

```sh
helm uninstall chromeless --namespace chromeless
kubectl delete crd browsersessions.cloud-browser-webrtc.example.com \
                  browsersessionpools.cloud-browser-webrtc.example.com
```

Order matters: deleting the CRDs while the controller is still
running causes a brief reconcile-error storm in the logs. Uninstall
the chart first.

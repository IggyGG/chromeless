#!/usr/bin/env bash
#
# infra/k8s/chromeless/install.sh — install / upgrade the chromeless
# Helm release on the Triform K8s cluster (T115).
#
# Usage:
#   CHROMELESS_IMAGE_TAG=<tag> [CHROMELESS_SECRETS_FILE=<path>] [DRY_RUN=1] install.sh [helm-args...]
#
# Required:
#   CHROMELESS_IMAGE_TAG     — image tag for every cb-* image. Set to the
#                       T113 build's output tag (e.g. m147-roll1-abc1234).
#                       This is the SAME tag for chromium / signaling /
#                       controller / turn-issuer / metrics-sidecar /
#                       input-bridge / cursor-watcher / clipboard-bridge.
#                       Per-image overrides are still possible via
#                       extra `--set images.<x>.tag=...` arguments.
#
# Optional:
#   CHROMELESS_SECRETS_FILE  — path to a values overlay carrying the auth
#                       pubkey + TURN shared secret + TURN URLs.
#                       NOT committed to git. See README.md /
#                       triform-deploy.md for the schema. Defaults
#                       to ~/.chromeless-secrets-triform.yaml if present.
#   DRY_RUN          — set to "1" to render + dry-run-server only
#                       (no actual install).
#   CHROMELESS_RELEASE_NAME  — Helm release name. Default: cb.
#   CHROMELESS_NAMESPACE     — target namespace. Default: chromeless.
#                       MUST match values-triform.yaml's namespace.name.
#   HELM_TIMEOUT     — timeout for `helm upgrade --wait`. Default: 10m.
#
# Behaviour:
#   1. Validates kubectl context is the Triform cluster.
#   2. Confirms registry-pull secret exists in the target namespace
#      (creates from the default ns copy if absent).
#   3. Validates CHROMELESS_IMAGE_TAG, CHROMELESS_SECRETS_FILE.
#   4. Runs `helm upgrade --install` with the values overlay + secrets
#      overlay (if provided) + per-image tag --set arguments.
#   5. Waits for all Deployments to be Available.
#   6. Smokes the signaling /healthz endpoint via in-cluster curl.
#
# Exit codes: any failure exits non-zero with the failing step logged.

set -euo pipefail

# ---------------------------------------------------------------------
# Resolve paths.
# ---------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
CHART_DIR="${REPO_ROOT}/infra/helm/chromeless"
VALUES_FILE="${SCRIPT_DIR}/values-triform.yaml"

CHROMELESS_RELEASE_NAME="${CHROMELESS_RELEASE_NAME:-cb}"
CHROMELESS_NAMESPACE="${CHROMELESS_NAMESPACE:-chromeless}"
CHROMELESS_SECRETS_FILE="${CHROMELESS_SECRETS_FILE:-${HOME}/.chromeless-secrets-triform.yaml}"
HELM_TIMEOUT="${HELM_TIMEOUT:-10m}"
DRY_RUN="${DRY_RUN:-}"

log()  { printf '[install.sh] %s\n' "$*"; }
die()  { log "FATAL: $*"; exit 1; }

# ---------------------------------------------------------------------
# Pre-flight.
# ---------------------------------------------------------------------

[[ -d "${CHART_DIR}" ]] || die "chart dir not found at ${CHART_DIR}"
[[ -f "${VALUES_FILE}" ]] || die "values file not found at ${VALUES_FILE}"

[[ -n "${CHROMELESS_IMAGE_TAG:-}" ]] || die "CHROMELESS_IMAGE_TAG is required (e.g. m147-roll1-abc1234)"

command -v kubectl >/dev/null || die "kubectl not on PATH"
command -v helm    >/dev/null || die "helm not on PATH"

# Sanity: confirm we're talking to the Triform cluster. Cheap check is
# the presence of triform-5 (GPU) + triform-6 (build) nodes; both
# exist on Triform and on no other cluster.
if ! kubectl get node triform-5 triform-6 >/dev/null 2>&1; then
    die "kubectl context doesn't see triform-5 + triform-6 nodes;
   refusing to install. Confirm with: kubectl config current-context"
fi
log "kubectl context: $(kubectl config current-context)"

# Ensure namespace exists, with the pod-security labels the chart's
# templates/namespace.yaml would otherwise apply. We pre-create it so
# the registry-pull secret can land before helm --wait gates on Pod
# readiness; namespace.create=false in values-triform.yaml stops the
# chart from racing us.
log "applying namespace ${CHROMELESS_NAMESPACE}"
kubectl apply -f - <<EOF
apiVersion: v1
kind: Namespace
metadata:
  name: ${CHROMELESS_NAMESPACE}
  labels:
    app.kubernetes.io/name: chromeless
    app.kubernetes.io/part-of: chromeless
    pod-security.kubernetes.io/enforce: baseline
    pod-security.kubernetes.io/audit: restricted
    pod-security.kubernetes.io/warn: restricted
EOF

# Registry pull secret — copy from `default` ns if missing. Idempotent.
if ! kubectl get secret -n "${CHROMELESS_NAMESPACE}" registry-pull >/dev/null 2>&1; then
    log "copying registry-pull from default ns into ${CHROMELESS_NAMESPACE}"
    kubectl get secret -n default registry-pull -o yaml \
        | sed "s/namespace: default/namespace: ${CHROMELESS_NAMESPACE}/" \
        | kubectl apply -f -
fi

# CRDs: Helm 3 installs the chart's crds/ dir on first install, but
# `helm template` + `--dry-run=server` can't validate the
# BrowserSessionPool resource without them. Apply CRDs up-front;
# kubectl apply is idempotent and CRDs survive across helm upgrades
# (Helm's crds/ is install-only by design).
if [[ -d "${CHART_DIR}/crds" ]]; then
    log "applying CRDs from ${CHART_DIR}/crds/ (idempotent)"
    kubectl apply -f "${CHART_DIR}/crds/"
fi

# Secrets overlay: required keys are signaling.authPubkey,
# turnIssuer.secrets.{authPubkey,sharedSecret,turnURLs}. The chart's
# fail() guards will reject the install if anything's missing, so
# we don't pre-validate here — the chart's error messages are clearer
# than anything we'd reproduce.
HELM_VALUES_ARGS=("-f" "${VALUES_FILE}")
if [[ -f "${CHROMELESS_SECRETS_FILE}" ]]; then
    log "secrets overlay: ${CHROMELESS_SECRETS_FILE}"
    HELM_VALUES_ARGS+=("-f" "${CHROMELESS_SECRETS_FILE}")
else
    log "WARN: no CHROMELESS_SECRETS_FILE at ${CHROMELESS_SECRETS_FILE}; relying on --set passes from caller"
fi

# Per-image tag --set arguments. CHROMELESS_IMAGE_TAG goes to every image
# unless the caller has overridden a specific image via extra args.
HELM_TAG_ARGS=()
for img in chromium signaling controller turnIssuer metricsSidecar inputBridge cursorWatcher clipboardBridge; do
    HELM_TAG_ARGS+=("--set" "images.${img}.tag=${CHROMELESS_IMAGE_TAG}")
done

log "release        : ${CHROMELESS_RELEASE_NAME}"
log "namespace      : ${CHROMELESS_NAMESPACE}"
log "image tag      : ${CHROMELESS_IMAGE_TAG}"
log "values         : ${VALUES_FILE}"
log "secrets overlay: ${CHROMELESS_SECRETS_FILE}${CHROMELESS_SECRETS_FILE:+ (exists=$([[ -f ${CHROMELESS_SECRETS_FILE} ]] && echo yes || echo no))}"
log "extra helm args: $*"

# ---------------------------------------------------------------------
# Render preview (always shown). Lets the operator eyeball what's
# about to land before --wait gates them in.
# ---------------------------------------------------------------------

log "rendering chart preview..."
helm template "${CHROMELESS_RELEASE_NAME}" "${CHART_DIR}" \
    "${HELM_VALUES_ARGS[@]}" \
    "${HELM_TAG_ARGS[@]}" \
    "$@" \
    > /tmp/chromeless-rendered.yaml 2> /tmp/chromeless-render.err
preview_status=$?
if [[ ${preview_status} -ne 0 ]]; then
    cat /tmp/chromeless-render.err >&2
    die "helm template failed (rc=${preview_status}); fix values and retry"
fi
preview_lines=$(wc -l < /tmp/chromeless-rendered.yaml | tr -d ' ')
log "rendered ${preview_lines} lines to /tmp/chromeless-rendered.yaml; head:"
head -30 /tmp/chromeless-rendered.yaml
log "... (full output at /tmp/chromeless-rendered.yaml)"

# ---------------------------------------------------------------------
# Install or dry-run.
# ---------------------------------------------------------------------

HELM_CMD=(
    helm upgrade --install "${CHROMELESS_RELEASE_NAME}" "${CHART_DIR}"
    --namespace "${CHROMELESS_NAMESPACE}"
    --create-namespace
    "${HELM_VALUES_ARGS[@]}"
    "${HELM_TAG_ARGS[@]}"
    --timeout "${HELM_TIMEOUT}"
    --wait
    --atomic
)

if [[ -n "${DRY_RUN}" ]]; then
    log "DRY_RUN=1; running --dry-run=server only"
    HELM_CMD+=(--dry-run=server)
fi

log "executing: ${HELM_CMD[*]} $*"
"${HELM_CMD[@]}" "$@"

# ---------------------------------------------------------------------
# Post-install smoke.
# ---------------------------------------------------------------------

if [[ -n "${DRY_RUN}" ]]; then
    log "DRY_RUN: skipping post-install smoke."
    exit 0
fi

log "release deployed; smoking endpoints..."

# 1. All Deployments report Available.
log "waiting for Deployments to become Available..."
kubectl -n "${CHROMELESS_NAMESPACE}" wait \
    --for=condition=Available \
    --timeout="${HELM_TIMEOUT}" \
    --all deployment

# 2. Signaling /healthz from inside the cluster (avoids ingress + DNS
#    being a smoke prerequisite).
log "smoking signaling /healthz from inside the cluster..."
kubectl -n "${CHROMELESS_NAMESPACE}" run chromeless-smoke-signaling-$$ \
    --image=curlimages/curl:8.10.1 \
    --restart=Never \
    --rm -i --quiet \
    --command -- \
    curl -fsS --max-time 10 \
    "http://signaling.${CHROMELESS_NAMESPACE}.svc.cluster.local:8080/healthz"
echo

# 3. Pool warm Pods come up. Look for at least the warmReplicas count
#    of session Pods reaching Ready.
log "checking BrowserSessionPool default-pool warm Pods..."
kubectl -n "${CHROMELESS_NAMESPACE}" get pods -l app.kubernetes.io/component=session 2>&1 || true

log "done. Verify in Grafana (kube-prometheus-stack: monitoring/Grafana)."
log "trace UI (if Jaeger is installed per T99): port-forward svc/jaeger 16686."

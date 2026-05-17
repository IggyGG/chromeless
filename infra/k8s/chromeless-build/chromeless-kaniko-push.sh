#!/usr/bin/env bash
# chromeless-kaniko-push.sh — canonical apply wrapper for chromeless-kaniko-push.yaml
#
# Why this script exists: the YAML is a template with three envsubst
# placeholders (CHROMELESS_KANIKO_TAG, KANIKO_NODE, KANIKO_VARIANT_LABEL).
# Direct `kubectl apply -f` would push the literal placeholders to the
# cluster. We derive CHROMELESS_KANIKO_TAG from the IMAGE_TAG file that
# chromeless-build.sh Step 9 wrote on the build node, so every push gets
# a unique SHA-derived tag and we never silently overwrite a prior tag.
#
# Operator hard-constraint (per CV2 ops norms): T7/T8 only. nvenc lives
# on T5 and is out of scope for the CV2 M-stack push lane.
#
# Usage:
#   ./infra/k8s/chromeless-build/chromeless-kaniko-push.sh <node> [variant-label]
#
# Examples:
#   ./infra/k8s/chromeless-build/chromeless-kaniko-push.sh triform-7
#   ./infra/k8s/chromeless-build/chromeless-kaniko-push.sh triform-7 x264-t7
#   ./infra/k8s/chromeless-build/chromeless-kaniko-push.sh triform-8 sw
#
# Override tag (skip IMAGE_TAG file read — emergency / repush scenario):
#   CHROMELESS_KANIKO_TAG=cr7727-deadbeef \
#     ./infra/k8s/chromeless-build/chromeless-kaniko-push.sh triform-7
#
# Override host path (nvenc fallback — must also pass node):
#   CONTEXT_PATH=/mnt/data/chromeless-build-t5/chromium-src/artifacts/context \
#     ./infra/k8s/chromeless-build/chromeless-kaniko-push.sh triform-5 nvenc

set -euo pipefail

NODE="${1:-}"
VARIANT_LABEL="${2:-${KANIKO_VARIANT_LABEL:-}}"

if [[ -z "${NODE}" ]]; then
  echo "ERROR: missing required arg: node (triform-7 or triform-8)" >&2
  echo "Usage: $0 <node> [variant-label]" >&2
  exit 2
fi

case "${NODE}" in
  triform-7|triform-8)
    : ;;
  triform-5)
    # Allowed only with explicit CONTEXT_PATH (nvenc lane). Warn loudly.
    if [[ -z "${CONTEXT_PATH:-}" ]]; then
      echo "ERROR: ${NODE} requires CONTEXT_PATH override (nvenc lane)" >&2
      exit 2
    fi
    echo "WARN: ${NODE} is outside the CV2 M-stack T7/T8 constraint — proceeding because CONTEXT_PATH is explicit" >&2
    ;;
  *)
    echo "ERROR: node ${NODE} is not on the T7/T8 allowlist (CV2 operator hard-constraint)" >&2
    exit 2
    ;;
esac

# Default variant label by node (matches build-job manifest labels).
if [[ -z "${VARIANT_LABEL}" ]]; then
  case "${NODE}" in
    triform-7) VARIANT_LABEL="x264-t7" ;;
    triform-8) VARIANT_LABEL="sw" ;;
    triform-5) VARIANT_LABEL="nvenc" ;;
  esac
fi

# Default kaniko build-context path (matches build-job hostPath layout on T7/T8).
CONTEXT_PATH="${CONTEXT_PATH:-/var/lib/longhorn/chromeless-build/chromium-src/artifacts/context}"

# Derive the destination tag from IMAGE_TAG written by chromeless-build.sh
# Step 9 unless the operator passed it explicitly.
if [[ -z "${CHROMELESS_KANIKO_TAG:-}" ]]; then
  echo "Reading IMAGE_TAG from ${NODE}:${CONTEXT_PATH}/IMAGE_TAG ..."
  CHROMELESS_KANIKO_TAG="$(ssh -o BatchMode=yes "${NODE}" \
    "cat ${CONTEXT_PATH}/IMAGE_TAG" 2>/dev/null | tr -d '[:space:]')"
  if [[ -z "${CHROMELESS_KANIKO_TAG}" ]]; then
    echo "ERROR: could not read IMAGE_TAG from ${NODE}:${CONTEXT_PATH}/IMAGE_TAG" >&2
    echo "  Either the build hasn't reached Step 9 yet, the path is wrong," >&2
    echo "  or ssh ${NODE} is denied. Pass CHROMELESS_KANIKO_TAG=... to override." >&2
    exit 1
  fi
fi

# Sanity-check tag shape: expect cr<digits>-<hex-sha> per chromeless-build.sh
# Step 9 ("image_tag=cr${CHROMIUM_BRANCH_NUMBER}-${CHROMELESS_GIT_SHA}").
if ! [[ "${CHROMELESS_KANIKO_TAG}" =~ ^cr[0-9]+-[A-Za-z0-9._-]+$ ]]; then
  echo "WARN: CHROMELESS_KANIKO_TAG '${CHROMELESS_KANIKO_TAG}' does not match" >&2
  echo "      expected pattern 'cr<branch>-<sha>'. Proceeding anyway." >&2
fi

# Tag-suffixed Job name (matches metadata.name template in the YAML).
JOB_NAME="chromeless-kaniko-push-${CHROMELESS_KANIKO_TAG}"

cat <<EOF
─── chromeless kaniko push ───
  node            : ${NODE}
  variant label   : ${VARIANT_LABEL}
  context path    : ${CONTEXT_PATH}
  destination tag : ${CHROMELESS_KANIKO_TAG}
  job name        : ${JOB_NAME}
──────────────────────────────
EOF

REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
TEMPLATE="${REPO_ROOT}/infra/k8s/chromeless-build/chromeless-kaniko-push.yaml"

if [[ ! -f "${TEMPLATE}" ]]; then
  echo "ERROR: template not found: ${TEMPLATE}" >&2
  exit 1
fi

# envsubst with explicit allowlist — leaves every other ${VAR} in the
# YAML (e.g. ${VARIANT} in the header comments, ${CHROMELESS_WORK_ROOT}
# in the hostPath comments) untouched. The kaniko $(VAR) args use K8s
# downward-API syntax and are NEVER touched by envsubst regardless.
RENDERED="$(CHROMELESS_KANIKO_TAG="${CHROMELESS_KANIKO_TAG}" \
            KANIKO_NODE="${NODE}" \
            KANIKO_VARIANT_LABEL="${VARIANT_LABEL}" \
            envsubst '${CHROMELESS_KANIKO_TAG} ${KANIKO_NODE} ${KANIKO_VARIANT_LABEL}' \
            < "${TEMPLATE}")"

echo "Applying rendered manifest..."
echo "${RENDERED}" | kubectl apply -f -

echo "Streaming kaniko logs into /tmp/kaniko-push-${CHROMELESS_KANIKO_TAG}.log ..."
# Wait briefly for the pod to be scheduled, then follow.
for _ in {1..30}; do
  POD="$(kubectl -n chromeless-build get pods \
    -l job-name="${JOB_NAME}" \
    -o jsonpath='{.items[0].metadata.name}' 2>/dev/null || true)"
  if [[ -n "${POD}" ]]; then
    break
  fi
  sleep 2
done

if [[ -z "${POD:-}" ]]; then
  echo "ERROR: kaniko pod did not appear within 60s; check 'kubectl -n chromeless-build get pods'" >&2
  exit 1
fi

echo "Following pod ${POD}..."
kubectl -n chromeless-build logs -f "${POD}" 2>&1 \
  | tee "/tmp/kaniko-push-${CHROMELESS_KANIKO_TAG}.log"

# Surface final status so the operator can see PASS/FAIL at a glance.
STATUS="$(kubectl -n chromeless-build get job "${JOB_NAME}" \
  -o jsonpath='{.status.conditions[?(@.type=="Complete")].status}' 2>/dev/null || true)"
FAILED="$(kubectl -n chromeless-build get job "${JOB_NAME}" \
  -o jsonpath='{.status.conditions[?(@.type=="Failed")].status}' 2>/dev/null || true)"

if [[ "${STATUS}" == "True" ]]; then
  echo
  echo "✓ kaniko-push SUCCEEDED — pushed registry.triform.cloud/chromeless/chromeless:${CHROMELESS_KANIKO_TAG}"
  exit 0
fi
if [[ "${FAILED}" == "True" ]]; then
  echo
  echo "✗ kaniko-push FAILED — see /tmp/kaniko-push-${CHROMELESS_KANIKO_TAG}.log" >&2
  exit 1
fi
echo
echo "? kaniko-push status indeterminate — inspect 'kubectl -n chromeless-build describe job ${JOB_NAME}'" >&2
exit 1

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
RELEASE_RECORD="${REPO_ROOT}/build/guest-release.json"

# ---------------------------------------------------------------------
# write_guest_release_record — the producer's provenance statement.
#
# Called ONLY on a confirmed-successful push (Job condition Complete=True),
# so the file can never claim an image that does not exist in the registry.
#
# Why this lives here and not in chromeless-build.sh: the build script runs
# inside the K8s build Job, on a node, as uid 1000, against a bootstrap
# clone with no push credentials and no working tree to commit into. It
# genuinely cannot update a git-tracked file. THIS script, by contrast,
# already runs on the operator's workstation from a real checkout — it
# computes REPO_ROOT above to find its own template. So the record is
# written where a human is already sitting in the repo that owns it.
#
# Honest limitation, stated so nobody mistakes this for a closed loop:
# this writes the file, it does NOT commit or push it. The operator must
# commit the result (the script prints the exact command). A push
# performed by someone who never commits the diff leaves the record stale
# — which the consumer-side check will then catch as a disagreement
# between the monorepo pin and this file. Stale-and-caught is the
# designed failure mode; silently-wrong is the one we are eliminating.
#
# The SHA is read from the BUILD NODE, never from the local checkout.
# The operator's working tree is routinely on some other branch (that is
# exactly how the current drift happened), so a local `git rev-parse`
# would confidently record a commit that had nothing to do with the
# binary. The tag itself carries the build's short SHA, so we expand that
# to a full 40-char SHA via the local object database and verify the
# expansion round-trips back to the tag before trusting it.
# ---------------------------------------------------------------------
write_guest_release_record() {
  local tag="${CHROMELESS_KANIKO_TAG}"
  local image="registry.triform.cloud/chromeless/chromeless:${tag}"
  local short_sha full_sha digest built_at

  # cr<branch>-<sha> -> <sha>. Anything else (an operator override like
  # cr7727-hotfix) is not a commit and must not be recorded as one.
  short_sha="${tag#cr*-}"
  if [[ "${short_sha}" == "${tag}" || ! "${short_sha}" =~ ^[0-9a-f]{7,40}$ ]]; then
    echo "WARN: tag '${tag}' carries no commit-shaped SHA — NOT writing ${RELEASE_RECORD}." >&2
    echo "      The provenance record is left untouched rather than filled with a guess." >&2
    return 0
  fi

  # Expand to the full SHA and verify it round-trips: if the object is
  # absent (operator's clone never fetched that branch) we must NOT
  # invent one. Record what we can verify, flag what we cannot.
  full_sha="$(git -C "${REPO_ROOT}" rev-parse --verify --quiet "${short_sha}^{commit}" 2>/dev/null || true)"
  if [[ -z "${full_sha}" ]]; then
    echo "WARN: commit ${short_sha} is not in the local object DB (git fetch --all?)." >&2
    echo "      Recording the short SHA only — a checker will treat it as unresolvable." >&2
    full_sha="${short_sha}"
  fi

  # Manifest digest — the immutable identity. A tag can be re-pushed to
  # point somewhere else; a digest cannot. Best-effort: crane may not be
  # installed, and a missing digest must not fail an otherwise-good push.
  digest=""
  if command -v crane >/dev/null 2>&1; then
    digest="$(crane digest "${image}" 2>/dev/null || true)"
  fi
  if [[ -z "${digest}" ]]; then
    echo "WARN: could not resolve manifest digest for ${image} (crane missing or registry auth)." >&2
    echo "      Writing the record without a digest; re-run with crane available to fill it." >&2
  fi

  built_at="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

  mkdir -p "$(dirname "${RELEASE_RECORD}")"
  cat > "${RELEASE_RECORD}" <<EOF
{
  "schema": "chromeless.guest-release/v1",
  "commit": "${full_sha}",
  "image": "${image}",
  "digest": "${digest}",
  "dockerfile": "build/Dockerfile.runtime",
  "built_at": "${built_at}",
  "recorded_by": "chromeless-kaniko-push.sh"
}
EOF

  echo
  echo "─── provenance record written ───"
  echo "  ${RELEASE_RECORD}"
  echo "  commit : ${full_sha}"
  echo "  image  : ${image}"
  echo "  digest : ${digest:-<unresolved>}"
  echo
  echo "  ACTION REQUIRED — this script does not commit. Run:"
  echo "    git -C ${REPO_ROOT} add build/guest-release.json && \\"
  echo "      git -C ${REPO_ROOT} commit -m 'chore(cv2-build): record guest release ${tag}'"
  echo
  if ! git -C "${REPO_ROOT}" merge-base --is-ancestor "${full_sha}" origin/main 2>/dev/null; then
    echo "  ⚠ ${full_sha} is NOT an ancestor of origin/main." >&2
    echo "    You are about to ship a guest whose source is not on main." >&2
    echo "    Land it on main before this image reaches production." >&2
    echo
  fi
}

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
  write_guest_release_record
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

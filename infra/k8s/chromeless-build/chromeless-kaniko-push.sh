#!/usr/bin/env bash
# chromeless-kaniko-push.sh — canonical apply wrapper for chromeless-kaniko-push.yaml
#
# Why this script exists: the YAML is a template with three envsubst
# placeholders (CHROMELESS_KANIKO_TAG, KANIKO_NODE, KANIKO_VARIANT_LABEL,
# KANIKO_JOB_NAME).
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

  # `|| true` is LOAD-BEARING, not defensive noise.
  #
  # This was `CHROMELESS_KANIKO_TAG="$(ssh ... )"` with no guard, under
  # `set -euo pipefail`. When ssh fails — no key, host not in known_hosts, or
  # simply running from a laptop that cannot reach cluster nodes — the failing
  # command substitution makes the ASSIGNMENT non-zero, and `set -e` kills the
  # script right there. So the carefully written error block below could never
  # execute. The operator saw:
  #
  #     Reading IMAGE_TAG from triform-7:/var/.../IMAGE_TAG ...
  #     $ echo $?
  #     0
  #
  # No error, no tag, exit 0 — a silent no-op that looks like success. Anyone
  # scripting on top of it (an auto-bump loop, a CI step) would treat that as
  # "push done" and carry on with no image. Reproduced in isolation: a bare
  # `X="$(ssh badhost cat /nope)"` under `set -e` exits 255 at that line.
  #
  # With `|| true` the assignment always succeeds, the emptiness check runs,
  # and the operator gets the diagnosis the author intended to give them.
  CHROMELESS_KANIKO_TAG="$(ssh -o BatchMode=yes "${NODE}" \
    "cat ${CONTEXT_PATH}/IMAGE_TAG" 2>/dev/null | tr -d '[:space:]' || true)"

  if [[ -z "${CHROMELESS_KANIKO_TAG}" ]]; then
    echo "ERROR: could not read IMAGE_TAG from ${NODE}:${CONTEXT_PATH}/IMAGE_TAG" >&2
    echo "  Either the build hasn't reached Step 9 yet, the path is wrong," >&2
    echo "  or ssh ${NODE} is denied (e.g. you are running this from a host" >&2
    echo "  with no SSH access to cluster nodes — kubectl alone is not enough)." >&2
    echo >&2
    echo "  Read the tag with kubectl instead, then pass it explicitly:" >&2
    echo "    kubectl run tagread --rm -i --restart=Never --image=busybox \\" >&2
    echo "      --overrides='{\"spec\":{\"nodeName\":\"${NODE}\"}}' -- \\" >&2
    echo "      cat ${CONTEXT_PATH}/IMAGE_TAG" >&2
    echo "    CHROMELESS_KANIKO_TAG=<tag> $0 ${NODE} ${VARIANT_LABEL:-}" >&2
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
PUSH_ATTEMPT="${CHROMELESS_PUSH_ATTEMPT:-1}"
case "${PUSH_ATTEMPT}" in
  1|2|3) ;;
  *) echo "ERROR: CHROMELESS_PUSH_ATTEMPT must be 1, 2 or 3" >&2; exit 2 ;;
esac
BASE_JOB_NAME="chromeless-kaniko-push-${CHROMELESS_KANIKO_TAG}"
JOB_NAME="${BASE_JOB_NAME}"
if [[ "${PUSH_ATTEMPT}" != 1 ]]; then
  JOB_NAME="${BASE_JOB_NAME}-attempt${PUSH_ATTEMPT}"
fi

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

# A retry is a distinct job. Preserve the failed attempt and refuse to overlap
# an active push, overwrite a published tag, or switch the staged build source.
if [[ "${PUSH_ATTEMPT}" != 1 ]]; then
  if ! [[ "${CHROMELESS_KANIKO_TAG}" =~ ^cr[0-9]+-[0-9a-f]{7,40}$ ]]; then
    echo "ERROR: retry requires a commit-shaped build tag" >&2
    exit 1
  fi
  PREVIOUS_JOB="${BASE_JOB_NAME}"
  if [[ "${PUSH_ATTEMPT}" == 3 ]]; then PREVIOUS_JOB="${BASE_JOB_NAME}-attempt2"; fi
  PREVIOUS_JSON="$(kubectl -n chromeless-build get job "${PREVIOUS_JOB}" -o json)"
  python3 "${REPO_ROOT}/infra/k8s/chromeless-build/check-push-retry.py" \
    --name "${PREVIOUS_JOB}" --node "${NODE}" --tag "${CHROMELESS_KANIKO_TAG}" \
    --context "${CONTEXT_PATH}" <<<"${PREVIOUS_JSON}"
fi

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
  if ! [[ "${full_sha}" =~ ^[0-9a-f]{40}$ && "${full_sha}" == "${short_sha}"* ]]; then
    echo "ERROR: build commit cannot be resolved; fetch that exact source before recovering its record" >&2
    return 1
  fi

  # Manifest digest — the immutable identity. A tag can be re-pushed to
  # point somewhere else; a digest cannot. Best-effort: crane may not be
  # installed, and a missing digest must not fail an otherwise-good push.
  digest=""
  if command -v crane >/dev/null 2>&1; then
    digest="$(crane digest "${image}" 2>/dev/null || true)"
  fi
  if ! [[ "${digest}" =~ ^sha256:[0-9a-f]{64}$ ]]; then
    echo "ERROR: could not resolve the published manifest digest; release record left untouched" >&2
    return 1
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
  # Rewrite the deploy manifest's pin IN THE SAME BREATH as the provenance
  # record. Writing only guest-release.json is how the two drift apart, and
  # that drift is not cosmetic: `deploy.sh` applies stack.yaml, so a stale pin
  # there means every deploy silently ROLLS THE CLUSTER BACK to an older guest.
  #
  # Measured 2026-08-24: stack.yaml pinned an image 64 commits behind the fix
  # under test, the worker Deployment reached revision 66 in a day as apply and
  # `kubectl set image` fought each other, and a user lost the day to it —
  # tests passing against an image they never saw. main has carried this same
  # drift since 2026-08-11 (guest-release cr7727-44f2e20dece6 vs stack.yaml
  # cr7727-6047599546e3).
  #
  # `make lint-deploy-pin` fails when they disagree; this keeps them agreeing
  # in the first place, so the lint is a backstop rather than a chore.
  STACK_MANIFEST="${REPO_ROOT}/infra/k8s/standalone/stack.yaml"
  if [[ -f "${STACK_MANIFEST}" ]]; then
    if python3 - "${STACK_MANIFEST}" "${image}" <<'PYEOF'; then
import pathlib, re, sys
path, image = pathlib.Path(sys.argv[1]), sys.argv[2]
text = path.read_text()
new, n = re.subn(
    r'(?m)^(\s*image:\s*)registry\.[\w.]+/chromeless/chromeless:\S+',
    lambda m: m.group(1) + image, text)
if n:
    path.write_text(new)
sys.exit(0 if n else 1)
PYEOF
      echo "  updated stack.yaml pin -> ${tag}"
    else
      echo "  ⚠ could not update the worker pin in stack.yaml — do it by hand," >&2
      echo "    or the next deploy.sh rolls the cluster back. See" >&2
      echo "    make lint-deploy-pin." >&2
    fi
  fi

  echo "  ACTION REQUIRED — this script does not commit. Run:"
  echo "    git -C ${REPO_ROOT} add build/guest-release.json infra/k8s/standalone/stack.yaml && \\"
  echo "      git -C ${REPO_ROOT} commit -m 'chore(cv2-build): record guest release ${tag}'"
  echo "  BOTH files — committing only the json is what made main drift."
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

command -v crane >/dev/null 2>&1 || { echo "ERROR: crane is required to verify image identity" >&2; exit 1; }
EXISTING_JSON="$(kubectl -n chromeless-build get job "${JOB_NAME}" --ignore-not-found -o json)"
if [[ -n "${EXISTING_JSON}" ]]; then
  # Recover a lost local release record from a completed matching job. Never
  # recreate it or repush its tag. Active/failed/mismatched jobs are refused.
  python3 "${REPO_ROOT}/infra/k8s/chromeless-build/check-push-retry.py" \
    --name "${JOB_NAME}" --node "${NODE}" --tag "${CHROMELESS_KANIKO_TAG}" \
    --context "${CONTEXT_PATH}" --required-state Complete <<<"${EXISTING_JSON}"
  write_guest_release_record
  exit 0
fi
if DIGEST_PROBE="$(crane digest "registry.triform.cloud/chromeless/chromeless:${CHROMELESS_KANIKO_TAG}" 2>&1)"; then
  echo "ERROR: this tag already has a manifest; refusing to overwrite it" >&2
  exit 1
fi
case "${DIGEST_PROBE}" in
  *MANIFEST_UNKNOWN*) ;;
  *) echo "ERROR: could not prove the tag is absent; registry errors are not absence" >&2; exit 1 ;;
esac

# envsubst with explicit allowlist — leaves every other ${VAR} in the
# YAML (e.g. ${VARIANT} in the header comments, ${CHROMELESS_WORK_ROOT}
# in the hostPath comments) untouched. The kaniko $(VAR) args use K8s
# downward-API syntax and are NEVER touched by envsubst regardless.
RENDERED="$(CHROMELESS_KANIKO_TAG="${CHROMELESS_KANIKO_TAG}" \
            KANIKO_JOB_NAME="${JOB_NAME}" \
            KANIKO_NODE="${NODE}" \
            KANIKO_VARIANT_LABEL="${VARIANT_LABEL}" \
            envsubst '${CHROMELESS_KANIKO_TAG} ${KANIKO_NODE} ${KANIKO_VARIANT_LABEL} ${KANIKO_JOB_NAME}' \
            < "${TEMPLATE}")"

echo "Creating push attempt (existing jobs are preserved)..."
echo "${RENDERED}" | kubectl create -f -

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

# Wait until the pod is actually FOLLOWABLE before streaming.
#
# The loop above waits for the pod to EXIST, which is not the same thing.
# `kubectl logs -f` against a pod still in ContainerCreating fails instantly
# with
#
#   Error from server (BadRequest): container "kaniko" ... is waiting to start
#
# and because it is the head of a pipe, the failure is invisible: the pipeline
# succeeds, the script sails past, and the STATUS check below runs against a
# Job that has not finished. The push itself then SUCCEEDS while the provenance
# record is never written — not even locally.
#
# That is the leak behind the drift on main: guest-release.json still records
# cr7727-44f2e20dece6 from 2026-08-11 while images have shipped since. Two more
# instances happened on 2026-08-24, and I hit the same BadRequest four times
# that day pushing by hand. It needs no mistake by anyone — just a pod that
# takes a few seconds to start.
#
# The existing comment says the record "can never claim an image that does not
# exist in the registry", which is true. This is the other direction: an image
# can exist that the record never learns about.
echo "Waiting for ${POD} to start..."
for _ in $(seq 1 60); do
  phase="$(kubectl -n chromeless-build get pod "${POD}" \
    -o jsonpath='{.status.phase}' 2>/dev/null || true)"
  case "${phase}" in
    Running|Succeeded|Failed) break ;;
  esac
  sleep 2
done

echo "Following pod ${POD}..."
# `|| true` so a mid-stream disconnect does not abort the run before the Job
# condition is read — the condition, not the log stream, is the authority.
{ kubectl -n chromeless-build logs -f "${POD}" 2>&1 || true; } \
  | tee "/tmp/kaniko-push-${CHROMELESS_KANIKO_TAG}.log"

# The log observer can disconnect. Check BOTH terminal conditions: waiting only
# for Complete delays a known failure by ten minutes and changes no evidence.
for _ in $(seq 1 120); do
  CONDITIONS="$(kubectl -n chromeless-build get job "${JOB_NAME}" \
    -o 'jsonpath={range .status.conditions[*]}{.type}={.status}{"\n"}{end}' 2>/dev/null || true)"
  case "${CONDITIONS}" in
    *Complete=True*|*Failed=True*) break ;;
  esac
  sleep 2
done

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

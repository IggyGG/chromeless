#!/usr/bin/env bash
#
# scripts/dev-push-streamer.sh — fast-path build & deploy for streamer-page
# changes only.
#
# WHY
#
# The full chromeless image is a Chromium-from-source build that takes
# ~4 hours cold (see infra/k8s/chromeless-build/). Streamer-page edits
# (capture/streamer-page/streamer.js + index.html) don't touch any C++
# source — they're just COPY'd into the runtime image at
# infra/Dockerfile:139:
#
#     COPY capture/streamer-page/ /opt/cloud-browser/streamer/
#
# So a streamer-only iteration only needs:
#   1. Re-run `docker build -f infra/Dockerfile .` (mostly cached layers;
#      only the streamer-page COPY layer + its descendants change).
#   2. Push to a non-prod tag at registry.triform.cloud (NOT the prod
#      tag `cr7727-sw` which the operator-managed kaniko Job pushes).
#   3. Roll the dev cb-browserless / chromeless deployment to pick up
#      the new tag.
#
# Total round-trip target: ~5 min. Cold cache the docker build is the
# slowest atom (~3-5 min on a fresh host); subsequent runs are
# sub-minute when the cache is warm.
#
# WHEN NOT TO USE
#
# * Any change to a Go service in capture/, signaling/, or
#   chromeless-metrics-sidecar — these need their respective build
#   stages re-run, which is fine here, but they're also picked up by
#   the regular CI release pipeline (.github/workflows/release.yml),
#   so for a sidecar-focused iteration that pipeline is the right
#   path.
# * Any change to Chromium itself — needs the kaniko build job
#   (infra/k8s/chromeless-build/build-job-x264.yaml), not this.
# * Production / staging targets — this script ONLY pushes a dev-tag
#   (default `dev-<short-sha>`) and rolls the dev cluster. Never use
#   it against integration / triform.wtf.
#
# CREDENTIALS
#
# Pushing to registry.triform.cloud requires either:
#   * `docker login registry.triform.cloud` already done in the shell,
#     OR
#   * a docker config.json at ~/.docker/config.json with auth for
#     the registry, OR
#   * `REGISTRY_USER` + `REGISTRY_PASSWORD` env vars (we'll do
#     `docker login` ourselves and immediately `docker logout` at the
#     end).
#
# Rolling the deployment requires kubectl access to the dev cluster
# with permissions on the target namespace (default: chromeless;
# override with --namespace). If your kubeconfig isn't already set
# up, the script bails before doing the docker build to avoid
# wasting that time.
#
# USAGE
#
#   scripts/dev-push-streamer.sh                       # full path; uses HEAD short-sha
#   scripts/dev-push-streamer.sh --tag my-feature      # push registry.triform.cloud/chromeless/chromeless:dev-my-feature
#   scripts/dev-push-streamer.sh --skip-rollout        # build + push only; don't kubectl set image
#   scripts/dev-push-streamer.sh --skip-push           # build only; don't push (smoke local Dockerfile)
#   scripts/dev-push-streamer.sh --namespace chromeless --deployment chromeless-browserless
#                                                      # explicit target (default tries both)
#   scripts/dev-push-streamer.sh --registry registry.triform.cloud --image chromeless/chromeless
#                                                      # override registry/image for one-off targets
#

set -euo pipefail

# ---- defaults ---------------------------------------------------------------

REGISTRY="${REGISTRY:-registry.triform.cloud}"
IMAGE_REPO="${IMAGE_REPO:-chromeless/chromeless}"
NAMESPACE="${NAMESPACE:-chromeless}"
# Try a couple of well-known deployment names; the user can override.
DEPLOYMENT="${DEPLOYMENT:-}"
CONTAINER="${CONTAINER:-chromeless}"

TAG=""
SKIP_PUSH=false
SKIP_ROLLOUT=false
DOCKERFILE="infra/Dockerfile"
BUILD_CONTEXT="."

# ---- helpers ---------------------------------------------------------------

red()    { printf "\033[0;31m%s\033[0m\n" "$*" >&2; }
yellow() { printf "\033[0;33m%s\033[0m\n" "$*" >&2; }
green()  { printf "\033[0;32m%s\033[0m\n" "$*"; }
info()   { printf "\033[0;36m[%s]\033[0m %s\n" "$(date +%H:%M:%S)" "$*"; }

usage() {
  sed -n '3,80p' "$0"
  exit "${1:-1}"
}

# ---- arg parse -------------------------------------------------------------

while [[ $# -gt 0 ]]; do
  case "$1" in
    --tag)        TAG="$2"; shift 2 ;;
    --tag=*)      TAG="${1#*=}"; shift ;;
    --skip-push)  SKIP_PUSH=true; shift ;;
    --skip-rollout) SKIP_ROLLOUT=true; shift ;;
    --namespace)  NAMESPACE="$2"; shift 2 ;;
    --namespace=*) NAMESPACE="${1#*=}"; shift ;;
    --deployment) DEPLOYMENT="$2"; shift 2 ;;
    --deployment=*) DEPLOYMENT="${1#*=}"; shift ;;
    --container)  CONTAINER="$2"; shift 2 ;;
    --container=*) CONTAINER="${1#*=}"; shift ;;
    --registry)   REGISTRY="$2"; shift 2 ;;
    --registry=*) REGISTRY="${1#*=}"; shift ;;
    --image)      IMAGE_REPO="$2"; shift 2 ;;
    --image=*)    IMAGE_REPO="${1#*=}"; shift ;;
    --dockerfile) DOCKERFILE="$2"; shift 2 ;;
    --dockerfile=*) DOCKERFILE="${1#*=}"; shift ;;
    --help|-h)    usage 0 ;;
    *) red "Unknown argument: $1"; usage 2 ;;
  esac
done

# ---- preflight -------------------------------------------------------------

# Resolve repo root by climbing from the script. We need the build context
# to be the repo root because infra/Dockerfile COPYs paths relative to it.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

if [[ ! -f "$DOCKERFILE" ]]; then
  red "Dockerfile not found at $DOCKERFILE"
  exit 2
fi
if [[ ! -d capture/streamer-page ]]; then
  red "capture/streamer-page/ missing — wrong repo root?"
  exit 2
fi

# Compute tag from HEAD short-sha if not provided.
if [[ -z "$TAG" ]]; then
  if SHORT_SHA="$(git rev-parse --short=12 HEAD 2>/dev/null)"; then
    DIRTY=""
    if ! git diff-index --quiet HEAD -- 2>/dev/null; then
      DIRTY="-dirty"
    fi
    TAG="dev-${SHORT_SHA}${DIRTY}"
  else
    TAG="dev-$(date +%Y%m%d-%H%M%S)"
  fi
fi

# Defensive guard: never let the user typo --tag into a prod-shape
# tag. The shape (not the literal name) is what matters — the operator
# kaniko Job pushes cr7727-* shapes, Flux ImageRepository feeds
# release*/main*/prod*/latest/stable, and all of those are off-limits
# from this dev fast-path. Glob-matches catch new variants
# (cr7727-release, cr7727-v2.1.0, release-2026-05, prod-eu-west-1, …)
# without us having to enumerate them as they appear.
case "$TAG" in
  cr7727-*|release*|prod*|main*|latest|stable)
    red "Refusing to use tag '$TAG' — matches a production-shape tag (cr7727-*/release*/prod*/main*/latest/stable). dev-* tags only."
    exit 2
    ;;
esac

FULL_IMAGE="${REGISTRY}/${IMAGE_REPO}:${TAG}"
info "target image: $FULL_IMAGE"

# ---- preflight: docker daemon ----------------------------------------------

if ! docker info >/dev/null 2>&1; then
  red "Docker daemon not reachable. Possible causes:"
  red "  * Docker Desktop / dockerd not running on this host."
  red "  * DOCKER_HOST points at a remote daemon that's down."
  red "  * Running inside a sandbox without a docker socket bind mount."
  red "If iterating from a Coder workspace, this script likely needs to"
  red "be run from a host that has the docker socket. Alternative: push"
  red "via the GitHub Actions workflow_dispatch on .github/workflows/release.yml"
  red "with a dev tag, or use the kaniko build Job in"
  red "infra/k8s/chromeless-build/."
  exit 3
fi

# ---- preflight: registry credentials ---------------------------------------

if ! $SKIP_PUSH; then
  # If the user passed env credentials, log in now (and arrange to log
  # out at exit). Otherwise we trust whatever auth the local docker
  # config has.
  if [[ -n "${REGISTRY_USER:-}" && -n "${REGISTRY_PASSWORD:-}" ]]; then
    info "REGISTRY_USER set; running docker login..."
    echo "$REGISTRY_PASSWORD" | docker login "$REGISTRY" \
      --username "$REGISTRY_USER" --password-stdin
    LOGGED_IN_HERE=true
  else
    LOGGED_IN_HERE=false
    # Probe with a no-op `docker manifest inspect` against the known
    # dev tag. We don't fail if it 404s — that's expected when this
    # is the first push of $TAG. We DO fail if the response is a
    # 401 / 403, which we can't tell apart from manifest-inspect's
    # output, so we settle for a softer warning.
    if ! docker manifest inspect "${REGISTRY}/${IMAGE_REPO}:dev-cred-probe-noexist" >/dev/null 2>&1; then
      yellow "could not probe registry auth (this may be fine if creds work)."
      yellow "if push fails, set REGISTRY_USER + REGISTRY_PASSWORD or run"
      yellow "  docker login $REGISTRY"
      yellow "before re-invoking."
    fi
  fi
fi

# ---- preflight: kubectl ----------------------------------------------------

if ! $SKIP_ROLLOUT; then
  if ! command -v kubectl >/dev/null 2>&1; then
    red "kubectl not found in PATH; --skip-rollout to push-only."
    exit 4
  fi
  if ! kubectl version --client >/dev/null 2>&1; then
    red "kubectl version --client failed; install or fix PATH."
    exit 4
  fi
  if ! kubectl get ns "$NAMESPACE" >/dev/null 2>&1; then
    red "namespace '$NAMESPACE' not found in current kubectl context."
    red "  current context: $(kubectl config current-context 2>/dev/null || echo '(none)')"
    red "Use --namespace <ns> or fix kubeconfig before re-running."
    exit 4
  fi
fi

# ---- build -----------------------------------------------------------------

info "docker build (context=$BUILD_CONTEXT, dockerfile=$DOCKERFILE)"
DOCKER_BUILD_START=$(date +%s)
docker build \
  --file "$DOCKERFILE" \
  --tag "$FULL_IMAGE" \
  "$BUILD_CONTEXT"
DOCKER_BUILD_END=$(date +%s)
green "docker build OK in $((DOCKER_BUILD_END - DOCKER_BUILD_START))s"

# Quick post-build sanity: confirm the streamer.js we just built is the
# one in the working tree (md5 round-trip). If they don't match, the
# build cached an older layer or something else is off.
if command -v md5sum >/dev/null 2>&1; then
  HOST_MD5="$(md5sum capture/streamer-page/streamer.js | awk '{print $1}')"
  IMG_MD5="$(docker run --rm --entrypoint sh "$FULL_IMAGE" \
    -c 'md5sum /opt/cloud-browser/streamer/streamer.js' \
    2>/dev/null | awk '{print $1}' || true)"
  if [[ -n "$IMG_MD5" && "$HOST_MD5" != "$IMG_MD5" ]]; then
    red "streamer.js mismatch between host ($HOST_MD5) and image ($IMG_MD5)."
    red "the build picked up a cached layer; re-run with --no-cache?"
    exit 5
  fi
  info "streamer.js md5 matches host ($HOST_MD5)"
fi

# ---- push ------------------------------------------------------------------

if $SKIP_PUSH; then
  green "build complete; skipping push (--skip-push)."
  exit 0
fi

info "docker push $FULL_IMAGE"
PUSH_START=$(date +%s)
if ! docker push "$FULL_IMAGE"; then
  red "push failed. Common causes:"
  red "  * not logged in to $REGISTRY (run \`docker login $REGISTRY\`)"
  red "  * image repo $IMAGE_REPO doesn't exist or you lack push access"
  red "  * network egress blocked from this host"
  if [[ "${LOGGED_IN_HERE:-false}" == "true" ]]; then
    docker logout "$REGISTRY" >/dev/null 2>&1 || true
  fi
  exit 6
fi
PUSH_END=$(date +%s)
green "push OK in $((PUSH_END - PUSH_START))s"

# ---- rollout ---------------------------------------------------------------

if $SKIP_ROLLOUT; then
  green "skipping rollout (--skip-rollout)."
  green "image pushed: $FULL_IMAGE"
  if [[ "${LOGGED_IN_HERE:-false}" == "true" ]]; then
    docker logout "$REGISTRY" >/dev/null 2>&1 || true
  fi
  exit 0
fi

# Resolve deployment name. If the user passed --deployment, use it.
# Otherwise probe a couple of well-known names — the workload was
# renamed across the chromeless rename phase 2 (cb-browserless →
# chromeless-browserless), so accept either.
declare -a CANDIDATES
if [[ -n "$DEPLOYMENT" ]]; then
  CANDIDATES=("$DEPLOYMENT")
else
  CANDIDATES=("chromeless-browserless" "cb-browserless" "chromeless")
fi

ROLLED=""
for d in "${CANDIDATES[@]}"; do
  if kubectl -n "$NAMESPACE" get deploy "$d" >/dev/null 2>&1; then
    info "kubectl set image deploy/$d $CONTAINER=$FULL_IMAGE -n $NAMESPACE"
    if kubectl -n "$NAMESPACE" set image \
        "deploy/$d" "$CONTAINER=$FULL_IMAGE"; then
      ROLLED="$d"
      break
    else
      yellow "set image failed on $d; trying next candidate..."
    fi
  fi
done

if [[ -z "$ROLLED" ]]; then
  red "none of these deployments exist in namespace '$NAMESPACE': ${CANDIDATES[*]}"
  red "use --deployment <name> to specify one explicitly, or --skip-rollout."
  if [[ "${LOGGED_IN_HERE:-false}" == "true" ]]; then
    docker logout "$REGISTRY" >/dev/null 2>&1 || true
  fi
  exit 7
fi

info "kubectl rollout status deploy/$ROLLED -n $NAMESPACE (timeout 5m)"
ROLL_START=$(date +%s)
if ! kubectl -n "$NAMESPACE" rollout status "deploy/$ROLLED" --timeout=5m; then
  red "rollout did not complete within 5m; falling back is manual."
  red "to rollback: kubectl -n $NAMESPACE rollout undo deploy/$ROLLED"
  if [[ "${LOGGED_IN_HERE:-false}" == "true" ]]; then
    docker logout "$REGISTRY" >/dev/null 2>&1 || true
  fi
  exit 8
fi
ROLL_END=$(date +%s)
green "rollout OK in $((ROLL_END - ROLL_START))s"

if [[ "${LOGGED_IN_HERE:-false}" == "true" ]]; then
  docker logout "$REGISTRY" >/dev/null 2>&1 || true
fi

# ---- summary ---------------------------------------------------------------

ELAPSED=$(( ROLL_END - DOCKER_BUILD_START ))
green "=================================================================="
green "dev-push-streamer.sh complete in ${ELAPSED}s"
green "  image:      $FULL_IMAGE"
green "  namespace:  $NAMESPACE"
green "  deployment: $ROLLED"
green "  container:  $CONTAINER"
green "=================================================================="

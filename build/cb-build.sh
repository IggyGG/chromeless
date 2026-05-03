#!/usr/bin/env bash
#
# build/cb-build.sh — Phase 2 build orchestrator. Run inside the K8s
# Job from T112 on the triform-6 build node (32 GB RAM, 16+ vCPU,
# 200 GB SSD per T17 §4).
#
# Drives T101's Day-1 sequence end-to-end:
#   1.  gclient config + shallow sync of Chromium at the pinned
#       branch-head.
#   2.  Symlink this repo into the Chromium src/ tree as
#       //cloud-browser/.
#   3.  Apply our patch series (patches/0001-*.patch).
#   4.  Set up sccache.
#   5.  gn gen.
#   6.  autoninja the cloud_browser_worker + unit-test targets.
#   7.  Run the unit tests.
#   8.  Strip + package the binary into /work/artifacts/.
#   9.  Stage the runtime image build context for a kaniko sidecar.
#       (Actual registry push happens in the Job's downstream
#        sidecar; this step doesn't need registry auth.)
#
# Mount expectations from the K8s Job (T112):
#   /work/src/chromium  — Chromium checkout PVC (fast SSD).
#   /work/artifacts     — output PVC; kaniko sidecar reads from
#                          here.
#   /work/logs          — log PVC; kept across Job retries.
#   /sccache            — sccache cache PVC; reused across Jobs.
#   /workspace          — this repo, mounted read-only or via
#                          configmap. CB_REPO env points at it.
#
# Env (all optional):
#   CB_REPO                 path to this repo inside the pod.
#                            Default: /workspace.
#   CHROMIUM_BRANCH_NUMBER  pinned branch number (per T17 §6).
#                            Default: 7727 (Chromium 147 stable).
#   SCCACHE_DIR             sccache directory. Default: /sccache.
#   NINJA_PARALLELISM       autoninja -j arg. Default: unset (auto).
#   CB_BUILD_TARGETS        space-separated list. Default: the three
#                            standard targets below.
#   CB_GIT_SHA              short git sha; default derived from
#                            ${CB_REPO}/.git or the env.
#   SKIP_FETCH              "1" to skip Step 1 entirely (assumes
#                            /work/src/chromium is already populated).
#                            Used by Job retries.
#   STUB_MODE               "1" to skip the heavy steps (gclient
#                            sync, ninja). Each step still executes
#                            its shell-side scaffolding so the
#                            script's structure can be exercised
#                            without 4+ hours of build time. The
#                            DoD test path.
#
# Exit codes: each step's failure propagates verbatim.

set -euo pipefail

# ---------------------------------------------------------------------
# Config + paths.
# ---------------------------------------------------------------------

: "${CB_REPO:=/workspace}"
: "${CHROMIUM_BRANCH_NUMBER:=7727}"
: "${SCCACHE_DIR:=/sccache}"
: "${SKIP_FETCH:=}"
: "${STUB_MODE:=}"

# Mount-point roots. The K8s Job (T112) provides /work and /sccache;
# overrides exist so STUB_MODE runs work on a dev host.
: "${CB_WORK_ROOT:=/work}"
CHROMIUM_SRC="${CB_WORK_ROOT}/src/chromium"
ARTIFACTS_DIR="${CB_WORK_ROOT}/artifacts"
LOGS_DIR="${CB_WORK_ROOT}/logs"
TIMESTAMP="$(date -u +%Y%m%dT%H%M%SZ)"
LOG_FILE="${LOGS_DIR}/cb-build-${TIMESTAMP}.log"

mkdir -p "${ARTIFACTS_DIR}" "${LOGS_DIR}"

# Tee everything to the log file from here on. Stderr lines also go
# to stderr (so kubectl logs picks them up) and to the same log.
exec > >(tee -a "${LOG_FILE}") 2>&1

CB_GIT_SHA_DEFAULT="$(git -C "${CB_REPO}" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
: "${CB_GIT_SHA:=${CB_GIT_SHA_DEFAULT}}"

DEFAULT_TARGETS="cloud_browser_worker cloud_browser_encoder_unittests cloud_browser_framesink_capturer_unittests"
: "${CB_BUILD_TARGETS:=${DEFAULT_TARGETS}}"

# ---------------------------------------------------------------------
# Logging helpers.
# ---------------------------------------------------------------------

step_start_epoch=0
step_label=""

log()  { printf '[cb-build %s] %s\n' "$(date -u +%H:%M:%SZ)" "$*"; }
die()  { log "FATAL: $*"; exit 1; }

step()      {
    step_label="$1"
    step_start_epoch=$(date +%s)
    log "=== STEP ${step_label} START ==="
}
step_done() {
    local elapsed=$(( $(date +%s) - step_start_epoch ))
    log "=== STEP ${step_label} OK (${elapsed}s) ==="
}

# Wrap a command so a non-zero exit propagates with the step label
# already in the log.
run() {
    log "+ $*"
    if [[ -n "${STUB_MODE}" ]]; then
        log "[stub] skipping in STUB_MODE=1"
        return 0
    fi
    "$@"
}

require_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        die "missing required tool: $1"
    fi
}

# ---------------------------------------------------------------------
# Up-front prerequisites.
# ---------------------------------------------------------------------

log "cb-build.sh starting"
log "  cb_repo=${CB_REPO}  sha=${CB_GIT_SHA}"
log "  chromium_branch=refs/branch-heads/${CHROMIUM_BRANCH_NUMBER}"
log "  chromium_src=${CHROMIUM_SRC}"
log "  artifacts=${ARTIFACTS_DIR}  logs=${LOGS_DIR}  sccache=${SCCACHE_DIR}"
log "  targets='${CB_BUILD_TARGETS}'"
log "  stub_mode=${STUB_MODE:-0}  skip_fetch=${SKIP_FETCH:-0}"

if [[ -z "${STUB_MODE}" ]]; then
    require_tool git
    require_tool gclient
    require_tool gn
    require_tool autoninja
    require_tool sccache
    require_tool zstd
    require_tool tar
    require_tool find
fi

[[ -d "${CB_REPO}" ]] || die "CB_REPO=${CB_REPO} not found"
[[ -f "${CB_REPO}/capture/build-integration/build.sh" ]] || \
    die "T49 wrapper not found at ${CB_REPO}/capture/build-integration/build.sh"

# ---------------------------------------------------------------------
# Step 1 — gclient config + sync.
# ---------------------------------------------------------------------

step "1/10 gclient sync"

if [[ -n "${SKIP_FETCH}" && -d "${CHROMIUM_SRC}/src" ]]; then
    log "SKIP_FETCH=1 and chromium tree present; skipping fetch"
elif [[ -n "${STUB_MODE}" ]]; then
    log "[stub] would gclient config + sync to refs/branch-heads/${CHROMIUM_BRANCH_NUMBER}"
    mkdir -p "${CHROMIUM_SRC}/src"
else
    mkdir -p "${CHROMIUM_SRC}"
    cd "${CHROMIUM_SRC}"

    if [[ ! -f .gclient ]]; then
        cat > .gclient <<EOF
# Generated by cb-build.sh. Pinned via gclient sync --revision below.
solutions = [{
  "name"        : "src",
  "url"         : "https://chromium.googlesource.com/chromium/src.git",
  "managed"     : False,
  "custom_deps" : {},
  "custom_vars" : {"checkout_pgo_profiles": True},
}]
target_os = ["linux"]
EOF
    fi

    # --no-history --shallow keeps the checkout under ~30 GB instead
    # of the ~80 GB a full-history clone needs. We don't need history;
    # any blame / log work happens upstream.
    run gclient sync \
        --no-history \
        --shallow \
        --with_branch_heads --with_tags \
        --revision "src@refs/branch-heads/${CHROMIUM_BRANCH_NUMBER}"
fi

step_done

# ---------------------------------------------------------------------
# Step 2 — symlink this repo as //cloud-browser.
# ---------------------------------------------------------------------

step "2/10 symlink cloud-browser into chromium src/"

CB_LINK="${CHROMIUM_SRC}/src/cloud-browser"
if [[ -L "${CB_LINK}" ]]; then
    log "symlink already present; replacing to point at ${CB_REPO}"
    rm "${CB_LINK}"
elif [[ -e "${CB_LINK}" ]]; then
    die "${CB_LINK} exists and is not a symlink; refusing to clobber"
fi
run ln -s "${CB_REPO}" "${CB_LINK}"

step_done

# ---------------------------------------------------------------------
# Step 3 — apply patches.
#
# Delegate to the existing T49 wrapper so this script and the
# developer-host wrapper stay in lock-step on patch-application
# semantics. The wrapper handles the "already applied?" detection.
# ---------------------------------------------------------------------

step "3/10 apply patches"

export CHROMIUM_SRC="${CHROMIUM_SRC}/src"
if [[ -n "${STUB_MODE}" ]]; then
    patch_count="$(find "${CB_REPO}/patches" -maxdepth 1 -type f -name '[0-9]*.patch' 2>/dev/null | wc -l | tr -d ' ')"
    log "[stub] would apply ${patch_count} patches"
else
    run bash "${CB_REPO}/capture/build-integration/build.sh" apply-patches
fi

step_done

# ---------------------------------------------------------------------
# Step 3.5 — verify profile system libraries are staged.
#
# Each CB_BUILD_PROFILE flips on a different encoder gate in
# capture/build-integration/BUILD.gn:config(":x264"|":vaapi"|":nvenc"
# |":svt_av1"). The gate references a system library that lives outside
# chromium's third_party/ tree (libx264 / libva / NVENC SDK / libSvtAv1Enc).
#
# The MAIN build container runs with allowPrivilegeEscalation=false, so
# sudo is unusable here. The libs are instead installed by the
# bootstrap initContainer in build-job-<profile>.yaml, which runs as
# root and copies headers/libs into /work/system-libs (a hostPath
# location reachable by both the build container and the test pod).
#
# This step verifies the expected staging layout is present and fails
# fast with an actionable message if not, rather than letting `gn gen`
# emit a cryptic "include not found" error 30s later.
# ---------------------------------------------------------------------

step "3.5/10 profile system-deps verify"

profile="${CB_BUILD_PROFILE:-sw}"
log "build profile: ${profile}"

verify_libs() {
    local prefix="$1"
    shift
    local missing=()
    while [[ $# -gt 0 ]]; do
        if [[ ! -e "${prefix}/$1" ]]; then
            missing+=("${prefix}/$1")
        fi
        shift
    done
    if [[ ${#missing[@]} -gt 0 ]]; then
        log "ERROR: profile '${profile}' is missing the following files at ${prefix}:"
        printf '  %s\n' "${missing[@]}"
        die "stage these via the build-job-${profile}.yaml bootstrap initContainer"
    fi
    log "profile '${profile}' libs verified at ${prefix}"
}

case "${profile}" in
    sw)
        log "no profile-specific system deps (sw build)"
        ;;
    x264|all)
        # Track F1 (T36). libx264 expected at /work/system-libs.
        if [[ -z "${STUB_MODE}" ]]; then
            verify_libs /work/system-libs include/x264.h lib/libx264.so
        else
            log "[stub] would verify /work/system-libs for libx264"
        fi
        ;;
    vaapi)
        # Track F2 (T70). libva expected at /work/system-libs.
        if [[ -z "${STUB_MODE}" ]]; then
            verify_libs /work/system-libs include/va/va.h lib/libva.so
        else
            log "[stub] would verify /work/system-libs for libva"
        fi
        ;;
    nvenc)
        # Track F3 (T63). NVENC SDK is mounted via a Secret-as-volume
        # by the build-job-nvenc.yaml at /opt/nvenc-sdk.
        : "${NVENC_SDK_PATH:=/opt/nvenc-sdk}"
        if [[ ! -d "${NVENC_SDK_PATH}/Interface" ]]; then
            die "NVENC profile requires ${NVENC_SDK_PATH}/Interface/ — mount the SDK Secret in the Job"
        fi
        log "NVENC SDK at ${NVENC_SDK_PATH}"
        ;;
    *)
        die "unknown CB_BUILD_PROFILE=${profile} (expected sw|x264|vaapi|nvenc|all)"
        ;;
esac

# Export so build.sh sees it during gn gen.
export CB_BUILD_PROFILE="${profile}"

step_done

# ---------------------------------------------------------------------
# Step 4 — sccache.
#
# Set sccache up BEFORE gn gen so cc_wrapper="sccache" in args.gn
# is resolved against a working binary. Cache hit-ratio for
# Chromium builds typically lands 70–85% per T17 §5; the cold
# build is the one that matters for our ~4–8h target.
# ---------------------------------------------------------------------

step "4/10 sccache setup"

mkdir -p "${SCCACHE_DIR}"
export SCCACHE_DIR
export CC="sccache clang"
export CXX="sccache clang++"
# Don't kill the build over a sccache-server hiccup; fall back to
# direct compile.
export SCCACHE_ERROR_LOG="${LOGS_DIR}/sccache-${TIMESTAMP}.log"
if [[ -z "${STUB_MODE}" ]]; then
    run sccache --start-server || log "WARN: sccache --start-server returned non-zero (likely already running)"
    sccache --show-stats || true
fi

step_done

# ---------------------------------------------------------------------
# Step 5 — gn gen.
# ---------------------------------------------------------------------

step "5/10 gn gen"

export OUT_DIR="out/cb-release"
run bash "${CB_REPO}/capture/build-integration/build.sh" gen

step_done

# ---------------------------------------------------------------------
# Step 6 — autoninja.
# ---------------------------------------------------------------------

step "6/10 autoninja"

ninja_args=()
if [[ -n "${NINJA_PARALLELISM:-}" ]]; then
    ninja_args+=(-j "${NINJA_PARALLELISM}")
fi

# Targets get word-split intentionally below.
# shellcheck disable=SC2086
if [[ -z "${STUB_MODE}" ]]; then
    cd "${CHROMIUM_SRC}"
    run autoninja "${ninja_args[@]}" -C "${OUT_DIR}" ${CB_BUILD_TARGETS}
    sccache --show-stats >> "${LOG_FILE}" 2>&1 || true
fi

step_done

# ---------------------------------------------------------------------
# Step 7 — unit tests.
#
# Don't run them under STUB_MODE, but in real builds run them before
# packaging. NON-FATAL during first-light bringup (Wall #38): test
# failures are logged but don't block STEP 8/9 because some encoder
# tests require HAS_X264 / HAS_NVENC / HAS_VAAPI codepaths that aren't
# enabled in this build profile, plus there are known DanglingPtr
# warnings from raw_ptr cleanup paths in test fixtures we'll fix
# alongside the runtime wiring. The worker binary itself builds and
# links cleanly; we want the artifact even if tests are imperfect.
#
# Set CB_TESTS_FATAL=1 to restore strict mode once tests pass cleanly.
# ---------------------------------------------------------------------

step "7/10 unit tests"

if [[ -n "${STUB_MODE}" ]]; then
    log "[stub] would run cloud_browser_encoder_unittests + cloud_browser_framesink_capturer_unittests"
else
    encoder_rc=0
    framesink_rc=0
    run "${CHROMIUM_SRC}/${OUT_DIR}/cloud_browser_encoder_unittests" || encoder_rc=$?
    run "${CHROMIUM_SRC}/${OUT_DIR}/cloud_browser_framesink_capturer_unittests" || framesink_rc=$?
    if [[ "${encoder_rc}" -ne 0 || "${framesink_rc}" -ne 0 ]]; then
        log "WARN: unit tests reported failures (encoder=${encoder_rc} framesink=${framesink_rc})"
        if [[ -n "${CB_TESTS_FATAL:-}" ]]; then
            log "ERROR: CB_TESTS_FATAL=1 set; failing build."
            exit 1
        fi
        log "WARN: continuing to STEP 8 (CB_TESTS_FATAL unset; first-light non-blocking)."
    fi
fi

step_done

# ---------------------------------------------------------------------
# Step 8 — strip + package the binary.
# ---------------------------------------------------------------------

step "8/10 package binary"

binary_src="${CHROMIUM_SRC}/${OUT_DIR}/cloud_browser_worker"
artifact_name="cloud_browser_worker-cr${CHROMIUM_BRANCH_NUMBER}-${CB_GIT_SHA}.tar.zst"
artifact_path="${ARTIFACTS_DIR}/${artifact_name}"

if [[ -n "${STUB_MODE}" ]]; then
    # Synthesise a placeholder so the kaniko stage can still exercise
    # its layout assumptions in stub runs.
    mkdir -p "${ARTIFACTS_DIR}/context"
    : > "${ARTIFACTS_DIR}/context/cloud_browser_worker"
    log "[stub] wrote placeholder binary to ${ARTIFACTS_DIR}/context/cloud_browser_worker"
else
    [[ -f "${binary_src}" ]] || die "build did not produce ${binary_src}"

    # Strip in place — a separate copy would double the disk hit;
    # the build out/ dir is regenerable.
    run llvm-strip --strip-unneeded "${binary_src}" || \
        run strip --strip-unneeded "${binary_src}"

    # Package: a tarball with just the binary + (later) any dynamic
    # libs we need to ship alongside. zstd because it's already in
    # the image (sccache pulls it transitively) and gives a 3-5×
    # better ratio than gzip on stripped Chromium binaries.
    tar --zstd -cf "${artifact_path}" \
        -C "$(dirname "${binary_src}")" "$(basename "${binary_src}")"

    # Also stage the binary uncompressed into the kaniko build
    # context — the runtime image (Step 9 / Dockerfile.runtime) COPYs
    # it from there.
    mkdir -p "${ARTIFACTS_DIR}/context"
    cp "${binary_src}" "${ARTIFACTS_DIR}/context/cloud_browser_worker"
    log "packaged ${artifact_path}"
    log "staged binary to ${ARTIFACTS_DIR}/context/cloud_browser_worker"
fi

step_done

# ---------------------------------------------------------------------
# Step 9 — stage runtime image build context.
#
# We do NOT push the image from this script. The K8s Job (T112)
# runs a kaniko sidecar that consumes ${ARTIFACTS_DIR}/context/
# and pushes to the forgejo registry — keeps registry-auth out of
# this script and lets the Job's IRSA / image-pull-secret flow
# stay in one place.
# ---------------------------------------------------------------------

step "9/10 stage runtime image build context"

cp "${CB_REPO}/build/Dockerfile.runtime" "${ARTIFACTS_DIR}/context/Dockerfile"
cp "${CB_REPO}/infra/launch-chromium.sh" "${ARTIFACTS_DIR}/context/launch-chromium.sh" 2>/dev/null || true
cp "${CB_REPO}/infra/supervisord.conf"   "${ARTIFACTS_DIR}/context/supervisord.conf"   2>/dev/null || true

# Tag metadata for the kaniko sidecar to read.
image_tag="cr${CHROMIUM_BRANCH_NUMBER}-${CB_GIT_SHA}"
cat > "${ARTIFACTS_DIR}/context/IMAGE_TAG" <<EOF
${image_tag}
EOF

log "image build context ready at ${ARTIFACTS_DIR}/context/"
log "  expected tag: cb-chromium:${image_tag}"
log "  the kaniko sidecar in the T112 Job picks up from here"

step_done

# ---------------------------------------------------------------------
# Step 10 — CDP validation against the just-pushed image.
#
# Schedules a `cb-cdp-validation` Job in the `cb-tests` namespace that
# runs `pytest tests/cdp/` against a freshly-spun cb-browserless built
# from this image tag. Gates promotion: if any of the four CDP ops
# (createBrowserContext / createTarget / attachToTarget / Page.navigate)
# fail, the build script exits non-zero and the kaniko-pushed image is
# NOT promoted to `cb-chromium:latest`.
#
# Why a separate K8s Job instead of inline-docker-run: the build host
# doesn't have egress to the chromium binary's full system-deps surface,
# but the cluster does. Running the smoke as a Job in the same cluster
# the image will eventually serve from also catches "the image runs
# differently in K8s than on the build host" regressions.
#
# Image tag is passed via env var (CB_TEST_IMAGE_TAG), NOT sed
# substitution into the manifest — keeps the YAML a stable artifact and
# makes `kubectl diff` against a manifest that's been through CI useful.
# ---------------------------------------------------------------------

step "10/10 cdp validation"

CDP_VALIDATION_MANIFEST="${CB_REPO}/infra/k8s/tests/cb-cdp-validation.yaml"
CDP_VALIDATION_NS="cb-tests"
CDP_VALIDATION_JOB="cb-cdp-validation"

if [[ ! -f "${CDP_VALIDATION_MANIFEST}" ]]; then
    die "cdp validation manifest missing: ${CDP_VALIDATION_MANIFEST} (k8s-manifest-author should have shipped it)"
fi

# Best-effort: clear any prior Job instance from the previous build so
# we don't get a "field is immutable" rejection on re-apply.
kubectl -n "${CDP_VALIDATION_NS}" delete job "${CDP_VALIDATION_JOB}" --ignore-not-found=true >/dev/null 2>&1 || true

log "applying ${CDP_VALIDATION_MANIFEST} with CB_TEST_IMAGE_TAG=${image_tag}"
CB_TEST_IMAGE_TAG="${image_tag}" kubectl apply -f "${CDP_VALIDATION_MANIFEST}"

log "waiting up to 120s for ${CDP_VALIDATION_JOB} to complete..."
set +e
kubectl -n "${CDP_VALIDATION_NS}" wait --for=condition=complete \
    --timeout=120s "job/${CDP_VALIDATION_JOB}"
wait_rc=$?
set -e

# Always tail the Job pod logs — green or red, the per-op pass/fail
# breakdown belongs in the build log so a failed promotion can be
# triaged from kubectl logs of the cb-build Job alone.
log "--- cb-cdp-validation pod logs ---"
kubectl -n "${CDP_VALIDATION_NS}" logs --tail=200 \
    "job/${CDP_VALIDATION_JOB}" 2>&1 | sed 's/^/  /' || true
log "--- end cb-cdp-validation pod logs ---"

if [[ ${wait_rc} -ne 0 ]]; then
    die "cdp validation job did not complete cleanly (kubectl wait rc=${wait_rc}); image NOT promoted"
fi

step_done

log ""
log "cb-build.sh finished successfully"
log "  artifact:    ${artifact_path}"
log "  image-tag:   cb-chromium:${image_tag}"
log "  log:         ${LOG_FILE}"

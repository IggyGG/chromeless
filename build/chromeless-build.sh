#!/usr/bin/env bash
#
# build/chromeless-build.sh — Phase 2 build orchestrator. Run inside the K8s
# Job from T112 on the triform-8 build node (128 GiB RAM, 48 vCPU,
# ~937 GiB free on /var/lib/longhorn; T17 §4 spec'd 32 GB / 16 vCPU
# / 200 GB SSD and we exceed all three).
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
#                          configmap. CHROMELESS_REPO env points at it.
#
# Env (all optional):
#   CHROMELESS_REPO                 path to this repo inside the pod.
#                            Default: /workspace.
#   CHROMIUM_BRANCH_NUMBER  pinned branch number (per T17 §6).
#                            Default: 7727 (Chromium 147 stable).
#   SCCACHE_DIR             sccache directory. Default: /sccache.
#   NINJA_PARALLELISM       autoninja -j arg. Default: unset (auto).
#   CHROMELESS_BUILD_TARGETS        space-separated list. Default: the three
#                            standard code targets plus the headless
#                            resource packs the runtime image loads.
#   CHROMELESS_GIT_SHA              short git sha; default derived from
#                            ${CHROMELESS_REPO}/.git or the env.
#   CHROMELESS_TESTS_NONFATAL       "1" to ship the artifact even when the
#                            Step 7 unit tests fail. Off by default —
#                            failing tests fail the build. Only set this
#                            on a lane whose build profile genuinely lacks
#                            the codepaths some tests need, and record why.
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

: "${CHROMELESS_REPO:=/workspace}"
: "${CHROMIUM_BRANCH_NUMBER:=7727}"
# build-czar 2026-05-17: default SCCACHE_DIR to /work/sccache, not the
# legacy /sccache hostPath mount. Commit 73e37e7 (S3 sccache backend)
# removed the per-node /sccache hostPath volume + volumeMount + the
# SCCACHE_DIR env that pinned it. STEP 4's `mkdir -p ${SCCACHE_DIR}`
# then tried to create /sccache on the read-only container root →
# `Permission denied` → STEP 4 abort → OnFailure restart (Build #24
# vp5rw restart=1). In S3 mode sccache's real cache is the Hetzner
# bucket; SCCACHE_DIR is only the local daemon scratch/socket dir, so
# any writable path works. /work is the always-mounted, uid-1000-owned
# workdir hostPath — safe for both S3 mode and any future local mode.
# An explicit SCCACHE_DIR env (if ever re-added to the manifest) still
# overrides this default.
#
# build-czar 2026-05-17 iter9: /work/sccache also fails — the workdir
# hostPath root (/work) is owned by root; the init container only
# `chown 1000:1000`s the specific subdirs /work/src|/work/artifacts|
# /work/logs (Phase 5), NOT /work itself, so the uid-1000 build
# container cannot mkdir a fresh /work/sccache under it (Build #25
# vhgtc STEP 4: mkdir /work/sccache Permission denied, restart=1).
# /tmp is always world-writable in the container and sccache's local
# dir in S3 mode is pure ephemeral daemon scratch (real cache = the
# Hetzner bucket, persistence not needed), so /tmp/sccache is the
# robust choice — no dependency on init-container chown coverage.
: "${SCCACHE_DIR:=/tmp/sccache}"
: "${SKIP_FETCH:=}"
: "${STUB_MODE:=}"

# Mount-point roots. The K8s Job (T112) provides /work and /sccache;
# overrides exist so STUB_MODE runs work on a dev host.
: "${CHROMELESS_WORK_ROOT:=/work}"
CHROMIUM_SRC="${CHROMELESS_WORK_ROOT}/src/chromium"
ARTIFACTS_DIR="${CHROMELESS_WORK_ROOT}/artifacts"
LOGS_DIR="${CHROMELESS_WORK_ROOT}/logs"
TIMESTAMP="$(date -u +%Y%m%dT%H%M%SZ)"
LOG_FILE="${LOGS_DIR}/chromeless-build-${TIMESTAMP}.log"

mkdir -p "${ARTIFACTS_DIR}" "${LOGS_DIR}"

# Tee everything to the log file from here on. Stderr lines also go
# to stderr (so kubectl logs picks them up) and to the same log.
exec > >(tee -a "${LOG_FILE}") 2>&1

# CHROMELESS_GIT_SHA — short SHA of the chromeless checkout; tags the
# build artifact and the runtime image (cr<branch>-<sha>).
#
# CV2-79 (IMAGE_TAG=unknown — recurrence #4 fix): the bootstrap init
# container clones ${CHROMELESS_REPO} (/workspace) as root, but this
# script runs as uid 1000. A bare `git -C "${CHROMELESS_REPO}"
# rev-parse` then trips git's "detected dubious ownership" guard
# (CVE-2022-24765 hardening); the error was swallowed by 2>/dev/null
# and the `|| echo unknown` fallback silently produced cr7727-unknown
# for four builds running. `.git` IS present — only the uid mismatch
# between the root clone and the uid-1000 reader is wrong.
#
# Fix: resolve inside a subshell whose GIT_CONFIG_GLOBAL points at a
# throwaway config that whitelists the checkout via safe.directory.
# safe.directory is honored ONLY from system/global config — never
# from `-c` or a repo-local config — so a scratch global config is the
# one surgical lever. The subshell confines the override so it cannot
# leak into the chromium patch-apply / depot_tools git calls later in
# this build.
CHROMELESS_GIT_SHA_DEFAULT="$(
    _cb_gitcfg="$(mktemp "${TMPDIR:-/tmp}/cb-gitconfig.XXXXXX" 2>/dev/null || echo "/tmp/cb-gitconfig.$$")"
    export GIT_CONFIG_GLOBAL="${_cb_gitcfg}"
    git config --global --add safe.directory "${CHROMELESS_REPO}" >/dev/null 2>&1 || true
    _cb_sha="$(git -C "${CHROMELESS_REPO}" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
    rm -f "${_cb_gitcfg}"
    printf '%s' "${_cb_sha}"
)"
: "${CHROMELESS_GIT_SHA:=${CHROMELESS_GIT_SHA_DEFAULT}}"

DEFAULT_TARGETS="cloud_browser_worker cloud_browser_encoder_unittests cloud_browser_framesink_capturer_unittests headless:resource_pack_data headless:resource_pack_strings"
: "${CHROMELESS_BUILD_TARGETS:=${DEFAULT_TARGETS}}"

# ---------------------------------------------------------------------
# Logging helpers.
# ---------------------------------------------------------------------

step_start_epoch=0
step_label=""

log()  { printf '[chromeless-build %s] %s\n' "$(date -u +%H:%M:%SZ)" "$*"; }
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

log "chromeless-build.sh starting"
log "  cb_repo=${CHROMELESS_REPO}  sha=${CHROMELESS_GIT_SHA}"
log "  chromium_branch=refs/branch-heads/${CHROMIUM_BRANCH_NUMBER}"
log "  chromium_src=${CHROMIUM_SRC}"
log "  artifacts=${ARTIFACTS_DIR}  logs=${LOGS_DIR}  sccache=${SCCACHE_DIR}"
log "  targets='${CHROMELESS_BUILD_TARGETS}'"
log "  stub_mode=${STUB_MODE:-0}  skip_fetch=${SKIP_FETCH:-0}"

# CV2-79 loud guard: an unresolved SHA yields a non-traceable
# cr<branch>-unknown tag that also collides with any other failed-
# derivation build. This class recurred four times because the
# fallback was silent — surface it loudly so a 5th recurrence is
# caught here, at build time, instead of in post-hoc triage.
if [[ "${CHROMELESS_GIT_SHA}" == "unknown" ]]; then
    log "WARN: CHROMELESS_GIT_SHA unresolved from ${CHROMELESS_REPO} — the image"
    log "WARN:   and artifact will tag cr${CHROMIUM_BRANCH_NUMBER}-unknown (not"
    log "WARN:   traceable, collision-prone). Pass CHROMELESS_GIT_SHA explicitly,"
    log "WARN:   or check the bootstrap clone / checkout ownership (CV2-79)."
fi

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

[[ -d "${CHROMELESS_REPO}" ]] || die "CHROMELESS_REPO=${CHROMELESS_REPO} not found"
[[ -f "${CHROMELESS_REPO}/capture/build-integration/build.sh" ]] || \
    die "T49 wrapper not found at ${CHROMELESS_REPO}/capture/build-integration/build.sh"

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
# Generated by chromeless-build.sh. Pinned via gclient sync --revision below.
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

CHROMELESS_LINK="${CHROMIUM_SRC}/src/cloud-browser"
if [[ -L "${CHROMELESS_LINK}" ]]; then
    log "symlink already present; replacing to point at ${CHROMELESS_REPO}"
    rm "${CHROMELESS_LINK}"
elif [[ -e "${CHROMELESS_LINK}" ]]; then
    die "${CHROMELESS_LINK} exists and is not a symlink; refusing to clobber"
fi
run ln -s "${CHROMELESS_REPO}" "${CHROMELESS_LINK}"

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
    patch_count="$(find "${CHROMELESS_REPO}/patches" -maxdepth 1 -type f -name '[0-9]*.patch' 2>/dev/null | wc -l | tr -d ' ')"
    log "[stub] would apply ${patch_count} patches"
else
    run bash "${CHROMELESS_REPO}/capture/build-integration/build.sh" apply-patches
fi

step_done

# ---------------------------------------------------------------------
# Step 3.5 — verify profile system libraries are staged.
#
# Each CHROMELESS_BUILD_PROFILE flips on a different encoder gate in
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

profile="${CHROMELESS_BUILD_PROFILE:-sw}"
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
        die "unknown CHROMELESS_BUILD_PROFILE=${profile} (expected sw|x264|vaapi|nvenc|all)"
        ;;
esac

# Export so build.sh sees it during gn gen.
export CHROMELESS_BUILD_PROFILE="${profile}"

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
run bash "${CHROMELESS_REPO}/capture/build-integration/build.sh" gen

step_done

# ---------------------------------------------------------------------
# Step 6 — autoninja.
# ---------------------------------------------------------------------

step "6/10 autoninja"

ninja_args=()
if [[ -n "${NINJA_PARALLELISM:-}" ]]; then
    ninja_args+=(-j "${NINJA_PARALLELISM}")
fi
# NINJA_KEEP_GOING — ninja -k arg. When set, ninja continues past
# failing edges instead of fast-failing on the first error, so a
# single build pass collects EVERY failing TU. "0" = unlimited
# (keep going regardless of failure count). Default: unset (ninja's
# default fast-fail). Used by the build-czar nuclear cold-rebuild
# diagnostic to surface the full chromium-API-drift surface in one
# pass rather than iter-by-iter.
if [[ -n "${NINJA_KEEP_GOING:-}" ]]; then
    ninja_args+=(-k "${NINJA_KEEP_GOING}")
fi

# Targets get word-split intentionally below.
# shellcheck disable=SC2086
if [[ -z "${STUB_MODE}" ]]; then
    cd "${CHROMIUM_SRC}"
    run autoninja "${ninja_args[@]}" -C "${OUT_DIR}" ${CHROMELESS_BUILD_TARGETS}
    sccache --show-stats >> "${LOG_FILE}" 2>&1 || true
fi

step_done

# ---------------------------------------------------------------------
# Step 7 — unit tests.
#
# Don't run them under STUB_MODE, but in real builds run them before
# packaging.
#
# OSS-W2: these are now FATAL BY DEFAULT. They were non-fatal during
# first-light bringup (Wall #38) for a real reason — some encoder tests
# require HAS_X264 / HAS_NVENC / HAS_VAAPI codepaths not enabled in every
# build profile, and there were known DanglingPtr warnings from raw_ptr
# cleanup in test fixtures. But "temporarily non-blocking" became
# permanent, and the effect is that the ONLY place C++ tests run at all
# (they need a Chromium tree, so `make verify` cannot touch them) reported
# failures as `WARN:` and shipped the artifact anyway. A test suite whose
# failures never block is not a test suite.
#
# The escape hatch is now explicit and named after its actual reason:
#   CHROMELESS_TESTS_NONFATAL=1   log failures, continue, ship anyway.
# Use it for a bringup lane on a profile with known-unsupported codepaths,
# and say so in the job that sets it. Everything else should fail loudly.
#
# The old CHROMELESS_TESTS_FATAL=1 opt-IN is gone: strict is the default
# now, so it had no meaning. Nothing in the repo set it (grep is clean),
# which is precisely why the tests never blocked anything.
# ---------------------------------------------------------------------

step "7/10 unit tests"

if [[ -n "${STUB_MODE}" ]]; then
    log "[stub] would run every *_unittests target in CHROMELESS_BUILD_TARGETS"
else
    # Profile builds dynamic-link system libs staged at /work/system-libs
    # (BUILD.gn:x264 with -Wl,--allow-shlib-undefined and NO rpath — the
    # runtime image provides the libs on its default path). The build
    # host's loader knows nothing about that directory, so the encoder
    # test binary dies at exec with "error while loading shared
    # libraries: libx264.so.164" before gtest even starts (exit 127) —
    # first hit 2026-07-30, the first profile=x264 build to reach STEP 7
    # after the profile-pipe fix. Export rather than per-command prefix
    # so both binaries and any future test target get it; harmless for
    # sw-profile builds (the dir just isn't consulted).
    export LD_LIBRARY_PATH="/work/system-libs/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

    # Run every *_unittests target that was BUILT, derived from
    # CHROMELESS_BUILD_TARGETS rather than hardcoded.
    #
    # This used to name two binaries literally. That is how
    # cb_wire_envelope_unittests came to be built-but-never-run: adding a
    # target to the lane's target list did nothing here, and the mismatch is
    # invisible — the build goes green either way. Same failure SHAPE as the
    # silent-skip this step's fatal-on-missing check was added to close, one
    # level up: there the binary was missing, here it exists and nobody
    # invokes it.
    #
    # Deriving the list means a target added to CHROMELESS_BUILD_TARGETS is
    # automatically executed, and STEP 7's existing fatal-on-missing check
    # still fires if it failed to link.
    # NOTE: no `local` here. This block runs at SCRIPT TOP LEVEL, not inside a
    # function, and bash makes `local` a fatal error there — under `set -e`
    # that aborts the step instantly with no output, which is exactly how the
    # first run of this rewrite died (STEP 7 START, then nothing). Caught
    # 2026-07-30 by the build itself; the shellcheck-style lint added
    # alongside this now catches it before a 12-minute compile does.
    test_binaries=()
    for _t in ${CHROMELESS_BUILD_TARGETS}; do
        # gn labels look like path/to:target_name — take the target name.
        _name="${_t##*:}"
        case "${_name}" in
            *_unittests) test_binaries+=("${_name}") ;;
        esac
    done

    tests_rc=0
    if [[ "${#test_binaries[@]}" -eq 0 ]]; then
        log "WARN: no *_unittests targets in CHROMELESS_BUILD_TARGETS — nothing to run."
        log "WARN: this lane is shipping an artifact no test has exercised."
    fi
    for _bin in "${test_binaries[@]}"; do
        _path="${CHROMIUM_SRC}/${OUT_DIR}/${_bin}"
        if [[ ! -x "${_path}" ]]; then
            log "ERROR: ${_bin} was requested but is missing or not executable."
            log "ERROR: it is in CHROMELESS_BUILD_TARGETS, so ninja should have"
            log "ERROR: produced it — treat this as a build failure, not a skip."
            tests_rc=1
            continue
        fi
        run "${_path}" || tests_rc=$?
    done

    if [[ "${tests_rc}" -ne 0 ]]; then
        log "unit tests reported failures (rc=${tests_rc}); ran: ${test_binaries[*]}"
        if [[ -n "${CHROMELESS_TESTS_NONFATAL:-}" ]]; then
            log "WARN: CHROMELESS_TESTS_NONFATAL=1 — continuing to STEP 8 and"
            log "WARN: shipping this artifact with failing unit tests."
        else
            log "ERROR: unit tests failed. Set CHROMELESS_TESTS_NONFATAL=1 to ship"
            log "ERROR: anyway (and record why in the job that sets it)."
            exit 1
        fi
    fi
fi

step_done

# ---------------------------------------------------------------------
# Step 8 — strip + package the binary.
# ---------------------------------------------------------------------

step "8/10 package binary"

binary_src="${CHROMIUM_SRC}/${OUT_DIR}/cloud_browser_worker"
artifact_name="cloud_browser_worker-cr${CHROMIUM_BRANCH_NUMBER}-${CHROMELESS_GIT_SHA}.tar.zst"
artifact_path="${ARTIFACTS_DIR}/${artifact_name}"

if [[ -n "${STUB_MODE}" ]]; then
    # Synthesise a placeholder so the kaniko stage can still exercise
    # its layout assumptions in stub runs.
    mkdir -p "${ARTIFACTS_DIR}/context"
    : > "${ARTIFACTS_DIR}/context/cloud_browser_worker"
    for runtime_asset in icudtl.dat headless_lib_data.pak headless_lib_strings.pak libEGL.so libGLESv2.so libvk_swiftshader.so libvulkan.so.1; do
        : > "${ARTIFACTS_DIR}/context/${runtime_asset}"
    done
    # CV2-89: stub the SwANGLE Vulkan ICD descriptor JSON. Real path
    # below sed-rewrites the chromium-generated JSON's library_path to
    # absolute; stub just creates the empty placeholder for kaniko
    # context-layout testing.
    : > "${ARTIFACTS_DIR}/context/vk_swiftshader_icd.json"
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
    for runtime_asset in icudtl.dat headless_lib_data.pak headless_lib_strings.pak libEGL.so libGLESv2.so libvk_swiftshader.so libvulkan.so.1; do
        asset_src="${CHROMIUM_SRC}/${OUT_DIR}/${runtime_asset}"
        [[ -f "${asset_src}" ]] || die "runtime asset missing: ${asset_src}"
        cp "${asset_src}" "${ARTIFACTS_DIR}/context/${runtime_asset}"
    done

    # CV2-89: stage the SwANGLE Vulkan ICD descriptor JSON.
    #
    # Why this exists: chromium's build emits vk_swiftshader_icd.json
    # alongside libvk_swiftshader.so in ${OUT_DIR}. The implementation
    # lib is the Vulkan driver; the JSON is the Vulkan loader's
    # registration descriptor (it tells libvulkan.so.1 "here is a
    # driver, here is its library_path"). Without a *reachable* JSON,
    # vkCreateInstance returns VK_ERROR_INITIALIZATION_FAILED
    # ("Internal Vulkan error (-3)"), which propagates as
    # `eglInitialize SwANGLE failed with error EGL_NOT_INITIALIZED`
    # in chromium's GPU process, killing the renderer.
    #
    # rv8: Dockerfile.runtime installs this JSON at
    # /usr/local/bin/vk_swiftshader_icd.json — co-located with the
    # ANGLE libs — because chromium's SwANGLE path self-sets
    # VK_ICD_FILENAMES to <ANGLE module dir>/vk_swiftshader_icd.json,
    # and that override makes the Vulkan loader skip the generic
    # /usr/share/vulkan/icd.d/ scan. See the rv8 comment block in
    # Dockerfile.runtime for the full CV2-89 ring-N+1 root cause.
    #
    # Why the sed: the upstream JSON has a relative
    # `"library_path": "./libvk_swiftshader.so"`. The sed rewrites it
    # to the absolute /usr/local/bin/libvk_swiftshader.so so the path
    # is unambiguous regardless of the JSON's own location (the lib
    # and JSON are in fact co-located at /usr/local/bin/, so a
    # relative path would also resolve — absolute is kept for
    # robustness and because the grep sanity-check below keys on it).
    #
    # Methodology event: 9th instance of lesson-(i) (previously-
    # untested code path FATALs when first exercised) + new
    # sub-lesson (g.3) "Completeness-shape" (partial-copy gap
    # rather than literal drift). Pre-CV2-78 Wave 1, no test
    # harness exercised the renderer-DOM-bearing path, so the
    # missing-ICD-JSON never surfaced. CV2-89 closes that gap at
    # the image-packaging layer.
    icd_src="${CHROMIUM_SRC}/${OUT_DIR}/vk_swiftshader_icd.json"
    [[ -f "${icd_src}" ]] || die "runtime asset missing: ${icd_src} (CV2-89: chromium build did not emit SwANGLE ICD descriptor — check args.gn use_swiftshader_with_subzero / swiftshader_for_webgpu settings)"
    sed 's|"\./libvk_swiftshader\.so"|"/usr/local/bin/libvk_swiftshader.so"|' \
        "${icd_src}" > "${ARTIFACTS_DIR}/context/vk_swiftshader_icd.json"
    # Sanity check the sed actually applied — if upstream changes
    # the relative-path form (e.g. drops the ./, becomes "libvk_..."),
    # the sed becomes a silent no-op and we'd ship a broken JSON.
    # Fail loud rather than ship a non-functional image.
    grep -q '"/usr/local/bin/libvk_swiftshader.so"' \
        "${ARTIFACTS_DIR}/context/vk_swiftshader_icd.json" || \
        die "CV2-89: sed-rewrite of library_path did not apply — check upstream vk_swiftshader_icd.json format. Saw: $(grep library_path "${icd_src}" || echo '<no library_path line>')"

    log "packaged ${artifact_path}"
    log "staged binary to ${ARTIFACTS_DIR}/context/cloud_browser_worker"
    log "staged SwANGLE ICD descriptor with absolute library_path (CV2-89)"
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

cp "${CHROMELESS_REPO}/build/Dockerfile.runtime" "${ARTIFACTS_DIR}/context/Dockerfile"
cp "${CHROMELESS_REPO}/infra/launch-chromeless.sh" "${ARTIFACTS_DIR}/context/launch-chromeless.sh"
cp "${CHROMELESS_REPO}/infra/supervisord.phase2.conf" "${ARTIFACTS_DIR}/context/supervisord.conf"
cp "${CHROMELESS_REPO}/infra/pulse-default.pa" "${ARTIFACTS_DIR}/context/pulse-default.pa"
cp "${CHROMELESS_REPO}/infra/devtools-proxy.sh" "${ARTIFACTS_DIR}/context/devtools-proxy.sh"
rm -rf "${ARTIFACTS_DIR}/context/lifecycle"
cp -R "${CHROMELESS_REPO}/infra/lifecycle" "${ARTIFACTS_DIR}/context/lifecycle"

# Tag metadata for the kaniko sidecar to read.
image_tag="cr${CHROMIUM_BRANCH_NUMBER}-${CHROMELESS_GIT_SHA}"
cat > "${ARTIFACTS_DIR}/context/IMAGE_TAG" <<EOF
${image_tag}
EOF

log "image build context ready at ${ARTIFACTS_DIR}/context/"
log "  expected tag: chromeless:${image_tag}"
log "  the kaniko sidecar in the T112 Job picks up from here"

step_done

# ---------------------------------------------------------------------
# Step 10 — CDP validation against the just-pushed image.
#
# Schedules a `chromeless-cdp-validation` Job in the `chromeless-tests` namespace that
# runs `pytest tests/cdp/` against a freshly-spun chromeless-browserless built
# from this image tag. Gates promotion: if any of the four CDP ops
# (createBrowserContext / createTarget / attachToTarget / Page.navigate)
# fail, the build script exits non-zero and the kaniko-pushed image is
# NOT promoted to `chromeless:latest`.
#
# Why a separate K8s Job instead of inline-docker-run: the build host
# doesn't have egress to the chromium binary's full system-deps surface,
# but the cluster does. Running the smoke as a Job in the same cluster
# the image will eventually serve from also catches "the image runs
# differently in K8s than on the build host" regressions.
#
# Image tag is passed via env var (CHROMELESS_TEST_IMAGE_TAG), NOT sed
# substitution into the manifest — keeps the YAML a stable artifact and
# makes `kubectl diff` against a manifest that's been through CI useful.
# ---------------------------------------------------------------------

step "10/10 cdp validation"

if [[ -n "${STUB_MODE}" ]]; then
    log "[stub] skipping cluster CDP validation"
    step_done
    log ""
    log "chromeless-build.sh finished successfully"
    log "  artifact:    ${artifact_path}"
    log "  image-tag:   chromeless:${image_tag}"
    log "  log:         ${LOG_FILE}"
    exit 0
fi

# SKIP_CDP_VALIDATION=1 short-circuits only STEP 10 (unlike STUB_MODE
# which skips the earlier build steps). Use when the build pod's
# container image lacks kubectl: the default chromeless-build pods run
# from debian:bookworm-slim with no kubectl, so this step's
# `kubectl apply` fails with exit 127. Observed on build #1 and build
# #3 (2026-05-17): each consumed all 3 backoffLimit attempts at
# STEP 10 even though the binary was already successfully built,
# packaged, and staged at STEPS 6-9. SKIP_CDP_VALIDATION=1 lets the
# binary ship without round-tripping through CDP validation here;
# run validation separately from an env that DOES have kubectl, e.g.
# from a workspace pod via the same manifest path.
if [[ -n "${SKIP_CDP_VALIDATION:-}" ]]; then
    log "[SKIP_CDP_VALIDATION=1] skipping cluster CDP validation"
    log "  -> run validation separately via 'kubectl apply -f ${CHROMELESS_REPO:-?}/infra/k8s/tests/chromeless-cdp-validation.yaml' from an env with kubectl"
    step_done
    log ""
    log "chromeless-build.sh finished successfully"
    log "  artifact:    ${artifact_path}"
    log "  image-tag:   chromeless:${image_tag}"
    log "  log:         ${LOG_FILE}"
    exit 0
fi

CDP_VALIDATION_MANIFEST="${CHROMELESS_REPO}/infra/k8s/tests/chromeless-cdp-validation.yaml"
CDP_VALIDATION_NS="chromeless-tests"
CDP_VALIDATION_JOB="chromeless-cdp-validation"

if [[ ! -f "${CDP_VALIDATION_MANIFEST}" ]]; then
    die "cdp validation manifest missing: ${CDP_VALIDATION_MANIFEST} (k8s-manifest-author should have shipped it)"
fi

# Best-effort: clear any prior Job instance from the previous build so
# we don't get a "field is immutable" rejection on re-apply.
kubectl -n "${CDP_VALIDATION_NS}" delete job "${CDP_VALIDATION_JOB}" --ignore-not-found=true >/dev/null 2>&1 || true

log "applying ${CDP_VALIDATION_MANIFEST} with CHROMELESS_TEST_IMAGE_TAG=${image_tag}"
CHROMELESS_TEST_IMAGE_TAG="${image_tag}" kubectl apply -f "${CDP_VALIDATION_MANIFEST}"

log "waiting up to 120s for ${CDP_VALIDATION_JOB} to complete..."
set +e
kubectl -n "${CDP_VALIDATION_NS}" wait --for=condition=complete \
    --timeout=120s "job/${CDP_VALIDATION_JOB}"
wait_rc=$?
set -e

# Always tail the Job pod logs — green or red, the per-op pass/fail
# breakdown belongs in the build log so a failed promotion can be
# triaged from kubectl logs of the chromeless-build Job alone.
log "--- chromeless-cdp-validation pod logs ---"
kubectl -n "${CDP_VALIDATION_NS}" logs --tail=200 \
    "job/${CDP_VALIDATION_JOB}" 2>&1 | sed 's/^/  /' || true
log "--- end chromeless-cdp-validation pod logs ---"

if [[ ${wait_rc} -ne 0 ]]; then
    die "cdp validation job did not complete cleanly (kubectl wait rc=${wait_rc}); image NOT promoted"
fi

step_done

log ""
log "chromeless-build.sh finished successfully"
log "  artifact:    ${artifact_path}"
log "  image-tag:   chromeless:${image_tag}"
log "  log:         ${LOG_FILE}"

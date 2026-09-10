#!/usr/bin/env bash
#
# build.sh — convenience wrapper for the cloud-browser worker build.
#
# This script lives at //cloud-browser/capture/build-integration/build.sh
# inside a Chromium src/ checkout (see capture/build-integration/README.md).
#
# It does three things:
#   1. apply-patches : applies our Chromium patch series.
#   2. gen           : runs `gn gen` with our args.gn.
#   3. ninja         : runs `autoninja` for the chosen targets.
#
# `bash build.sh all` runs all three; individual subcommands are exposed
# so CI can split steps.
#
# TODO(T17-build-env): exercise this on a Linux box with depot_tools.

set -euo pipefail

# ---------------------------------------------------------------------
# Resolve paths.
# ---------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CB_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# CHROMIUM_SRC is the directory `gn gen` is invoked relative to. By
# convention this script is invoked from inside the Chromium src/ tree
# with this repo symlinked as //cloud-browser, so we walk up from the
# symlink to find src/.
CHROMIUM_SRC_DEFAULT="$(cd "${CB_ROOT}/.." && pwd)"
CHROMIUM_SRC="${CHROMIUM_SRC:-${CHROMIUM_SRC_DEFAULT}}"

OUT_DIR="${OUT_DIR:-out/cb-release}"
TARGETS_DEFAULT="cloud_browser_worker cloud_browser_encoder_unittests"
TARGETS="${TARGETS:-${TARGETS_DEFAULT}}"

# ---------------------------------------------------------------------
# Logging helpers.
# ---------------------------------------------------------------------

log() {
    printf '[build.sh] %s\n' "$*" >&2
}

die() {
    log "ERROR: $*"
    exit 1
}

# ---------------------------------------------------------------------
# Sanity checks.
# ---------------------------------------------------------------------

require_chromium_src() {
    if [[ ! -d "${CHROMIUM_SRC}/build" || ! -f "${CHROMIUM_SRC}/DEPS" ]]; then
        die "CHROMIUM_SRC=${CHROMIUM_SRC} doesn't look like a Chromium tree (no build/ or DEPS)."
    fi
}

require_branch_pin() {
    require_chromium_src
    local head
    head="$(cd "${CHROMIUM_SRC}" && git rev-parse --abbrev-ref --symbolic-full-name HEAD 2>/dev/null || echo unknown)"
    case "${head}" in
        refs/branch-heads/*|*branch-heads/*)
            log "Chromium tree at ${head}; OK."
            ;;
        *)
            log "WARN: Chromium tree at '${head}', not on a refs/branch-heads/* ref."
            log "WARN: Patches in patches/ are authored against the pinned release branch"
            log "WARN: (see docs/build/chromium-from-source.md §6). Continue at your own risk."
            ;;
    esac
}

require_depot_tools() {
    if ! command -v gn >/dev/null 2>&1; then
        die "gn not found on PATH. Source depot_tools."
    fi
    if ! command -v autoninja >/dev/null 2>&1; then
        die "autoninja not found on PATH. Source depot_tools."
    fi
}

# ---------------------------------------------------------------------
# Subcommands.
# ---------------------------------------------------------------------

cmd_apply_patches() {
    require_chromium_src
    require_branch_pin
    local patches=("${CB_ROOT}/patches/"[0-9]*.patch)
    if [[ ${#patches[@]} -eq 0 || ! -e "${patches[0]}" ]]; then
        die "no patches found at ${CB_ROOT}/patches/"
    fi

    # Defensive: a prior cb-build run that died mid-`git am` (Pod OOM,
    # node eviction, kubectl interrupt, etc.) leaves the chromium tree
    # with a held .git/index.lock and/or in-progress .git/rebase-apply/
    # state. Both are fatal to subsequent runs and require out-of-band
    # cleanup via a debug pod (per vaapi-builder's session 2026-05-01).
    # Recover both upfront so retries are self-healing.
    #
    # Also: prior runs that *succeeded* at apply-patches leave HEAD
    # advanced by 5 commits (the patches as commits). Re-running
    # apply-patches against that HEAD produces "needs merge" conflicts
    # because the patch context lines reference the pre-patch base.
    # Reset HEAD + worktree back to the LKGM base before the apply loop
    # so re-runs (after Pod OOM, node eviction, etc.) are idempotent.
    # observed 2026-05-02 by embedder-bootstrap, after repeated job
    # restarts left the tree in a state where apply-patches could
    # neither succeed nor cleanly retry.
    local lkgm_base="2731573cc6"
    if (cd "${CHROMIUM_SRC}" && git rev-parse --git-dir >/dev/null 2>&1); then
        # `git am --abort` is a no-op when no rebase-apply state exists,
        # but cleanly tears it down when one does. Suppress its noise so
        # the no-op case doesn't pollute logs.
        (cd "${CHROMIUM_SRC}" && git am --abort) >/dev/null 2>&1 || true
        rm -f  "${CHROMIUM_SRC}/.git/index.lock"
        rm -rf "${CHROMIUM_SRC}/.git/rebase-apply"
        # Reset HEAD + worktree to the LKGM base. -c required because
        # the chromium worktree on the build host has no committer
        # identity configured.
        if (cd "${CHROMIUM_SRC}" && git cat-file -e "${lkgm_base}^{commit}" 2>/dev/null); then
            (cd "${CHROMIUM_SRC}" && git \
                -c user.email=cb-build@triform.ai \
                -c user.name='cb-build' \
                reset --hard "${lkgm_base}") >/dev/null 2>&1 || \
                log "WARN: reset to ${lkgm_base} failed; continuing"
        else
            # LKGM commit is not in the shallow clone that
            # chromeless-build.sh STEP 1 creates (gclient sync runs
            # with --no-history --shallow). Fall back to the
            # fetched-branch ref that IS in the shallow clone:
            # refs/remotes/branch-heads/<CHROMIUM_BRANCH_NUMBER>.
            # Without this, SKIP_FETCH=1 retries on a previously
            # patched tree fail at STEP 3 because LKGM-reset is
            # skipped and apply-patches re-runs on top of HEAD that
            # already advanced by 5 patch commits. Observed 2026-05-17
            # on triform-7 after build #2-retry hit silent gclient
            # stall and we flipped SKIP_FETCH=1.
            log "WARN: LKGM base ${lkgm_base} not present; falling back to fetched branch ref"
            local branch_ref
            branch_ref="$(cd "${CHROMIUM_SRC}" && git for-each-ref \
                --format='%(refname)' \
                "refs/remotes/branch-heads/${CHROMIUM_BRANCH_NUMBER}" 2>/dev/null | head -1)"
            if [[ -n "${branch_ref}" ]]; then
                if (cd "${CHROMIUM_SRC}" && git \
                        -c user.email=cb-build@triform.ai \
                        -c user.name='cb-build' \
                        reset --hard "${branch_ref}") >/dev/null 2>&1; then
                    log "Reset to fallback ref ${branch_ref}"
                else
                    log "WARN: reset to ${branch_ref} failed; continuing"
                fi
            else
                log "WARN: no fallback ref for branch-heads/${CHROMIUM_BRANCH_NUMBER}; tree may be dirty"
            fi
        fi
        # `git reset --hard` only undoes TRACKED file changes; untracked
        # files persist. `git am` then refuses to apply a patch that
        # CREATES one of those files: "untracked working tree files
        # would be overwritten by merge". Observed 2026-05-17 on build
        # #6 v2: patches/0003 failed because
        # third_party/webrtc_overrides/cloud_browser/BUILD.gn was
        # untracked in the tree (residue from prior partial apply).
        #
        # The first attempt at this fix was a rootwide `git clean -fdx`
        # which is far too aggressive: chromium has many legitimately-
        # untracked artifacts (gclient-fetched binaries at
        # buildtools/linux64/gn, third_party/llvm-build/, .gclient_entries,
        # .cipd_client, etc.) that are NOT patch residue but ARE
        # untracked + ignored by .gitignore. Rootwide -x deleted those
        # too, broke STEP 5 gn gen ("Could not find gn executable at
        # buildtools/linux64/gn"). Observed on build #6 v3 (8rq9z).
        #
        # Surgical fix: only clean the paths that our patches CREATE
        # as new directories. Per current patch series:
        #   0001 modifies third_party/webrtc/api/* (tracked, reset handles)
        #   0002 modifies root BUILD.gn                (tracked, reset handles)
        #   0003 CREATES third_party/webrtc_overrides/cloud_browser/ ← clean here
        #   0004 modifies content/public/browser/*.cc  (tracked, reset handles)
        #   0005 modifies components/viz/host/*.h      (tracked, reset handles)
        # Only 0003 introduces a brand-new directory; the residue from
        # a partial prior run is always in that directory. Scoping the
        # clean to `--` <path> avoids touching gclient binaries.
        (cd "${CHROMIUM_SRC}" && git clean -fdx -- third_party/webrtc_overrides/cloud_browser/) >/dev/null 2>&1 || \
            log "WARN: scoped clean failed; untracked residue may persist"
    fi

    log "Applying ${#patches[@]} patch(es) to ${CHROMIUM_SRC}..."
    local p
    for p in "${patches[@]}"; do
        log "  -> $(basename "${p}")"

        # WHICH REPO does this patch belong to?
        #
        # `src/` is not one checkout. DEPS clones several sub-repos INTO it,
        # each with its own .git — third_party/webrtc is one (DEPS:3012).
        # A patch touching one of those cannot be applied from src/: `git am`
        # finds no blob for a path its index does not track, and
        # `--3way` then fails with
        #
        #   error: sha1 information is lacking or useless (<path>)
        #   error: could not build fake ancestor
        #
        # which reads as a malformed patch and is not one. Patches 0002/0003/
        # 0005 never hit it because they touch content/ and
        # third_party/webrtc_overrides/, both of which ARE in the src repo.
        # 0006 (the pulse ADM fix) is the first to touch third_party/webrtc.
        #
        # So: read the target path out of the patch and apply from whichever
        # checkout actually owns it.
        local target_repo="${CHROMIUM_SRC}"
        local strip_prefix=""
        local first_path
        first_path="$(sed -n 's|^+++ b/||p' "${p}" | head -1)"
        local sub
        for sub in third_party/webrtc third_party/angle v8; do
            case "${first_path}" in
                "${sub}"/*)
                    if [[ -d "${CHROMIUM_SRC}/${sub}/.git" ]]; then
                        target_repo="${CHROMIUM_SRC}/${sub}"
                        strip_prefix="${sub}/"
                        log "     (sub-repo: ${sub})"
                    fi
                    ;;
            esac
        done

        # Per-patch defensive: even after the upfront recovery above,
        # a failure mid-loop on patch N would leave state behind for
        # patch N+1. Same pattern, idempotent.
        rm -rf "${target_repo}/.git/rebase-apply"

        # A sub-repo patch carries paths relative to src/, so strip the
        # sub-repo prefix on the way in rather than rewriting the patch file
        # — the patch stays readable against the chromium tree it documents.
        local am_input="${p}"
        if [[ -n "${strip_prefix}" ]]; then
            am_input="$(mktemp)"
            sed "s|^--- a/${strip_prefix}|--- a/|; s|^+++ b/${strip_prefix}|+++ b/|; s|^diff --git a/${strip_prefix}|diff --git a/|; s| b/${strip_prefix}| b/|" \
                "${p}" > "${am_input}"
        fi

        if ! (cd "${target_repo}" && git \
                -c user.email=iggy@triform.ai \
                -c user.name="Iggy" \
                am --3way --keep-non-patch "${am_input}"); then
            [[ -n "${strip_prefix}" ]] && rm -f "${am_input}"
            die "patch failed to apply: ${p} (repo: ${target_repo})"
        fi
        [[ -n "${strip_prefix}" ]] && rm -f "${am_input}"
    done
    log "All patches applied."
}

cmd_gen() {
    require_chromium_src
    require_depot_tools
    # CHROMELESS_BUILD_PROFILE selects which args.<profile>.gn overlay to
    # import. Each overlay imports the base args.gn first, then sets its
    # profile-specific *_libdir / *_sdk_path to flip the matching
    # encoder gate on. "sw" (default) imports the base args.gn directly
    # — no HW or x264 path enabled.
    #
    # HISTORY (2026-07-30): this read `CB_BUILD_PROFILE` from 2026-05-01
    # (Track F1) until today, but the c12d40d cb-*→chromeless rename
    # (2026-05-05) renamed the WRAPPER's variable — chromeless-build.sh
    # validates and exports CHROMELESS_BUILD_PROFILE — without touching
    # this reader. Result: the wrapper's export never reached this
    # switch, `:-sw` won silently, and EVERY build since 2026-05-05 —
    # including every deployed cr7727-* production image — was gn-gen'd
    # with the base args.gn: x264_libdir empty, HAS_X264 undefined,
    # H264Encoder compiled as the stub. Nobody noticed for 12 weeks
    # because (a) the t7 lane also skipped the unit tests that assert
    # InitEncode works, and (b) at runtime the encoder factory's probe
    # fails closed onto libwebrtc's built-in encoders, so video kept
    # flowing — just never through the x264 path the profile was meant
    # to enable. Found by the restored verify loop: H264EncoderTest
    # failed 7/7 with WEBRTC_VIDEO_CODEC_ERROR from the #else stub,
    # while the same params succeeded against the staged libx264
    # directly, and `objdump -p cloud_browser_worker` showed no x264
    # NEEDED entry.
    # CB_BUILD_PROFILE is still honoured as a fallback for any direct
    # caller of this script that predates the rename.
    #
    # Valid values: sw (default), x264, vaapi, nvenc, all.
    local profile="${CHROMELESS_BUILD_PROFILE:-${CB_BUILD_PROFILE:-sw}}"
    local overlay
    case "${profile}" in
        sw)             overlay="args.gn";;
        x264|vaapi|nvenc|all)
                        overlay="args.${profile}.gn";;
        *)              die "unknown CHROMELESS_BUILD_PROFILE=${profile} (expected sw|x264|vaapi|nvenc|all)";;
    esac
    local args_path="//cloud-browser/capture/build-integration/${overlay}"
    log "gn gen ${OUT_DIR} (profile=${profile}, importing ${args_path})..."
    (cd "${CHROMIUM_SRC}" && \
        gn gen "${OUT_DIR}" \
            --args="import(\"${args_path}\")")
    log "gn gen done."
}

cmd_ninja() {
    require_chromium_src
    require_depot_tools
    log "autoninja -C ${OUT_DIR} ${TARGETS}"
    # shellcheck disable=SC2086
    # TARGETS is intentionally word-split into autoninja arguments.
    (cd "${CHROMIUM_SRC}" && autoninja -C "${OUT_DIR}" ${TARGETS})
    log "ninja done."
}

cmd_all() {
    cmd_apply_patches
    cmd_gen
    cmd_ninja
}

cmd_clean() {
    require_chromium_src
    if [[ -d "${CHROMIUM_SRC}/${OUT_DIR}" ]]; then
        log "Removing ${CHROMIUM_SRC}/${OUT_DIR}"
        rm -rf "${CHROMIUM_SRC:?}/${OUT_DIR:?}"
    else
        log "Nothing to clean at ${CHROMIUM_SRC}/${OUT_DIR}"
    fi
}

usage() {
    cat <<USAGE
Usage: build.sh <subcommand>

Subcommands:
  apply-patches   Apply patches/*.patch to the Chromium tree at
                  CHROMIUM_SRC.
  gen             Run \`gn gen \${OUT_DIR}\` with our args.gn.
  ninja           Run \`autoninja -C \${OUT_DIR} \${TARGETS}\`.
  all             apply-patches + gen + ninja.
  clean           Remove the OUT_DIR.

Environment:
  CHROMIUM_SRC    Path to the Chromium src/ checkout.
                  Default: directory two levels above this script
                  (we expect this repo to be symlinked into src/ as
                   //cloud-browser).
  OUT_DIR         Build output dir, relative to CHROMIUM_SRC.
                  Default: out/cb-release.
  TARGETS         Space-separated list of ninja targets.
                  Default: ${TARGETS_DEFAULT}.

See capture/build-integration/README.md for the full setup.
USAGE
}

# ---------------------------------------------------------------------
# Dispatch.
# ---------------------------------------------------------------------

if [[ $# -eq 0 ]]; then
    usage
    exit 1
fi

case "${1:-}" in
    apply-patches)  shift; cmd_apply_patches "$@";;
    gen)            shift; cmd_gen "$@";;
    ninja)          shift; cmd_ninja "$@";;
    all)            shift; cmd_all "$@";;
    clean)          shift; cmd_clean "$@";;
    -h|--help|help) usage;;
    *)              usage; exit 1;;
esac

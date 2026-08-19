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
        # Per-patch defensive: even after the upfront recovery above,
        # a failure mid-loop on patch N would leave state behind for
        # patch N+1. Same pattern, idempotent.
        rm -rf "${CHROMIUM_SRC}/.git/rebase-apply"
        if ! (cd "${CHROMIUM_SRC}" && git \
                -c user.email=iggy@triform.ai \
                -c user.name="Iggy" \
                am --3way --keep-non-patch "${p}"); then
            die "patch failed to apply: ${p}"
        fi
    done
    log "All patches applied."
}

cmd_gen() {
    require_chromium_src
    require_depot_tools
    # Selects which args.<profile>.gn overlay to import. Each overlay
    # imports the base args.gn first, then sets its profile-specific
    # *_libdir / *_sdk_path to flip the matching encoder gate on. "sw"
    # imports the base args.gn directly — no HW or x264 path enabled.
    #
    # Valid values: sw (default), x264, vaapi, nvenc, all.
    #
    # ─── READ BOTH NAMES. THIS COST 12 WEEKS OF SILENT WRONG BUILDS. ───
    #
    # build/chromeless-build.sh exports CHROMELESS_BUILD_PROFILE. This
    # function used to read only CB_BUILD_PROFILE — the old name, from
    # before the cb-*→chromeless rename (c12d40d) changed the writer and
    # not the reader. The export never arrived, `:-sw` won, and EVERY
    # image built after that rename was gn-gen'd profile=sw with
    # x264_libdir empty and HAS_X264 undefined.
    #
    # Nothing caught it, because both of its alarms were disabled:
    #   * the t7 lane had dropped the unit tests, so H264EncoderTest's
    #     9/9 InitEncode == -1 was never run;
    #   * at runtime the encoder factory fails CLOSED onto libwebrtc's
    #     built-in encoders, so video kept flowing and looked fine. Every
    #     fps/quality measurement taken in that window was therefore made
    #     against libwebrtc software encoders, not x264.
    #
    # CHROMELESS_BUILD_PROFILE is authoritative; CB_BUILD_PROFILE remains
    # as a fallback so an old caller still works.
    local profile="${CHROMELESS_BUILD_PROFILE:-${CB_BUILD_PROFILE:-sw}}"
    local overlay
    case "${profile}" in
        sw)             overlay="args.gn";;
        x264|vaapi|nvenc|all)
                        overlay="args.${profile}.gn";;
        *)              die "unknown build profile '${profile}' (expected sw|x264|vaapi|nvenc|all) — set CHROMELESS_BUILD_PROFILE";;
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

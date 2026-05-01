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
            log "WARN: LKGM base ${lkgm_base} not present; skipping reset"
        fi
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
    # CB_BUILD_PROFILE selects which args.<profile>.gn overlay to import.
    # Each overlay imports the base args.gn first, then sets its
    # profile-specific *_libdir / *_sdk_path to flip the matching
    # encoder gate on. "sw" (default) imports the base args.gn directly
    # — no HW or x264 path enabled.
    #
    # Valid values: sw (default), x264, vaapi, nvenc, all.
    local profile="${CB_BUILD_PROFILE:-sw}"
    local overlay
    case "${profile}" in
        sw)             overlay="args.gn";;
        x264|vaapi|nvenc|all)
                        overlay="args.${profile}.gn";;
        *)              die "unknown CB_BUILD_PROFILE=${profile} (expected sw|x264|vaapi|nvenc|all)";;
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

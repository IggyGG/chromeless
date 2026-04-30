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
    log "Applying ${#patches[@]} patch(es) to ${CHROMIUM_SRC}..."
    local p
    for p in "${patches[@]}"; do
        log "  -> $(basename "${p}")"
        if ! (cd "${CHROMIUM_SRC}" && git apply --3way --check "${p}"); then
            die "patch failed pre-check: ${p}"
        fi
        if ! (cd "${CHROMIUM_SRC}" && git \
                -c user.email=iggy@triform.ai \
                -c user.name="Iggy" \
                am --keep-non-patch "${p}"); then
            die "patch failed to apply: ${p}"
        fi
    done
    log "All patches applied."
}

cmd_gen() {
    require_chromium_src
    require_depot_tools
    log "gn gen ${OUT_DIR} (importing //cloud-browser/capture/build-integration/args.gn)..."
    (cd "${CHROMIUM_SRC}" && \
        gn gen "${OUT_DIR}" \
            --args='import("//cloud-browser/capture/build-integration/args.gn")')
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

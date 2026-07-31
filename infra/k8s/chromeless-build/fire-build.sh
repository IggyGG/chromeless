#!/usr/bin/env bash
#
# fire-build.sh — launch a chromeless build Job from a git ref, and refuse
# to launch one whose LIVE spec does not match what that ref says.
#
# WHY THIS EXISTS
# ---------------
# Firing a build is three steps: take a manifest, adjust the name/ref/targets,
# apply it. Every one of those steps is a place for the thing you MEANT to
# test and the thing that ACTUALLY RAN to come apart, and the divergence is
# invisible: the Job runs, the Job finishes, the Job reports a verdict. The
# verdict is about whatever the live spec said, not about your branch.
#
# On 2026-07-30 this cost three wrong results in one afternoon:
#
#   1. A manifest derived by `sed` from a PREVIOUS job's copy. The branch had
#      six test targets; the live Job had three. The build went green and was
#      reported as "all six targets compile" — it had compiled three.
#   2. The same class again, caught only because the live object was read
#      back by hand and disagreed.
#   3. A branch that did not contain the commit under test at all, because
#      OUR_REPO_REF still said "main".
#
# In every case the fix was the same: READ THE LIVE OBJECT BACK. So that is
# what this script does, and it does it before the job starts rather than
# after it has burned 30 minutes.
#
# WHAT IT GUARANTEES
# ------------------
#   * The manifest comes from `git show <ref>:<path>` — the ref's own file,
#     never a local edit, never a previous job's copy, never sed output.
#   * OUR_REPO_REF is rewritten to <ref> at every site (there are two: the
#     init container and the build container; a mismatch between them silently
#     runs an old build script — that one bit us on 2026-05-17).
#   * After apply, the LIVE Job object is read back and its targets, ref, and
#     image are diffed against the ref's manifest. Mismatch = the Job is
#     deleted and the script exits non-zero. You cannot get a verdict from a
#     spec you did not intend.
#
# WHAT IT DOES NOT DO
# -------------------
# It does not watch the build, and it does not interpret the result. Reading
# a build log is still a thing you do with your eyes. See the README's
# "Watch the build" section.
#
# USAGE
#   ./fire-build.sh <ref> [--name NAME] [--manifest FILE] [--keep-going]
#
#   ref          git ref to build (branch, tag, or SHA). Required — there is
#                no default, because defaulting to main is exactly how you
#                end up testing main while believing you tested your branch.
#   --name       Job name. Default: chromeless-build-<sanitized ref>.
#   --manifest   Manifest path within the ref. Default the x264-t7 lane.
#   --keep-going Set NINJA_KEEP_GOING=0 so one pass collects EVERY failing
#                TU instead of stopping at the first. Use when probing for
#                drift; leave off for a normal build.

set -euo pipefail

NS=chromeless-build
DEFAULT_MANIFEST=infra/k8s/chromeless-build/build-job-x264-t7.yaml

die() { echo "fire-build: $*" >&2; exit 1; }

REF=""
NAME=""
MANIFEST="${DEFAULT_MANIFEST}"
KEEP_GOING=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --name)       NAME="${2:?--name needs a value}"; shift 2 ;;
        --manifest)   MANIFEST="${2:?--manifest needs a value}"; shift 2 ;;
        --keep-going) KEEP_GOING=1; shift ;;
        -h|--help)    sed -n '2,60p' "$0"; exit 0 ;;
        -*)           die "unknown flag: $1" ;;
        *)            [[ -z "${REF}" ]] || die "ref given twice: ${REF} and $1"
                      REF="$1"; shift ;;
    esac
done

[[ -n "${REF}" ]] || die "no ref given. Usage: fire-build.sh <ref> [--name NAME]"

command -v kubectl >/dev/null 2>&1 || die "kubectl not on PATH"
git rev-parse --verify "${REF}" >/dev/null 2>&1 \
    || die "ref '${REF}' does not resolve. Push it first — the build pod
        clones from the REMOTE, so a local-only branch will 'succeed' at
        this check and then fail to clone inside the pod."

REF_SHA="$(git rev-parse --short "${REF}")"

if [[ -z "${NAME}" ]]; then
    # k8s names: lowercase alphanumeric and '-', so slashes in branch names
    # have to go.
    NAME="chromeless-build-$(printf '%s' "${REF}" | tr '/_' '--' | tr -cd 'a-z0-9-' | cut -c1-40)"
fi

echo "fire-build: ref=${REF} (${REF_SHA})  job=${NAME}  manifest=${MANIFEST}"

# --- 1. Take the manifest from the REF, not from the working tree. ----------
#
# `git show <ref>:<path>` is the whole point. A local edit you forgot to
# commit, or a stale copy left over from a previous job, cannot get in here.
RENDERED="$(mktemp)"
trap 'rm -f "${RENDERED}"' EXIT

git show "${REF}:${MANIFEST}" > "${RENDERED}" 2>/dev/null \
    || die "'${MANIFEST}' does not exist at ref '${REF}'"

python3 - "${RENDERED}" "${NAME}" "${REF}" "${KEEP_GOING}" <<'PY'
import re, sys

path, name, ref, keep_going = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
lines = open(path).read().split('\n')
out, pending_ref, ref_sites = [], False, 0

for ln in lines:
    stripped = ln.strip()

    # Job name: the metadata.name at top level (two-space indent).
    if re.fullmatch(r'  name: chromeless-build\S*', ln):
        out.append(f'  name: {name}')
        continue

    if stripped == '- name: OUR_REPO_REF':
        pending_ref = True
        out.append(ln)
        continue

    if pending_ref and stripped.startswith('value:'):
        indent = ln[:len(ln) - len(ln.lstrip())]
        out.append(f'{indent}value: "{ref}"')
        if keep_going:
            out.append(f'{indent[:-2]}- name: NINJA_KEEP_GOING')
            out.append(f'{indent}value: "0"')
        pending_ref = False
        ref_sites += 1
        continue

    out.append(ln)

# Both containers carry OUR_REPO_REF. If we rewrote fewer than two, one of
# them still points somewhere else — which is the 2026-05-17 failure exactly
# (init=main + build=cv2/m2 ran main's older build script for two attempts).
if ref_sites < 2:
    sys.exit(f'fire-build: rewrote OUR_REPO_REF at only {ref_sites} site(s); '
             'expected 2 (init container + build container). Manifest shape '
             'changed — fix this script before firing.')

open(path, 'w').write('\n'.join(out))
print(f'fire-build: rewrote OUR_REPO_REF at {ref_sites} site(s)')
PY

# --- 2. What SHOULD be live, read from the rendered manifest. ---------------
expected_targets="$(python3 -c "
import re,sys
t=open('${RENDERED}').read()
m=re.search(r'name: CHROMELESS_BUILD_TARGETS.*?value: \"(.*?)\"', t, re.S)
print(' '.join(sorted(m.group(1).split())) if m else '')
")"
[[ -n "${expected_targets}" ]] || die "no CHROMELESS_BUILD_TARGETS in the manifest"

echo "fire-build: expecting $(printf '%s' "${expected_targets}" | wc -w | tr -d ' ') target(s)"

# --- 3. Apply. Delete any same-named Job first: Job pod templates are
#        IMMUTABLE, so `kubectl apply` over an existing Job silently keeps
#        the OLD spec and you get a verdict about the previous run's config.
kubectl delete job -n "${NS}" "${NAME}" --ignore-not-found --wait=true >/dev/null
kubectl apply -f "${RENDERED}" >/dev/null
echo "fire-build: applied"

# --- 4. Read the LIVE object back and diff it. ------------------------------
#
# This is the step that all three of the 2026-07-30 wrong results skipped.
live_targets="$(kubectl get job -n "${NS}" "${NAME}" -o \
    jsonpath='{.spec.template.spec.containers[0].env[?(@.name=="CHROMELESS_BUILD_TARGETS")].value}' \
    | tr ' ' '\n' | sort | tr '\n' ' ' | sed 's/ $//')"
live_refs="$(kubectl get job -n "${NS}" "${NAME}" -o \
    jsonpath='{range .spec.template.spec..env[?(@.name=="OUR_REPO_REF")]}{.value}{"\n"}{end}' \
    | sort -u | tr -d '\n')"

fail=0

if [[ "${live_targets}" != "${expected_targets}" ]]; then
    echo "fire-build: LIVE TARGETS DISAGREE WITH THE REF" >&2
    echo "  manifest wants: ${expected_targets}" >&2
    echo "  live job  has : ${live_targets}" >&2
    fail=1
fi

if [[ "${live_refs}" != "${REF}" ]]; then
    echo "fire-build: LIVE OUR_REPO_REF DISAGREES" >&2
    echo "  wanted: ${REF}" >&2
    echo "  live  : ${live_refs}  (multiple values = containers disagree)" >&2
    fail=1
fi

if [[ "${fail}" -ne 0 ]]; then
    echo "fire-build: deleting the Job — a verdict from this spec would be" >&2
    echo "            about something other than ${REF}." >&2
    kubectl delete job -n "${NS}" "${NAME}" --ignore-not-found >/dev/null
    exit 1
fi

echo "fire-build: VERIFIED — live Job spec matches ${REF} (${REF_SHA})"
# shellcheck disable=SC2086
# Word-splitting is the point: one target per line, so a six-target list is
# six lines you can count rather than one line you skim.
printf '  targets: %s\n' ${live_targets}
[[ -n "${KEEP_GOING}" ]] && echo "  NINJA_KEEP_GOING=0 (one pass collects every failing TU)"
echo
echo "Watch it:"
echo "  kubectl get job -n ${NS} ${NAME} -w"
echo "  kubectl logs -n ${NS} -l job-name=${NAME} -f --tail=50"

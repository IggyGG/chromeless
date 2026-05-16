#!/usr/bin/env bash
#
# verification/scripts/check-branch-protection.sh — R8 compensating evidence.
#
# Forgejo branch-protection rules live OUTSIDE the repo (they're stored
# per-branch in Forgejo's database). This script queries the Forgejo API
# and asserts that `native-peer-gate / native-peer-gate-scaffold` is in
# the required-status-checks list for the specified branch.
#
# Usage:
#   FORGEJO_TOKEN=... ./verification/scripts/check-branch-protection.sh \
#     --owner triform --repo chromeless --branch integration/native-peer
#
# Requires curl + jq.

set -euo pipefail

OWNER="${OWNER:-triform}"
REPO="${REPO:-chromeless}"
BRANCH="${BRANCH:-integration/native-peer}"
BASE_URL="${FORGEJO_BASE_URL:-https://forgejo.triform.dev}"
EXPECT_CHECK="${EXPECT_CHECK:-native-peer-gate / native-peer-gate-scaffold}"

while [ $# -gt 0 ]; do
    case "$1" in
        --owner)  OWNER="$2";  shift 2;;
        --repo)   REPO="$2";   shift 2;;
        --branch) BRANCH="$2"; shift 2;;
        --expect-check) EXPECT_CHECK="$2"; shift 2;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done

if [ -z "${FORGEJO_TOKEN:-}" ]; then
    echo "FORGEJO_TOKEN env var required" >&2
    exit 2
fi

command -v curl >/dev/null || { echo "curl required" >&2; exit 127; }
command -v jq >/dev/null || { echo "jq required" >&2; exit 127; }

# URL-encode the branch name (handles 'integration/native-peer' → 'integration%2Fnative-peer').
BRANCH_ENC=$(printf '%s' "$BRANCH" | jq -sRr @uri)

URL="${BASE_URL}/api/v1/repos/${OWNER}/${REPO}/branch_protections/${BRANCH_ENC}"

resp=$(curl -fsS -H "Authorization: token ${FORGEJO_TOKEN}" "$URL" || true)
if [ -z "$resp" ]; then
    echo "FAIL: no branch_protection rule found for ${OWNER}/${REPO}@${BRANCH}" >&2
    exit 1
fi

# Forgejo's branch_protection schema has `status_check_contexts: []` — the
# names of required checks.
if echo "$resp" | jq -e --arg c "$EXPECT_CHECK" '.status_check_contexts | index($c) != null' >/dev/null; then
    echo "PASS: required check '${EXPECT_CHECK}' present on ${BRANCH}"
    exit 0
else
    echo "FAIL: required check '${EXPECT_CHECK}' NOT present on ${BRANCH}" >&2
    echo "current status_check_contexts:" >&2
    echo "$resp" | jq -r '.status_check_contexts[]?' >&2 || true
    exit 1
fi

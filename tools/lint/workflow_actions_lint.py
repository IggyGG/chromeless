#!/usr/bin/env python3
"""Check that every `uses:` action resolves on this repo's CI host.

WHY THIS EXISTS
---------------
CI here runs on Forgejo (forgejo.triform.dev), not GitHub, and Forgejo
resolves actions from its own mirror at data.forgejo.org. Two of the actions
in these workflows do not exist there:

    actions/github-script            -> 404
    actions/attest-build-provenance  -> 404

That is not a soft failure. Forgejo's runner git-clones EVERY `uses:` in a
job before executing ANY step, so a single unresolvable action fails the
whole job — regardless of any `if:` condition on it. The logs show
"skipping post step for 'actions/checkout@v4'; step was not executed",
i.e. the job died before checkout.

`actions/github-script` alone accounted for 56 of this repo's 171 CI
failures. Across June-July 2026 the repo had ZERO successful runs; this was
one of the two root causes.

The failure mode is nasty because it is invisible locally: the workflow is
valid YAML, valid GitHub Actions, and passes any schema check. It only fails
on the host that actually runs it.

Usage:
    python3 tools/lint/workflow_actions_lint.py           # offline, denylist
    python3 tools/lint/workflow_actions_lint.py --online  # probe the mirror

Exit status: 0 clean, 1 findings.
"""

from __future__ import annotations

import argparse
import glob
import re
import sys

MIRROR = "https://data.forgejo.org"

# Actions verified ABSENT from data.forgejo.org. Offline default so the lint
# is fast and works without network; --online re-checks reality.
KNOWN_MISSING = {
    "actions/github-script": "no Forgejo mirror; use a plain `run:` step "
                             "(curl/python against the Forgejo API)",
    "actions/attest-build-provenance": "no Forgejo mirror, and the attestation "
                                       "API is GitHub-specific; cosign signing "
                                       "is the portable alternative",
}

# Captures owner/repo, tolerating a subpath: `github/codeql-action/init@v3`
# resolves the REPO `github/codeql-action`, so the subpath must be stripped
# before probing. An earlier version anchored on exactly two path segments
# and silently skipped every subpath action — which is how the codeql jobs
# (38 failures, same 404 root cause) went unnoticed by this very linter.
USES_RE = re.compile(
    r"^\s*(?:-\s*)?uses:\s*['\"]?([A-Za-z0-9_.\-]+/[A-Za-z0-9_.\-]+)(?:/[A-Za-z0-9_.\-/]+)?@")


# A job carrying this guard never runs off github.com, so Forgejo never
# resolves its actions and a GitHub-only action there is intentional, not a
# defect. `codeql.yml` uses exactly this to keep CodeQL — a GitHub-hosted
# service with no Forgejo equivalent — from failing every run.
GITHUB_ONLY_GUARD = re.compile(r"^\s*if:\s*.*github\.server_url\s*==\s*'https://github\.com'")
JOB_RE = re.compile(r"^  [A-Za-z0-9_.\-]+:\s*$")


def collect(paths: list[str]) -> dict[str, list[str]]:
    """action -> [file:line, ...], skipping jobs gated to github.com only."""
    found: dict[str, list[str]] = {}
    for path in paths:
        github_only_job = False
        with open(path, encoding="utf-8") as fh:
            for n, line in enumerate(fh, 1):
                # A new top-level job resets the guard.
                if JOB_RE.match(line):
                    github_only_job = False
                if GITHUB_ONLY_GUARD.match(line):
                    github_only_job = True
                m = USES_RE.match(line)
                if m and not github_only_job:
                    found.setdefault(m.group(1), []).append(f"{path}:{n}")
    return found


def probe(action: str) -> bool:
    """True if the action exists on the mirror.

    Only a 404 counts as "missing". The mirror answers 403 for repositories
    that DO exist when it doesn't like the client (it bot-blocks unknown
    user-agents), so treating any >=400 as missing would flag
    `actions/checkout` — which plainly works in production. Verified:
    actions/checkout -> 403, actions/github-script -> 404.

    Anything else (network error, timeout, 5xx) is treated as "exists" so a
    flaky probe can never fail the build on a false negative.
    """
    import urllib.error
    import urllib.request
    try:
        req = urllib.request.Request(f"{MIRROR}/{action}", method="GET")
        with urllib.request.urlopen(req, timeout=10):
            return True
    except urllib.error.HTTPError as e:
        return e.code != 404
    except Exception:
        return True


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--online", action="store_true",
                    help=f"probe {MIRROR} instead of using the built-in denylist")
    args = ap.parse_args(argv)

    paths = sorted(glob.glob(".github/workflows/*.yml") +
                   glob.glob(".github/workflows/*.yaml") +
                   glob.glob(".forgejo/workflows/*.yml") +
                   glob.glob(".forgejo/workflows/*.yaml"))
    if not paths:
        print("workflow-actions-lint: no workflow files found")
        return 0

    actions = collect(paths)
    findings: list[str] = []

    for action, sites in sorted(actions.items()):
        if args.online:
            bad, why = (not probe(action)), f"does not resolve on {MIRROR}"
        else:
            bad, why = action in KNOWN_MISSING, KNOWN_MISSING.get(action, "")
        if bad:
            for site in sites:
                findings.append(f"{site}: `{action}` — {why}")

    if findings:
        print(f"workflow-actions-lint: {len(findings)} finding(s)\n")
        for f in findings:
            print(f"  {f}")
        print("\nForgejo resolves every `uses:` in a job BEFORE running any step,")
        print("so an unresolvable action fails the whole job even when its `if:`")
        print("is false. Replace it with a plain `run:` step.")
        return 1

    mode = "probed" if args.online else "denylist"
    print(f"workflow-actions-lint: clean "
          f"({len(actions)} distinct actions across {len(paths)} workflows, {mode})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

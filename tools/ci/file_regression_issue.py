#!/usr/bin/env python3
"""File a harness-regression issue via the forge API.

Replaces `uses: actions/github-script@v7`, which does not exist on Forgejo's
action mirror (data.forgejo.org/actions/github-script -> 404). Because
Forgejo resolves every `uses:` in a job BEFORE executing any step, that one
unresolvable action failed every run of harness-loopback.yml — 56 of this
repo's 171 CI failures — with the job dying before checkout.

Lives in a file rather than inline in the workflow: the previous inline
attempt nested a Python heredoc inside a YAML block scalar inside a shell
string, which broke the YAML. Workflow steps should call scripts.

Reads from the environment (all set by the workflow):
    API_TOKEN   forge API token
    SERVER_URL  e.g. https://forgejo.triform.dev
    REPO        owner/name
    RUN_ID      workflow run id, for the backlink

Best-effort by design: a failure to file the issue must not mask the
regression itself, which the workflow's next step fails on. Always exits 0.
"""

from __future__ import annotations

import datetime
import json
import os
import pathlib
import sys
import urllib.request


def main() -> int:
    token = os.environ.get("API_TOKEN", "")
    if not token:
        print("::warning::no API token available — skipping issue creation")
        return 0

    server = os.environ.get("SERVER_URL", "").rstrip("/")
    repo = os.environ.get("REPO", "")
    run_id = os.environ.get("RUN_ID", "")
    if not server or not repo:
        print("::warning::SERVER_URL/REPO unset — skipping issue creation")
        return 0

    cmp_path = pathlib.Path("comparison.txt")
    comparison = (cmp_path.read_text(encoding="utf-8") if cmp_path.exists()
                  else "(comparison.txt missing — see workflow logs)")

    today = datetime.datetime.now(datetime.timezone.utc).date().isoformat()
    body = "\n".join([
        "`harness-loopback` detected a regression past the "
        "5 ms p50 / 10 ms p95 threshold.",
        "",
        "## Comparison",
        "",
        "```",
        comparison.rstrip(),
        "```",
        "",
        f"Workflow run: {server}/{repo}/actions/runs/{run_id}",
        "",
        "See docs/operations/runbook.md#debugging-a-slow-session for triage steps.",
    ])

    payload = json.dumps({
        "title": f"Harness regression on {today}",
        "body": body,
        "labels": ["regression", "harness"],
    }).encode()

    req = urllib.request.Request(
        f"{server}/api/v1/repos/{repo}/issues",
        data=payload,
        method="POST",
        headers={"Content-Type": "application/json",
                 "Authorization": f"token {token}"},
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            print(f"filed regression issue (HTTP {resp.status})")
    except Exception as exc:  # noqa: BLE001 — best-effort by contract
        print(f"::warning::could not file regression issue: {exc}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

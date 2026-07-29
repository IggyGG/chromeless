#!/usr/bin/env python3
"""Create or update a release from RELEASE_NOTES.md via the forge API.

Replaces `uses: softprops/action-gh-release@v2`, which 404s on Forgejo's
action mirror. Forgejo resolves every `uses:` in a job before running any
step, so an unresolvable action fails the whole job regardless of its `if:`.

Forgejo's release API is GitHub-compatible for this operation, so a plain
API call keeps the workflow working on either host.

Reads from the environment (set by the workflow):
    API_TOKEN   forge API token
    SERVER_URL  e.g. https://forgejo.triform.dev
    REPO        owner/name
    TAG         the release tag

Unlike the regression-issue script, failures here are fatal: a release that
silently doesn't publish is worse than a red build.
"""

from __future__ import annotations

import json
import os
import pathlib
import sys
import urllib.error
import urllib.request


def call(url: str, token: str, payload: dict | None = None, method: str = "GET") -> dict:
    req = urllib.request.Request(
        url,
        data=json.dumps(payload).encode() if payload is not None else None,
        method=method,
        headers={"Content-Type": "application/json",
                 "Authorization": f"token {token}"},
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        raw = resp.read()
    return json.loads(raw) if raw else {}


def main() -> int:
    token = os.environ.get("API_TOKEN", "")
    server = os.environ.get("SERVER_URL", "").rstrip("/")
    repo = os.environ.get("REPO", "")
    tag = os.environ.get("TAG", "")

    missing = [k for k, v in
               {"API_TOKEN": token, "SERVER_URL": server, "REPO": repo, "TAG": tag}.items()
               if not v]
    if missing:
        print(f"::error::missing required env: {', '.join(missing)}")
        return 1

    notes = pathlib.Path("RELEASE_NOTES.md")
    if not notes.exists():
        print("::error::RELEASE_NOTES.md not found")
        return 1

    base = f"{server}/api/v1/repos/{repo}/releases"
    payload = {
        "tag_name": tag,
        "name": tag,
        "body": notes.read_text(encoding="utf-8"),
        "draft": False,
        "prerelease": False,
    }

    try:
        existing = call(f"{base}/tags/{tag}", token)
        call(f"{base}/{existing['id']}", token, payload, method="PATCH")
        print(f"updated release {tag}")
    except urllib.error.HTTPError as exc:
        if exc.code != 404:
            print(f"::error::release API returned HTTP {exc.code}: {exc.read()[:400]!r}")
            return 1
        call(base, token, payload, method="POST")
        print(f"created release {tag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

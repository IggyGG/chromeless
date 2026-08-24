#!/usr/bin/env python3
"""The worker-image pin must be internally consistent and point at real code.

WHY THIS EXISTS
---------------
On 2026-08-17 the repo had THREE disagreeing answers to "which worker image do
we test against":

  build/guest-release.json           cr7727-44f2e20dece6   built 2026-08-11
  Forgejo var CHROMELESS_IMAGE       cr7727-f9e46a1434b5   built 2026-07-30
  main                               carried fixes newer than both

CI used the variable, so its container-smoke job booted a browser containing
none of the product under test — and passed. That is the exact failure shape
CLAUDE.md warns about: "a green build lane is green about a target list, not
about the tree." The gate was real (it pulls an image, boots it, drives CDP,
screenshots) and still told us nothing, because it was real about the wrong
binary.

The root cause was mechanical, not careless. `chromeless-kaniko-push.sh` writes
the provenance record only after `kubectl logs -f` returns, and that command
fails outright against a pod still in ContainerCreating. A push that SUCCEEDED
could therefore leave the pin untouched, silently. Both halves are fixed — the
script now waits for the pod to be followable and treats the Job condition as
the authority — and this lint is the backstop for the next mechanism nobody
predicted.

WHAT IT CHECKS
--------------
1. build/guest-release.json parses and has every field the schema promises.
2. Its `commit` is a real commit in this repository. A pin naming a SHA that no
   longer exists (force-push, wrong clone) is worse than no pin: it looks
   authoritative and cannot be verified.
3. Its `image` tag ends with the same short SHA as `commit`. The tag is
   `cr<branch>-<sha>` by construction, so a mismatch means the record was
   hand-edited or assembled from two different builds.
4. `.github/workflows/ci.yml` resolves the image from this file, not solely
   from a repo variable that lives outside git and moves without review.

WHAT IT DELIBERATELY DOES NOT CHECK
-----------------------------------
Whether the pinned commit is an ancestor of main, or how old it is. A pin
legitimately lags main between builds — the browser needs a 4-8h Chromium
build, so "the pin is behind" is the normal state, not a defect. Failing on
that would fire constantly and teach people to ignore this lint, which
CLAUDE.md is explicit about: a false positive costs more trust than a missed
defect costs time.

It also does not contact the registry. The digest is recorded for humans and
for `crane`; a lint that needs network credentials is a lint that gets skipped.
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
RECORD = REPO / "build" / "guest-release.json"
# Every workflow that boots a worker image, not just ci.yml. e2e.yml and
# native-peer-gate.yml resolve from this record too, and a lint that policed
# only one of the three would go green while the other two drifted back to the
# out-of-git variable — the same "green about a subset" shape as the unbuilt
# test() targets.
CI_WORKFLOWS = (
    REPO / ".github" / "workflows" / "ci.yml",
    REPO / ".github" / "workflows" / "e2e.yml",
    REPO / ".github" / "workflows" / "native-peer-gate.yml",
)

REQUIRED_FIELDS = ("schema", "commit", "image", "dockerfile", "built_at", "recorded_by")


def fail(msg: str) -> None:
    print(f"guest-release-lint: {msg}", file=sys.stderr)


def main() -> int:
    problems: list[str] = []

    if not RECORD.exists():
        # Absence is legitimate: a fork with no build of its own has nothing to
        # pin. CI falls back to the CHROMELESS_IMAGE variable, and skips the
        # smoke entirely when that is unset too.
        print("guest-release-lint: no build/guest-release.json — nothing to check")
        return 0

    try:
        rec = json.loads(RECORD.read_text())
    except json.JSONDecodeError as e:
        fail(f"build/guest-release.json is not valid JSON: {e}")
        return 1

    missing = [f for f in REQUIRED_FIELDS if not rec.get(f)]
    if missing:
        problems.append(f"missing or empty field(s): {', '.join(missing)}")

    commit = rec.get("commit", "")
    image = rec.get("image", "")

    if commit and not re.fullmatch(r"[0-9a-f]{40}", commit):
        problems.append(f"commit {commit!r} is not a full 40-char SHA")
    elif commit:
        # Does this commit exist here? `cat-file -e` is the cheap existence
        # check; ^{commit} rejects a SHA that happens to name a tree or blob.
        ok = subprocess.run(
            ["git", "-C", str(REPO), "cat-file", "-e", f"{commit}^{{commit}}"],
            capture_output=True,
        ).returncode == 0
        if not ok:
            problems.append(
                f"commit {commit[:12]} is not a commit in this repository — "
                "the pin cannot be verified against any code"
            )

    # Tag shape: registry/path:cr<branch>-<shortsha>
    if image and commit:
        tag = image.rsplit(":", 1)[-1]
        m = re.search(r"-([0-9a-f]{7,40})$", tag)
        if not m:
            problems.append(
                f"image tag {tag!r} carries no commit-shaped suffix; cannot be "
                "cross-checked against the recorded commit"
            )
        elif not commit.startswith(m.group(1)):
            problems.append(
                f"image tag names {m.group(1)} but commit says {commit[:12]} — "
                "the record was assembled from two different builds"
            )

    # The workflows must prefer this file over the out-of-git variable.
    for wf in CI_WORKFLOWS:
        if not wf.exists():
            continue
        if "guest-release.json" not in wf.read_text():
            problems.append(
                f".github/workflows/{wf.name} never reads build/guest-release.json"
                " — it resolves the worker image from a repo variable that lives"
                " outside git, which is how the pin went 18 days stale unnoticed"
            )

    if problems:
        for p in problems:
            fail(p)
        return 1

    print(
        f"guest-release-lint: clean (pin {image.rsplit(':', 1)[-1]} "
        f"= commit {commit[:12]})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

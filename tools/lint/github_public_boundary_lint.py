#!/usr/bin/env python3
"""Keep private worker images and credentials out of public GitHub jobs.

Chromeless integration jobs use a tree-pinned image in Triform's private
registry. Forgejo owns that integration lane and has the scoped pull secret.
GitHub is public and intentionally does not. Image-dependent jobs may therefore
run on GitHub only when the repository owner explicitly configures a public
``CHROMELESS_IMAGE`` override.
"""

from __future__ import annotations

from pathlib import Path
import sys


PUBLIC_IMAGE_GUARD = (
    "github.server_url != 'https://github.com' || vars.CHROMELESS_IMAGE != ''"
)

REQUIRED_JOBS = {
    Path(".github/workflows/ci.yml"): ("smoke",),
    Path(".github/workflows/e2e.yml"): ("e2e",),
    Path(".github/workflows/native-peer-gate.yml"): (
        "native-peer-gate-scaffold",
        "native-peer-gate-strict",
    ),
}


def job_blocks(text: str) -> dict[str, str]:
    """Return top-level workflow job blocks using Actions' two-space layout."""
    blocks: dict[str, list[str]] = {}
    current: str | None = None
    for line in text.splitlines():
        if line.startswith("  ") and not line.startswith("    ") and line.endswith(":"):
            current = line.strip()[:-1]
            blocks[current] = [line]
        elif current is not None:
            blocks[current].append(line)
    return {name: "\n".join(lines) for name, lines in blocks.items()}


def findings_for(path: Path, text: str, required_jobs: tuple[str, ...]) -> list[str]:
    blocks = job_blocks(text)
    findings: list[str] = []
    for job in required_jobs:
        block = blocks.get(job)
        if block is None:
            findings.append(f"{path}: missing required job {job!r}")
        elif PUBLIC_IMAGE_GUARD not in block:
            findings.append(
                f"{path}: job {job!r} can consume the private worker image "
                "on public GitHub"
            )
    return findings


def main() -> int:
    findings: list[str] = []
    for path, jobs in REQUIRED_JOBS.items():
        findings.extend(findings_for(path, path.read_text(encoding="utf-8"), jobs))
    if findings:
        print("github-public-boundary-lint: FAILED", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print("github-public-boundary-lint: clean (4 private-image jobs guarded)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

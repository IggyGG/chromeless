#!/usr/bin/env python3
"""Self-tests for github_public_boundary_lint.py."""

from pathlib import Path

from github_public_boundary_lint import PUBLIC_IMAGE_GUARD, findings_for


PATH = Path(".github/workflows/example.yml")


def main() -> None:
    safe = f"""jobs:
  e2e:
    name: e2e
    if: {PUBLIC_IMAGE_GUARD}
    runs-on: ubuntu-latest
"""
    assert findings_for(PATH, safe, ("e2e",)) == []

    unsafe = """jobs:
  e2e:
    name: e2e
    runs-on: ubuntu-latest
"""
    assert findings_for(PATH, unsafe, ("e2e",)) == [
        f"{PATH}: job 'e2e' can consume the private worker image on public GitHub"
    ]

    assert findings_for(PATH, "jobs:\n", ("e2e",)) == [
        f"{PATH}: missing required job 'e2e'"
    ]


if __name__ == "__main__":
    main()

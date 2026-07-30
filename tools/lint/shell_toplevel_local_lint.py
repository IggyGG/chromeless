#!/usr/bin/env python3
"""shell_toplevel_local_lint.py — catch `local` used outside a function.

bash makes `local` a hard error at script top level:

    bash: line 1: local: can only be used in a function

Under `set -e` that aborts immediately, and — this is the part that cost
real time — it produces NO output of its own. A step logs its START banner
and then simply stops, which reads like an infrastructure kill rather than
a shell bug.

Found 2026-07-30: build/chromeless-build.sh STEP 7 was rewritten to derive
its test-binary list, using `local -a test_binaries=()` inside an `if`
block at top level. `bash -n` does NOT catch it (it is a runtime error, not
a syntax error), so the script passed every local check and died 27 minutes
into a t7 build — after a clean 745-second compile. The feedback loop for a
four-character mistake was a quarter of an hour of Chromium.

This lint is the cheap version of that feedback: it runs in milliseconds
and fails on the same condition.

Exit status: 0 clean, 1 findings.
"""

from __future__ import annotations

import argparse
import glob
import re
import sys

# `name() {`  or  `function name {`  — the two forms bash accepts.
FUNC_RE = re.compile(r"^\s*(?:function\s+\w+|\w+\s*\(\s*\))\s*(?:\{|$)")
LOCAL_RE = re.compile(r"^\s*local\s")

# A heredoc body is not code we can brace-count through, and `local` inside
# one is just text. Track the delimiter so we can skip those lines.
HEREDOC_RE = re.compile(r"<<-?\s*['\"]?([A-Za-z_][A-Za-z0-9_]*)['\"]?")


def scan(path: str) -> list[tuple[int, str]]:
    """Return [(lineno, text)] for every top-level `local`."""
    findings: list[tuple[int, str]] = []
    depth = 0            # brace depth
    func_depth: list[int] = []   # depth at which each open function started
    heredoc: str | None = None

    with open(path, encoding="utf-8", errors="replace") as fh:
        for n, line in enumerate(fh, 1):
            if heredoc is not None:
                if line.strip() == heredoc:
                    heredoc = None
                continue

            stripped = line.strip()
            if stripped.startswith("#"):
                continue

            if FUNC_RE.match(line):
                func_depth.append(depth)

            if LOCAL_RE.match(line) and not func_depth:
                findings.append((n, stripped))

            # Count braces AFTER the local check so a one-line function body
            # (`f() { local x=1; }`) is still attributed to the function.
            m = HEREDOC_RE.search(line)
            opens = line.count("{")
            closes = line.count("}")
            depth += opens - closes
            while func_depth and depth <= func_depth[-1] and closes:
                func_depth.pop()

            if m:
                heredoc = m.group(1)

    return findings


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("paths", nargs="*", default=None,
                    help="shell scripts to check (default: build/ + infra/lifecycle/)")
    args = ap.parse_args()

    paths = args.paths or sorted(
        glob.glob("build/**/*.sh", recursive=True)
        + glob.glob("infra/lifecycle/**/*.sh", recursive=True)
        + glob.glob("tests/**/*.sh", recursive=True)
    )

    total = 0
    for p in paths:
        for lineno, text in scan(p):
            total += 1
            print(f"  {p}:{lineno}: `local` outside a function — "
                  f"bash aborts here at RUNTIME with no output\n"
                  f"      {text}")

    if total:
        print(f"\nshell-toplevel-local-lint: {total} finding(s)\n")
        print("`local` is only valid inside a function. At top level bash errors")
        print("with \"local: can only be used in a function\" — and `bash -n` does")
        print("NOT catch it, because it is a runtime error, not a syntax error.")
        print("Drop the keyword: top-level variables are already global.")
        return 1

    print(f"shell-toplevel-local-lint: clean ({len(paths)} file(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())

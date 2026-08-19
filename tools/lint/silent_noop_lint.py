#!/usr/bin/env python3
"""silent_noop_lint.py — catch checks whose FAILURE is indistinguishable from success.

Two mechanical forms, both of which shipped and stayed green while doing
nothing. Neither is a style preference; each has a dated incident.

FORM 1 — `if ! cmd; then rc=$?`
--------------------------------
Inside the branch, `$?` is the status of the NEGATED pipeline, which is
always 0. So a step that means "capture the failure code and re-raise it"
exits 0 and reports a FAILED stack as a GREEN run.

    if ! docker compose up -d --wait; then
        rc=$?          # <-- always 0
        exit "$rc"     # <-- green
    fi

Verified in a shell 2026-08-19: the buggy form captured 0, the correct form
captured 254. Write it as `rc=0; cmd || rc=$?` instead.

FORM 2 — a guard keyed to LINE NUMBERS in a file it does not own
----------------------------------------------------------------
`sed -n '1000,1140p' some/other/file.yaml | grep -qF marker` asserts a
contract by coordinates. The file grows, the marker moves outside the
window, and the guard goes RED while the contract it checks is satisfied.

Measured 2026-08-19: tf-multiverse's cargoless lockstep guard did exactly
this — serve-witness.yaml reached 2060 lines, the marker moved to 1174, and
the guard failed 58 times across 45 distinct PRs for a defect that did not
exist. A guard that cries wolf trains everyone to ignore the check that
would have caught the real thing, so this fails as a FALSE ALARM — the
expensive direction.

Anchor on structure (a name, a `kind:`, a document separator) instead.

Scope: shell scripts and workflow YAML in this repo. Skips .claude/worktrees
(full checkouts; see CLAUDE.md).

Exit status: 0 clean, 1 findings.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# `if ! <cmd>; then` ... `$?` before any other command substitution reset.
NEGATED_IF = re.compile(r"^\s*if\s*!\s+.+;\s*then\s*$")
CAPTURES_RC = re.compile(r"\$\?")

# sed -n 'N,Mp' / head -N | tail / awk 'NR>=N && NR<=M' against a path.
LINE_WINDOW = re.compile(
    r"""(sed\s+-n\s+['"]?\d{2,},\s*\d{2,}p"""
    r"""|awk\s+['"]NR\s*[><=]=?\s*\d{2,}\s*&&\s*NR\s*[><=]=?\s*\d{2,})"""
)

SKIP_DIRS = {".git", "node_modules", "worktrees", "dist", "out", "vendor"}
EXTS = (".sh", ".bash", ".yml", ".yaml")


def _walk(root: str):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            if fn.endswith(EXTS):
                yield os.path.join(dirpath, fn)


def _is_comment(line: str) -> bool:
    return line.lstrip().startswith("#")


def scan(path: str) -> list[tuple[int, str, str]]:
    """Return (lineno, form, evidence)."""
    out: list[tuple[int, str, str]] = []
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return out

    for i, line in enumerate(lines):
        if _is_comment(line):
            continue

        # FORM 1: `$?` read anywhere inside an `if ! ...; then` branch.
        #
        # Scan to the branch's own `fi`/`else`, NOT a fixed lookahead. A
        # 6-line window was the first version and it MISSED the real case:
        # in a workflow `run: |` block the dump loop between `then` and
        # `exit` is ten lines long, so the bug sat outside the window and the
        # lint reported clean. Bound by structure, not by a guessed distance
        # — the same mistake this lint's second form exists to catch.
        if NEGATED_IF.match(line):
            base_indent = len(line) - len(line.lstrip())
            for j in range(i + 1, len(lines)):
                nxt = lines[j]
                stripped = nxt.strip()
                if not stripped:
                    continue
                indent = len(nxt) - len(nxt.lstrip())
                # Branch ends at fi/else/elif at or left of the `if` column.
                if indent <= base_indent and re.match(
                        r"^(fi|else|elif)\b", stripped):
                    break
                # Or when we dedent out of the block entirely (YAML `run: |`).
                if indent < base_indent and stripped:
                    break
                if not _is_comment(nxt) and CAPTURES_RC.search(nxt):
                    out.append(
                        (j + 1, "negated-if-rc",
                         "`$?` inside `if ! cmd; then` is always 0 — "
                         "use `rc=0; cmd || rc=$?`")
                    )
                    break

        # FORM 2: a hardcoded line window used as an assertion.
        if LINE_WINDOW.search(line) and re.search(r"grep|test|\[\[|\[ ", line):
            out.append(
                (i + 1, "line-window-guard",
                 "assertion keyed to line numbers goes stale as the file "
                 "grows — anchor on structure")
            )

    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("roots", nargs="*", default=["."], help="paths to scan")
    args = ap.parse_args()

    findings = []
    for root in args.roots:
        for path in _walk(root):
            for lineno, form, why in scan(path):
                findings.append((path, lineno, form, why))

    if not findings:
        print("silent-noop-lint: clean (no false-green rc idioms, "
              "no line-number-keyed assertions)")
        return 0

    for path, lineno, form, why in sorted(findings):
        rel = os.path.relpath(path)
        print(f"{rel}:{lineno}: [{form}] {why}", file=sys.stderr)
    print(f"\nsilent-noop-lint: {len(findings)} finding(s)", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())

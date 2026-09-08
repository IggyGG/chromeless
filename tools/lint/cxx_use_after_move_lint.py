#!/usr/bin/env python3
"""Fail when a value is moved and then read in the SAME argument list.

WHY THIS EXISTS
---------------
On 2026-09-08 this line cost five image rolls:

    self->OnFinalised(std::move(id), std::move(p),
                      std::move(result.first), result.second,
                      !result.first.empty());          // <-- argument 5

`result.first` is moved into argument 3 and read in argument 5. **C++ leaves
argument evaluation order unspecified**, so which happens first is a
toolchain decision. Here argument 3 won: `ok` was computed from a moved-from
(empty) string, and every file upload reported failure while the file sat
correct and complete on disk.

It compiled clean and linked clean. The symptom was four layers away — the
caller reported "could not read the file back", about a read that the logs
eventually proved had never failed once — so the diagnosis went through the
filesystem, the task runner, and a genuine end/write race before reaching the
line itself.

WHAT COUNTS
-----------
Only the UNSEQUENCED case: a move and a read of the same name inside one
parenthesised argument list, with no `;` and no `{` between them. Those
boundaries matter, and an earlier version of this check that ignored them
produced 14 findings, ALL false:

    auto drained = std::move(pending_);  pending_.clear();     // legal
    [pc = std::move(pc_)]() {...};       pc_ = nullptr;        // legal

A move is sequenced before the next statement, so reading (or clearing, or
reassigning) a moved-from object there is deliberate and correct. Only
arguments to a single call are unordered relative to each other.

Checked against the whole tree when added: one finding, which is the bug
above; zero after the fix. Negative control reintroduces it and this fails.
"""

from __future__ import annotations

import pathlib
import re
import sys

MOVE = re.compile(r"std::move\(\s*([A-Za-z_][\w.]*)\s*\)")


def argument_lists(src: str):
    """Yield (offset, text) for every parenthesised call argument list."""
    i = 0
    while True:
        m = re.search(r"[A-Za-z_]\w*\s*\(", src[i:])
        if not m:
            return
        start = i + m.end() - 1
        depth, j = 0, start
        while j < len(src):
            if src[j] == "(":
                depth += 1
            elif src[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        yield start, src[start:j + 1]
        i = start + 1


def scan(path: pathlib.Path) -> list[str]:
    src = re.sub(r"//[^\n]*", "", path.read_text(errors="ignore"))
    out: list[str] = []
    for off, expr in argument_lists(src):
        # A statement or block boundary means the move IS sequenced.
        if ";" in expr or "{" in expr:
            continue
        for name in MOVE.findall(expr):
            tail = expr.split(f"std::move({name})", 1)[-1]
            # \b on both sides: `chunk_bytes` is not a read of `bytes`.
            if re.search(rf"(?<!std::move\()\b{re.escape(name)}\b", tail):
                line = src[:off].count("\n") + 1
                out.append(
                    f"{path}:{line}: `{name}` is moved and then read in the "
                    f"same argument list — argument evaluation order is "
                    f"unspecified, so the read may see a moved-from value")
    return out


def main(argv: list[str]) -> int:
    root = pathlib.Path(argv[1]) if len(argv) > 1 else pathlib.Path("capture")
    findings: list[str] = []
    files = 0
    for path in sorted(root.rglob("*.cc")):
        if any(p in (".claude", "node_modules") for p in path.parts):
            continue
        files += 1
        findings.extend(scan(path))

    if findings:
        print("cxx-use-after-move-lint: FAIL")
        for f in sorted(set(findings)):
            print(f"  {f}")
        return 1
    print(f"cxx-use-after-move-lint: clean ({files} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

#!/usr/bin/env python3
"""Fail when a class method is declared in a .h and defined in no .cc.

WHY THIS EXISTS
---------------
On 2026-09-08 a scripted edit to `cb_file_upload_receiver.cc` replaced a
range that ran from one function to the next, and silently took TWO other
function bodies with it. `OnFinalised` and `ResolveChooserWith` were still
declared, still called, and defined nowhere.

`make lint-cxx` was clean (it checks includes), `make verify` was clean
(it cannot compile `capture/`), and the defect surfaced ~13 minutes into
the build lane as:

    ld.lld: error: undefined symbol:
      cloud_browser::CbFileUploadReceiver::OnFinalised(...)

That is a full lane cycle to learn something a script can check in
milliseconds. The linker sees it because a CALL exists; this lint is
strictly stronger — it also catches a method that is declared, defined
nowhere, and not yet called, which links fine today and breaks whoever
adds the first caller.

WHAT IT DOES NOT DO
-------------------
It is a text scan, not a parser. It deliberately only considers methods
whose declaration it can recognise unambiguously, and skips:

  * anything defined inline in the header (`... { ... }` on the decl)
  * pure virtuals and deleted functions
  * `= default` and the compiler-generated shapes
  * templates and macros

so it under-reports rather than inventing work. A false positive costs
more trust than a missed defect costs time (CLAUDE.md), and this ran
clean across all of `capture/` when it was added.
"""

from __future__ import annotations

import pathlib
import re
import sys

# A method declaration inside a class body: leading whitespace, an optional
# `virtual`, a return type, the name, an open paren. Captures the name.
DECL = re.compile(
    r"^[ \t]+(?!return\b|delete\b|else\b)"          # indented, not a statement
    r"(?:virtual\s+)?"
    r"(?:const\s+)?"
    r"[A-Za-z_][\w:<>,\s&*\[\]]*?"                  # return type
    r"\s[*&]?\s*"
    r"([A-Za-z_]\w*)\s*\("                          # NAME (
)

# Things that are not out-of-line definitions.
INLINE_BODY = re.compile(r"\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?\{")
PURE_OR_DELETED = re.compile(r"\)\s*(?:const\s*)?(?:override\s*)?=\s*(?:0|delete|default)")


def class_name_of(header: pathlib.Path) -> list[str]:
    """Every class/struct declared in the header."""
    text = header.read_text(errors="ignore")
    return re.findall(r"^(?:class|struct)\s+(?:\w+_EXPORT\s+)?(\w+)\s*(?::|\{)",
                      text, re.M)


def declared_methods(header: pathlib.Path) -> set[str]:
    out: set[str] = set()
    for raw in header.read_text(errors="ignore").splitlines():
        line = raw.split("//", 1)[0]
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if INLINE_BODY.search(line) or PURE_OR_DELETED.search(line):
            continue
        if "template" in line or "friend" in line or "typedef" in line:
            continue
        m = DECL.match(line)
        if not m:
            continue
        name = m.group(1)
        # Constructors/destructors and control keywords.
        if name in {"if", "for", "while", "switch", "return", "sizeof",
                    "explicit", "operator"}:
            continue
        # Thread-safety annotation macros trail a member declaration and look
        # exactly like a call: `int n_ RTC_GUARDED_BY(lock_);`. Both spellings
        # are present in this tree (webrtc's and absl's).
        if name.endswith(("_GUARDED_BY", "_LOCKS_REQUIRED", "_ACQUIRED_AFTER",
                          "_ACQUIRED_BEFORE", "_PT_GUARDED_BY")):
            continue
        out.add(name)
    return out


def main(argv: list[str]) -> int:
    root = pathlib.Path(argv[1]) if len(argv) > 1 else pathlib.Path("capture")
    findings: list[str] = []
    checked = 0

    for header in sorted(root.rglob("*.h")):
        if any(p in (".claude", "node_modules") for p in header.parts):
            continue
        classes = class_name_of(header)
        if not classes:
            continue
        sibling = header.with_suffix(".cc")
        if not sibling.exists():
            continue                       # header-only; nothing to check
        body = sibling.read_text(errors="ignore")
        checked += 1
        for name in sorted(declared_methods(header)):
            if name in classes:            # constructor
                continue
            if any(f"{cls}::{name}(" in body for cls in classes):
                continue
            # Some classes define members in a DIFFERENT .cc; only flag when
            # the name appears nowhere as a qualified definition in the tree.
            if any(f"::{name}(" in p.read_text(errors="ignore")
                   for p in root.rglob("*.cc")):
                continue
            findings.append(
                f"{header.relative_to(root.parent)}: {classes[0]}::{name} is "
                f"declared and defined nowhere — the linker will only notice "
                f"once something calls it")

    if findings:
        print("cxx-orphan-decl-lint: FAIL")
        for f in findings:
            print(f"  {f}")
        return 1
    print(f"cxx-orphan-decl-lint: clean ({checked} header/impl pairs)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

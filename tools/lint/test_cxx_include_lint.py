#!/usr/bin/env python3
"""Self-test for cxx_include_lint.py.

A linter nobody trusts gets ignored, and a linter with false positives gets
disabled. This pins both directions:

  * it MUST flag the real defect it was built for (the Cb.shutdown bug:
    base::BindOnce + FROM_HERE + GetUIThreadTaskRunner with none of their
    headers), and
  * it MUST NOT flag the transitive-include patterns this tree legitimately
    relies on, or mentions inside comments and string literals.

Run: python3 tools/lint/test_cxx_include_lint.py
"""

from __future__ import annotations

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cxx_include_lint as lint  # noqa: E402

FAILURES: list[str] = []


def check(name: str, source: str, expect_symbols: set[str]) -> None:
    """Write `source` to a temp .cc and assert which symbols are reported."""
    with tempfile.TemporaryDirectory() as td:
        path = os.path.join(td, "t.cc")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(source)
        found = lint.check_file(path)

    got = {sym for _, _, sym in lint.RULES
           if any(f"uses {sym} " in line for line in found)}
    if got != expect_symbols:
        FAILURES.append(
            f"{name}\n     expected: {sorted(expect_symbols) or '(none)'}"
            f"\n     got:      {sorted(got) or '(none)'}")
        return
    print(f"  ok   {name}")


# --- the regression that motivated the tool -------------------------------
check(
    "catches the Cb.shutdown defect (BindOnce + FROM_HERE + UI task runner)",
    """
#include "base/logging.h"
void Foo() {
  content::GetUIThreadTaskRunner({})->PostTask(FROM_HERE, base::BindOnce([]{}));
}
""",
    {"base::BindOnce", "FROM_HERE", "content::GetUIThreadTaskRunner"},
)

check(
    "clean when every header is present",
    """
#include "base/functional/bind.h"
#include "base/location.h"
#include "content/public/browser/browser_thread.h"
void Foo() {
  content::GetUIThreadTaskRunner({})->PostTask(FROM_HERE, base::BindOnce([]{}));
}
""",
    set(),
)

# --- false-positive guards ------------------------------------------------
check(
    "ignores symbols named only in comments",
    """
// This talks about base::BindOnce and FROM_HERE and base::JSONReader.
/* base::JSONWriter too, in a block comment. */
void Foo() {}
""",
    set(),
)

check(
    "ignores symbols inside string literals",
    """
#include "base/logging.h"
void Foo() { LOG(INFO) << "call base::BindOnce with FROM_HERE"; }
""",
    set(),
)

check(
    "accepts FROM_HERE via the sequenced_task_runner umbrella",
    """
#include "base/task/sequenced_task_runner.h"
void Foo() { runner->PostTask(FROM_HERE, cb); }
""",
    set(),
)

check(
    "accepts FROM_HERE via base/timer/timer.h (cb_signaling_reconnect.cc)",
    """
#include "base/timer/timer.h"
void Foo() { timer_.Start(FROM_HERE, delay, cb); }
""",
    set(),
)

check(
    "accepts callback.h as satisfying the binders",
    """
#include "base/functional/callback.h"
void Foo() { auto c = base::BindRepeating([]{}); }
""",
    set(),
)

# --- line-number accuracy -------------------------------------------------
# An early version stripped comments by deleting them, which shifted every
# reported line by the number of comment lines above it.
with tempfile.TemporaryDirectory() as td:
    p = os.path.join(td, "lines.cc")
    with open(p, "w", encoding="utf-8") as fh:
        fh.write("// c1\n// c2\n/* c3\n   c4 */\n#include \"base/logging.h\"\n"
                 "void F() { auto x = base::JSONReader::Read(s); }\n")
    out = lint.check_file(p)
    # base::JSONReader sits on line 6, below 4 lines of comment. A linter that
    # deletes comments instead of blanking them reports line 2.
    if len(out) == 1 and f"{p}:6:" in out[0]:
        print("  ok   reports the true line number (comments preserved)")
    else:
        FAILURES.append(f"line-number accuracy: expected exactly one finding "
                        f"at line 6\n     got: {out}")

# --- the real tree stays clean -------------------------------------------
repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
capture = os.path.join(repo, "capture")
if os.path.isdir(capture):
    tree = [f for src in lint.iter_sources([capture]) for f in lint.check_file(src)]
    if tree:
        FAILURES.append("capture/ is not clean:\n     " + "\n     ".join(tree[:10]))
    else:
        print("  ok   capture/ is clean")

print()
if FAILURES:
    print(f"{len(FAILURES)} FAILURE(S):\n")
    for f in FAILURES:
        print(f"  - {f}")
    sys.exit(1)
print("all self-tests passed")

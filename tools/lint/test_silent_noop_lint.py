#!/usr/bin/env python3
"""test_silent_noop_lint.py — prove the lint fires on the REAL defects.

A lint that has never fired is indistinguishable from one that cannot fire
— which is the very failure mode it exists to catch, so it would be absurd
to ship it unverified. Both positives below are the actual code that
shipped; both negatives are the corrected form.

Run: python3 tools/lint/test_silent_noop_lint.py
"""

from __future__ import annotations

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import silent_noop_lint as lint  # noqa: E402

# The shape that shipped in chromeless e2e.yml before review caught it:
# `$?` inside `if ! ...` is 0, so the step exits 0 on failure.
BAD_RC = """#!/usr/bin/env bash
set -euo pipefail
if ! docker compose -f infra/compose.yaml up -d --wait; then
    rc=$?
    echo "compose up failed (rc=${rc})"
    exit "${rc}"
fi
"""

# The corrected form actually committed.
GOOD_RC = """#!/usr/bin/env bash
set -euo pipefail
rc=0
docker compose -f infra/compose.yaml up -d --wait || rc=$?
if [ "${rc}" -ne 0 ]; then
    echo "compose up failed (rc=${rc})"
    exit "${rc}"
fi
"""

# tf-multiverse's cargoless lockstep guard: 58 failures / 45 PRs for a
# defect that did not exist, because serve-witness.yaml grew past line 1140.
BAD_WINDOW = """#!/usr/bin/env bash
check "witness B is stable-policy managed" \\
    sh -c "sed -n '1000,1140p' '$ROOT/serve-witness.yaml' | grep -qF 'marker'"
"""

# The structural anchor that replaced it.
GOOD_WINDOW = """#!/usr/bin/env bash
check "witness B is stable-policy managed" \\
    sh -c "awk '/^  name: cargoless-serve-witness-b$/{d=1} d&&/^---$/{exit} d' \\
        '$ROOT/serve-witness.yaml' | grep -qF 'marker'"
"""

# A comment describing the bug must NOT trip the lint — CLAUDE.md and the
# lint's own docstring quote both forms verbatim.
COMMENTED = """#!/usr/bin/env bash
# NB: `if ! cmd; then rc=$?` reads the negated pipeline's status.
# Do not use `sed -n '1000,1140p' file | grep -qF marker` as an assertion.
rc=0
cmd || rc=$?
"""

CASES = [
    ("false-green rc idiom", BAD_RC, "negated-if-rc", True),
    ("corrected rc capture", GOOD_RC, "negated-if-rc", False),
    ("line-window guard", BAD_WINDOW, "line-window-guard", True),
    ("structural anchor", GOOD_WINDOW, "line-window-guard", False),
    ("both forms in comments", COMMENTED, None, False),
]


def main() -> int:
    failures = 0
    with tempfile.TemporaryDirectory() as td:
        for name, body, form, should_fire in CASES:
            path = os.path.join(td, f"{abs(hash(name))}.sh")
            with open(path, "w") as fh:
                fh.write(body)
            found = lint.scan(path)
            forms = {f for _, f, _ in found}
            fired = (form in forms) if form else bool(found)
            ok = fired == should_fire
            failures += 0 if ok else 1
            verdict = "ok  " if ok else "FAIL"
            expect = "fires" if should_fire else "silent"
            print(f"  {verdict} {name:<28} expected {expect}, got "
                  f"{'fires' if fired else 'silent'}")

    if failures:
        print(f"\ntest-silent-noop-lint: {failures} case(s) FAILED",
              file=sys.stderr)
        return 1
    print(f"\ntest-silent-noop-lint: all {len(CASES)} cases pass "
          "(both arms — it fires on the real defects and stays silent on "
          "their fixes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

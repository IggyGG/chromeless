#!/usr/bin/env python3
"""Every test entrypoint under tests/ must be reachable from something.

WHY THIS EXISTS
---------------
`tests/` accumulated 34 files that no make target and no workflow invoked:
27 under `tests/webrtc/`, plus `tests/cdp/`, `tests/rendering/`, and
`tests/runtime/`. They were written, reviewed, merged — and then run by nothing.

That is not hypothetical rot. When they were finally executed:

  * `tests/runtime/streamer-signaling-url-contract.sh` had been RED for months.
    It asserted a pre-M7 `token=<jwt>` URL parameter; the launcher moved the
    token to an env var. It also read `capture/streamer-page/`, deleted in M7.
    A red test that nothing runs is indistinguishable from no test.
  * `tests/webrtc/encoder-assertions.test.mjs` passed 13/13 in 1.3 s with zero
    dependencies — free signal nobody was collecting, guarding a regression
    this repo had ALREADY shipped (every image profile=sw for two months).

This is the same defect as unbuilt `test()` targets, one directory over, and
`build_targets_lint.py` is the precedent: it was written after three test
targets went uncompiled for ten weeks, and it caught a live regression within
hours. This lint closes the sibling hole so the orphan pile cannot regrow.

ENTRYPOINT vs LIBRARY — why this is not just "list every file"
---------------------------------------------------------------
Most files under `tests/` are not independently runnable. `scenarios/_lib.mjs`
is 1226 lines imported by 13 scenarios; fixtures are data; `pod-templates/` are
manifests. Flagging those would be noise, and CLAUDE.md is explicit that a false
positive costs more trust than a missed defect costs time.

So a file is an ORPHAN only when BOTH hold:
  1. no make target, workflow, or script names it, AND
  2. no other file under tests/ references it by name.

A library is referenced by its consumers. An orphan entrypoint is referenced by
nobody. That distinction is what makes this checkable without a hand-maintained
allowlist — the same "derive it, never hand-maintain it" property that makes
`build_targets_lint.py` hold up.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
TESTS = REPO / "tests"

# Files that can plausibly be run on their own.
ENTRYPOINT_SUFFIXES = (".sh", ".mjs", ".py", ".spec.ts", ".test.ts", ".test.mjs")

# Directories whose contents are never entrypoints.
SKIP_DIRS = {
    "node_modules",
    "scratch",       # gitignored ad-hoc drivers, per CLAUDE.md
    "__pycache__",
    "fixtures",
    "pod-templates",
    "test-results",
    "artifacts",
    "baselines",
    "sample-input",
    "dist",
}

# Where an invocation could come from.
INVOKER_GLOBS = [
    "Makefile",
    ".github/workflows/*.yml",
    ".github/workflows/*.yaml",
    "build/*.sh",
    "infra/k8s/**/*.yaml",
    "tests/*.sh",
    "tests/**/README.md",
    # k8s pod templates are a REAL invoker surface: cv2-*.yaml.tpl name the
    # .mjs driver they run in-cluster. The first version of this lint missed
    # them (pod-templates/ is in SKIP_DIRS as a non-entrypoint dir) and flagged
    # phase-a-m5-r1-dom-probe.mjs, which cv2-phase-a-m5-r1-dom-probe.yaml.tpl
    # genuinely invokes. Being a non-entrypoint and being a non-invoker are
    # different properties; this dir is the former, not the latter.
    "tests/**/*.tpl",
    "package.json",
    "tests/**/package.json",
]


def is_skipped(path: Path) -> bool:
    return any(part in SKIP_DIRS for part in path.parts)


def candidate_files() -> list[Path]:
    out = []
    for p in sorted(TESTS.rglob("*")):
        if not p.is_file() or is_skipped(p.relative_to(REPO)):
            continue
        if p.name.endswith(ENTRYPOINT_SUFFIXES):
            out.append(p)
    return out


def read_invoker_text() -> str:
    """Everything that could name a test, concatenated.

    Deliberately NOT filtered through is_skipped(). Those two checks answer
    different questions and conflating them is a bug I shipped in the first
    draft: `pod-templates/` is in SKIP_DIRS because nothing in it is an
    ENTRYPOINT, but its `.tpl` files genuinely INVOKE drivers in-cluster.
    Filtering invokers by the entrypoint skip-list silently discarded them and
    the lint reported a file as unreachable while a template ran it.
    """
    chunks = []
    for pattern in INVOKER_GLOBS:
        for f in REPO.glob(pattern):
            if f.is_file():
                try:
                    chunks.append(f.read_text(errors="ignore"))
                except OSError:
                    pass
    return "\n".join(chunks)


def read_intra_test_text(exclude: Path) -> str:
    """Every OTHER file under tests/, for the library-reference check."""
    chunks = []
    for p in TESTS.rglob("*"):
        if not p.is_file() or p == exclude:
            continue
        if is_skipped(p.relative_to(REPO)):
            continue
        if p.suffix in {".mjs", ".js", ".ts", ".sh", ".py", ".json", ".md", ".yaml", ".yml"}:
            try:
                chunks.append(p.read_text(errors="ignore"))
            except OSError:
                pass
    return "\n".join(chunks)


def main() -> int:
    if not TESTS.is_dir():
        print("tests-wired-lint: no tests/ directory — nothing to check.")
        return 0

    files = candidate_files()
    if not files:
        print("tests-wired-lint: found no candidate test files.")
        print("  That is almost certainly a bug in this lint, not in the tree.")
        return 1

    invoker_text = read_invoker_text()
    # A glob that names a whole directory counts as invoking everything in it —
    # `for f in tests/runtime/*.sh` is a real invocation, not a near-miss.
    dir_globs = set(re.findall(r"tests/[\w/-]+/\*+\.\w+", invoker_text))

    # RUNTIME DISCOVERY. A runner that does readdirSync(HERE).filter(endsWith
    # ".scenario.mjs") really does invoke every matching sibling — but no static
    # reference to those files exists anywhere, so a naive check flags all of
    # them. The first run of this lint did exactly that: it reported the three
    # wire scenarios (20/21/22) as orphans when scenarios/runner.mjs:61 walks
    # the directory and runs them.
    #
    # False positives are the failure mode that gets a lint deleted (CLAUDE.md:
    # "a false positive costs more trust than a missed defect costs time"), so
    # honour discovery: if any file names a suffix pattern, everything matching
    # that suffix in the same tree is reachable.
    discovered_suffixes: set[str] = set()
    for p in TESTS.rglob("*"):
        if not p.is_file() or is_skipped(p.relative_to(REPO)):
            continue
        if p.suffix not in {".mjs", ".js", ".ts", ".sh", ".py"}:
            continue
        try:
            text = p.read_text(errors="ignore")
        except OSError:
            continue
        if "readdir" not in text and "glob" not in text and "find " not in text:
            continue
        for suf in re.findall(r'endsWith\(\s*[\'"]([^\'"]+)[\'"]', text):
            discovered_suffixes.add(suf)
        for suf in re.findall(r'\*(\.[\w.]+)', text):
            discovered_suffixes.add(suf)

    orphans = []
    for f in files:
        rel = f.relative_to(REPO)
        name = f.name

        if name in invoker_text or str(rel) in invoker_text:
            continue
        # A directory glob covers a file only when the glob's DIRECTORY is this
        # file's directory AND its extension matches. The first draft tested
        # `str(rel.parent) in g`, a substring match, so "tests/webrtc" matched
        # "tests/webrtc/pod-templates/*.yaml" — a different directory and a
        # different extension — and every .mjs in tests/webrtc/ was silently
        # treated as covered. That is the failure mode this lint exists to
        # catch, reproduced inside the lint itself.
        if any(
            str(rel.parent) == g.rsplit("/", 1)[0]
            and name.endswith(g.rsplit("*", 1)[-1])
            for g in dir_globs
        ):
            continue
        if any(name.endswith(suf) for suf in discovered_suffixes):
            continue
        # Referenced by a sibling => it is a library, not an orphan entrypoint.
        if name in read_intra_test_text(exclude=f):
            continue
        orphans.append(rel)

    if orphans:
        print("tests-wired-lint: test files that NOTHING invokes:")
        for rel in orphans:
            print(f"  {rel}")
        print()
        print("  Each is either a test nobody runs (so its result is unknown —")
        print("  it may have been failing for months) or dead code. Both are")
        print("  defects. Wire it into a make target, or delete it and say why")
        print("  in the commit message.")
        print()
        print("  A red test that nothing runs is indistinguishable from no test.")
        return 1

    print(f"tests-wired-lint: {len(files)} test entrypoint(s), all reachable.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

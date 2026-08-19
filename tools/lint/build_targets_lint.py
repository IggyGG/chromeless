#!/usr/bin/env python3
"""Every test() target declared in capture/ must be built by some build lane.

WHY THIS EXISTS
---------------
capture/**/BUILD.gn declares seven `test()` targets. The k8s build manifests
in infra/k8s/chromeless-build/ each name an explicit list of ninja targets in
CHROMELESS_BUILD_TARGETS. Nothing connected the two. So a target could be
declared, reviewed, merged, and never compiled by anything — and the build
lane stayed green, because green meant "the targets I was told to build
worked", not "the code in the tree compiles".

Three of the seven were in that state for roughly ten weeks:

    cloud_browser_adm_unittests
    cloud_browser_input_dispatch_unittests
    cloud_browser_pointer_state_unittests

When they were finally added to the x264-t7 lane on 2026-07-30, the very
first compile failed: CbAudioTestRecorder implemented two of AudioTransport's
three pure virtuals, so it was an abstract class and the test could not
declare one. The header's own TODO had predicted precisely that, and had been
sitting there unread because nothing ever forced the question.

This lint makes that impossible to repeat. It is not a compiler and cannot
tell you whether a target builds; it tells you whether anything ever TRIES.

THE TWO DIRECTIONS
------------------
1. Declared but never built — a test() in BUILD.gn named by no manifest.
   This is the ten-week gap. Fatal.

2. Built but not declared — a manifest naming a target that no BUILD.gn
   declares. Same shape as the Cb.* silently-ignored-param class the repo
   has been bitten by twice: the build would fail at gn-gen time with a
   confusing "label not found", hours into a job. Catch it in 40ms instead.
   Fatal.

DELIBERATE ASYMMETRY IS FINE
----------------------------
The rule is "at least ONE lane builds it", not "every lane builds it".
cloud_browser_pcf_unittests is x264-t2-only on purpose, and the encoder tests
are gated on profile capabilities. Requiring uniformity would be wrong and
would generate exactly the false positives CLAUDE.md warns cost more trust
than a missed defect costs time.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent

CAPTURE = REPO / "capture"
MANIFEST_DIR = REPO / "infra" / "k8s" / "chromeless-build"

# test("name") at the start of a line. gn's own formatter puts it there, and
# every declaration in the tree matches; an indented one would be inside a
# template and is not a target we could name on a command line anyway.
TEST_DECL = re.compile(r'^test\("([^"]+)"\)', re.M)

# The env var whose value is the ninja command line. Captures the quoted
# value that follows, tolerating the long comment blocks these manifests
# carry between the name: and value: keys.
TARGETS_ENV = re.compile(
    r"name:\s*CHROMELESS_BUILD_TARGETS\b.*?\bvalue:\s*\"([^\"]*)\"",
    re.S,
)


def declared_test_targets() -> dict[str, Path]:
    """Map test-target name -> the BUILD.gn that declares it."""
    found: dict[str, Path] = {}
    for gn in sorted(CAPTURE.rglob("BUILD.gn")):
        for name in TEST_DECL.findall(gn.read_text()):
            found[name] = gn.relative_to(REPO)
    return found


def manifest_targets() -> dict[str, set[str]]:
    """Map manifest path -> set of bare target names it asks ninja to build.

    gn labels look like //path/to:target_name; we compare on the bare name
    because that is also what STEP 7 of chromeless-build.sh derives when it
    decides which binaries to execute.
    """
    per_manifest: dict[str, set[str]] = {}
    for yml in sorted(MANIFEST_DIR.glob("build-job*.yaml")):
        text = yml.read_text()
        names: set[str] = set()
        # A manifest has an init container and a build container, both
        # carrying the same env. Union them — a divergence between the two
        # is a different bug (and one the manifests comment about), not
        # this lint's business.
        for value in TARGETS_ENV.findall(text):
            for label in value.split():
                names.add(label.rsplit(":", 1)[-1])
        per_manifest[str(yml.relative_to(REPO))] = names
    return per_manifest


def main() -> int:
    declared = declared_test_targets()
    per_manifest = manifest_targets()

    if not declared:
        print("build-targets-lint: found no test() targets under capture/.")
        print("  That is almost certainly a bug in this lint, not in the tree.")
        return 1
    if not per_manifest:
        print(f"build-targets-lint: no build-job*.yaml under {MANIFEST_DIR}.")
        return 1

    built_anywhere: set[str] = set()
    for names in per_manifest.values():
        built_anywhere |= names

    failures = 0

    # Direction 1 — declared but never built.
    orphans = sorted(set(declared) - built_anywhere)
    if orphans:
        failures += len(orphans)
        print("build-targets-lint: test targets that NO build lane compiles:")
        for name in orphans:
            print(f"  {name}")
            print(f"      declared in {declared[name]}")
        print()
        print("  Nothing has ever compiled these. The build lanes are green")
        print("  about a smaller set of files than the tree contains.")
        print("  Add each to CHROMELESS_BUILD_TARGETS in at least one manifest")
        print(f"  under {MANIFEST_DIR.relative_to(REPO)}/ — one lane is enough.")
        print()

    # Direction 2 — built but not declared. Only consider names that look
    # like test targets; the manifests legitimately name non-test targets
    # (cloud_browser_worker, headless:resource_pack_data) that this lint has
    # no opinion about.
    #
    # The predicate is a substring, not an `_unittests` suffix. Self-testing
    # this lint with a deliberately typo'd target found the reason: dropping
    # the trailing "s" (cloud_browser_adm_unittest) is the most likely typo
    # of all, and a suffix match skips exactly that. Direction 1 still failed
    # the run, so the typo was never going to ship silently — but it reported
    # "nothing compiles adm_unittests", sending the reader to look for a
    # missing manifest entry that is in fact sitting right there, misspelled.
    def looks_like_a_test(name: str) -> bool:
        return "unittest" in name or name.endswith("_test") or "_tests" in name

    for manifest, names in sorted(per_manifest.items()):
        ghosts = sorted(
            n for n in names if looks_like_a_test(n) and n not in declared
        )
        if ghosts:
            failures += len(ghosts)
            print(f"build-targets-lint: {manifest} asks ninja to build test")
            print("  targets that no BUILD.gn declares:")
            for name in ghosts:
                print(f"  {name}")
            print()
            print("  gn gen will fail on this, but hours into the job. Either")
            print("  the target was renamed/removed and the manifest was not")
            print("  updated, or the name is a typo.")
            print()

    if failures:
        return 1

    # Report coverage on success — the useful half. "clean" here means every
    # target is built somewhere, which is a weaker claim than most readers
    # will assume, so spell out what each lane actually covers.
    print(f"build-targets-lint: {len(declared)} test target(s), all built by "
          f"at least one lane.")
    for manifest, names in sorted(per_manifest.items()):
        tests = sorted(n for n in names if n in declared)
        short = Path(manifest).name
        print(f"  {short:28s} {len(tests)}/{len(declared)}  "
              f"{' '.join(t.replace('cloud_browser_', 'cb_') for t in tests)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Self-tests for build_targets_lint.py.

A lint that only ever passes is indistinguishable from no lint at all. These
fixtures build the failure states themselves — including a byte-accurate
replay of the real ten-week gap — and assert the lint fails on each.

Run: python3 tools/lint/test_build_targets_lint.py
Wired into `make lint-build-targets`, matching the cxx-include-lint precedent.
"""

from __future__ import annotations

import importlib.util
import io
import re
import sys
import textwrap
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent

spec = importlib.util.spec_from_file_location(
    "build_targets_lint", HERE / "build_targets_lint.py"
)
lint = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lint)


def _write(root: Path, rel: str, body: str) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(body))


def _manifest(targets: str) -> str:
    """A build-job manifest stripped to the shape the lint actually parses.

    Keeps the long comment block between `name:` and `value:` because the
    real manifests have one and an earlier draft of the regex could not see
    past it.
    """
    return f"""\
        apiVersion: batch/v1
        kind: Job
        spec:
          template:
            spec:
              containers:
                - name: build
                  env:
                    - name: CHROMELESS_BUILD_TARGETS
                      # A comment block sits here in every real manifest,
                      # sometimes 20 lines of it, explaining why each target
                      # is or is not in the list. The parser must see past it.
                      value: "{targets}"
                    - name: SOMETHING_ELSE
                      value: "1"
        """


def _run(root: Path) -> tuple[int, str]:
    """Point the lint at a fixture tree and capture its verdict + output."""
    old_repo, old_capture, old_manifests = lint.REPO, lint.CAPTURE, lint.MANIFEST_DIR
    lint.REPO = root
    lint.CAPTURE = root / "capture"
    lint.MANIFEST_DIR = root / "infra" / "k8s" / "chromeless-build"
    buf = io.StringIO()
    try:
        with redirect_stdout(buf):
            rc = lint.main()
    finally:
        lint.REPO, lint.CAPTURE, lint.MANIFEST_DIR = old_repo, old_capture, old_manifests
    return rc, buf.getvalue()


FAILURES: list[str] = []


def check(name: str, cond: bool, detail: str = "") -> None:
    if cond:
        print(f"  ok   {name}")
    else:
        FAILURES.append(name)
        print(f"  FAIL {name}{(' — ' + detail) if detail else ''}")


def case_clean(tmp: Path) -> None:
    root = tmp / "clean"
    _write(root, "capture/a/BUILD.gn", 'test("alpha_unittests") {\n}\n')
    _write(root, "capture/b/BUILD.gn", 'test("beta_unittests") {\n}\n')
    _write(
        root,
        "infra/k8s/chromeless-build/build-job.yaml",
        _manifest("//capture/a:alpha_unittests //capture/b:beta_unittests"),
    )
    rc, out = _run(root)
    check("clean tree passes", rc == 0, f"rc={rc}")
    check("clean tree reports coverage", "2 test target(s)" in out)


def case_split_across_lanes(tmp: Path) -> None:
    """One lane each is enough. Requiring uniformity would be a false positive
    — pcf_unittests is deliberately x264-t2-only in the real tree."""
    root = tmp / "split"
    _write(root, "capture/a/BUILD.gn", 'test("alpha_unittests") {\n}\n')
    _write(root, "capture/b/BUILD.gn", 'test("beta_unittests") {\n}\n')
    _write(
        root,
        "infra/k8s/chromeless-build/build-job-x.yaml",
        _manifest("//capture/a:alpha_unittests"),
    )
    _write(
        root,
        "infra/k8s/chromeless-build/build-job-y.yaml",
        _manifest("//capture/b:beta_unittests"),
    )
    rc, _ = _run(root)
    check("deliberate per-lane asymmetry is not a failure", rc == 0, f"rc={rc}")


def case_the_real_gap(tmp: Path) -> None:
    """The actual defect: declared, merged, never compiled by anything."""
    root = tmp / "gap"
    _write(
        root,
        "capture/build-integration/BUILD.gn",
        """\
        test("cloud_browser_encoder_unittests") {
        }
        test("cloud_browser_adm_unittests") {
        }
        """,
    )
    _write(
        root,
        "infra/k8s/chromeless-build/build-job.yaml",
        _manifest(
            "//capture/build-integration:cloud_browser_worker "
            "//capture/build-integration:cloud_browser_encoder_unittests"
        ),
    )
    rc, out = _run(root)
    check("declared-but-never-built fails", rc == 1, f"rc={rc}")
    check("names the orphan", "cloud_browser_adm_unittests" in out)
    check("names where it was declared", "capture/build-integration/BUILD.gn" in out)
    check(
        "does not flag the non-test target it has no opinion about",
        "cloud_browser_worker" not in out,
    )


def case_ghost_target(tmp: Path) -> None:
    """Manifest names a target no BUILD.gn declares — gn gen would die hours in."""
    root = tmp / "ghost"
    _write(root, "capture/a/BUILD.gn", 'test("alpha_unittests") {\n}\n')
    _write(
        root,
        "infra/k8s/chromeless-build/build-job.yaml",
        _manifest("//capture/a:alpha_unittests //capture/a:removed_unittests"),
    )
    rc, out = _run(root)
    check("built-but-not-declared fails", rc == 1, f"rc={rc}")
    check("names the ghost", "removed_unittests" in out)


def case_dropped_trailing_s(tmp: Path) -> None:
    """The typo that made me widen the direction-2 predicate.

    `_unittest` (no s) is the likeliest misspelling of `_unittests`, and a
    suffix match on "_unittests" skips it exactly. Both arms must fire so the
    reader sees a near-miss pair rather than a phantom missing entry.
    """
    root = tmp / "typo"
    _write(root, "capture/a/BUILD.gn", 'test("alpha_unittests") {\n}\n')
    _write(
        root,
        "infra/k8s/chromeless-build/build-job.yaml",
        _manifest("//capture/a:alpha_unittest"),
    )
    rc, out = _run(root)
    check("dropped trailing 's' fails", rc == 1, f"rc={rc}")
    check("flags it as a ghost, not just as unbuilt", "no BUILD.gn declares" in out)


def case_comment_block_not_parsed_as_value(tmp: Path) -> None:
    """A commented-out target must not count as built.

    The real manifests list every test target inside a comment block for
    documentation. If the parser scooped those up, the lint would pass on the
    exact tree it exists to fail — the ten-week gap would have gone on
    unnoticed with a green lint on top of it.
    """
    root = tmp / "comments"
    _write(
        root,
        "capture/a/BUILD.gn",
        'test("alpha_unittests") {\n}\ntest("beta_unittests") {\n}\n',
    )
    manifest = _manifest("//capture/a:alpha_unittests").replace(
        "# past it.",
        "#   beta_unittests   <- documented here, NOT built\n"
        "                      # past it.",
    )
    _write(root, "infra/k8s/chromeless-build/build-job.yaml", manifest)
    rc, out = _run(root)
    check("a target named only in a comment does not count as built", rc == 1,
          f"rc={rc}")
    check("names it", "beta_unittests" in out)


def case_real_repo(tmp: Path) -> None:
    """The lint must agree with the live tree — 7 declared, all covered."""
    repo = HERE.parent.parent
    declared = set()
    for gn in (repo / "capture").rglob("BUILD.gn"):
        declared |= set(re.findall(r'^test\("([^"]+)"\)', gn.read_text(), re.M))
    rc, out = _run(repo)
    check("live tree passes", rc == 0, f"rc={rc}")
    check(
        f"sees all {len(declared)} declared target(s)",
        f"{len(declared)} test target(s)" in out,
        out.splitlines()[0] if out else "(no output)",
    )


def main() -> int:
    import tempfile

    print("build-targets-lint self-tests")
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        for case in (
            case_clean,
            case_split_across_lanes,
            case_the_real_gap,
            case_ghost_target,
            case_dropped_trailing_s,
            case_comment_block_not_parsed_as_value,
            case_real_repo,
        ):
            case(tmp)

    if FAILURES:
        print(f"\n{len(FAILURES)} self-test(s) FAILED: {', '.join(FAILURES)}")
        return 1
    print("\nall self-tests pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())

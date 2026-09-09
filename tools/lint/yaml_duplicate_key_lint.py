#!/usr/bin/env python3
"""Fail on a duplicate mapping key in any YAML file in this repo.

WHY THIS EXISTS
---------------
On 2026-09-08 `infra/k8s/chromeless-build/build-job-x264-t7.yaml` was found
carrying this, and had for three weeks:

    memory: "64Gi"
    memory: "72Gi"

Twenty lines of comment above the first line argued — with measurements —
for 64Gi. YAML's last-key-wins meant every build ran with a 72Gi
reservation. The 64Gi edit had been inserted ABOVE the existing line
instead of replacing it.

Nothing surfaced it. `kubectl apply` accepts a duplicate mapping key
silently, PyYAML's safe_load accepts it silently, both values scheduled, so
the build worked and the manifest simply did not mean what it said. The
class is worse than the instance: this repo's manifests are unusually
heavily commented precisely BECAUSE the numbers are load-bearing and hard
won (see the `OutOfmemory` / `OutOfcpu` traps in CLAUDE.md), and a silently
ignored value makes every one of those comments unreliable.

That makes it the same shape as CLAUDE.md's "a guard whose skipped path is
silent is not a guard" — the discarded value reports identically to the
applied one.

SCOPE
-----
Every `*.yaml` / `*.yml` outside `.claude/` and `node_modules/`. Helm
templates under `infra/helm/*/templates/` are Go templates, not YAML, and
do not parse — they are skipped BY NAME and the skip is REPORTED, so this
lint never silently checks nothing (same rule as above, applied to itself).
Any other unparseable file is a FAILURE: it means either a real syntax
error or a new template directory this list has not learned about.

Checked against the whole tree when added: zero findings after the t7 fix.
"""

from __future__ import annotations

import pathlib
import sys

import yaml

# Directories whose YAML is Go-templated and cannot be parsed as YAML.
TEMPLATE_DIRS = ("infra/helm",)
SKIP_DIRS = (".claude", "node_modules", ".git")


class DuplicateKeyLoader(yaml.SafeLoader):
    """SafeLoader that records duplicate keys instead of accepting them."""


def _find_duplicates(loader, node, deep=False):
    seen: set = set()
    for key_node, _ in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if isinstance(key, (str, int, float, bool)) and key in seen:
            loader.cb_duplicates.append((key_node.start_mark.line + 1, key))
        seen.add(key)
    return yaml.SafeLoader.construct_mapping(loader, node, deep)


DuplicateKeyLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _find_duplicates
)


def scan(path: pathlib.Path) -> list[tuple[int, object]]:
    loader = DuplicateKeyLoader(path.read_text())
    loader.cb_duplicates = []
    try:
        while loader.check_data():
            loader.get_data()
    finally:
        loader.dispose()
    return loader.cb_duplicates


def main(argv: list[str]) -> int:
    root = pathlib.Path(argv[1]) if len(argv) > 1 else pathlib.Path(".")
    findings: list[str] = []
    templated = 0
    checked = 0

    for path in sorted(root.rglob("*.y*ml")):
        rel = path.relative_to(root).as_posix()
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        if any(rel.startswith(d) for d in TEMPLATE_DIRS):
            templated += 1
            continue
        try:
            dups = scan(path)
        except yaml.YAMLError as exc:
            findings.append(f"{rel}: does not parse as YAML ({type(exc).__name__})")
            continue
        checked += 1
        for line, key in dups:
            findings.append(f"{rel}:{line}: duplicate key {key!r} — "
                            "the EARLIER value is silently discarded")

    if findings:
        print("yaml-duplicate-key-lint: FAIL")
        for f in findings:
            print(f"  {f}")
        return 1

    print(f"yaml-duplicate-key-lint: clean ({checked} files checked, "
          f"{templated} Go-templated skipped)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

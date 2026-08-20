#!/usr/bin/env python3
"""Self-tests for pod_resource_request_lint.

A lint that has never fired is indistinguishable from one that CANNOT fire.
These cases are built from the real manifests involved, so a refactor that
silently stops detecting the defect fails here instead of in production.
"""

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import pod_resource_request_lint as lint          # noqa: E402


def _write(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".yaml", delete=False)
    f.write(text)
    f.close()
    return f.name


# The exact shape build-job-x264.yaml carried on 2026-08-20, when three
# consecutive pods were rejected OutOfephemeral-storage with no log.
DEFECT = """
apiVersion: batch/v1
kind: Job
metadata: {name: chromeless-build}
spec:
  template:
    spec:
      containers:
        - name: build
          resources:
            requests: {cpu: "2", memory: "72Gi"}
            limits:   {cpu: "40", memory: "192Gi", ephemeral-storage: "8Gi"}
"""

FIXED = DEFECT.replace(
    'requests: {cpu: "2", memory: "72Gi"}',
    'requests: {cpu: "2", memory: "72Gi", ephemeral-storage: "512Mi"}',
)


class TestDetectsTheRealDefect(unittest.TestCase):
    def test_fires_on_limit_without_request(self):
        v = lint.check_file(_write(DEFECT))
        self.assertEqual(len(v), 1, v)
        self.assertIn("ephemeral-storage", v[0])
        self.assertIn("build", v[0])

    def test_silent_once_request_is_explicit(self):
        self.assertEqual(lint.check_file(_write(FIXED)), [])

    def test_fires_on_init_containers_too(self):
        doc = DEFECT.replace("containers:", "initContainers:")
        v = lint.check_file(_write(doc))
        self.assertEqual(len(v), 1, v)
        self.assertIn("initContainers", v[0])

    def test_fires_on_flow_style_mapping(self):
        """The wire-bridge job writes resources inline; block-style-only
        matching would have missed it (and did, on the first pass)."""
        doc = """
apiVersion: batch/v1
kind: Job
metadata: {name: wire-bridge}
spec:
  template:
    spec:
      containers:
        - name: test-driver
          resources:
            requests: { cpu: "100m", memory: "256Mi" }
            limits:   { cpu: "1", memory: "1Gi", ephemeral-storage: "2Gi" }
"""
        self.assertEqual(len(lint.check_file(_write(doc))), 1)


class TestNoFalsePositives(unittest.TestCase):
    """CLAUDE.md: a false positive costs more trust than a missed defect
    costs time. Each of these appears in this repo's infra/ tree."""

    def test_helm_template_is_skipped(self):
        doc = """
{{- if .Values.controller.enabled }}
kind: Deployment
spec:
  template:
    spec:
      containers:
        - name: c
          resources:
            limits: {ephemeral-storage: "1Gi"}
{{- end }}
"""
        self.assertEqual(lint.check_file(_write(doc)), [])

    def test_no_resources_block_at_all(self):
        doc = """
apiVersion: batch/v1
kind: Job
metadata: {name: j}
spec:
  template:
    spec:
      containers: [{name: c, image: busybox}]
"""
        self.assertEqual(lint.check_file(_write(doc)), [])

    def test_cpu_and_memory_limits_are_not_this_rule(self):
        """Narrow by design: cpu/memory mirror too, but over-reserving there
        surfaces as a Pending pod with a clear scheduler message."""
        doc = """
apiVersion: batch/v1
kind: Job
metadata: {name: j}
spec:
  template:
    spec:
      containers:
        - name: c
          resources:
            limits: {cpu: "1", memory: "1Gi"}
"""
        self.assertEqual(lint.check_file(_write(doc)), [])

    def test_non_pod_kinds_ignored(self):
        doc = """
apiVersion: v1
kind: ConfigMap
metadata: {name: cm}
data: {note: 'ephemeral-storage: 8Gi'}
"""
        self.assertEqual(lint.check_file(_write(doc)), [])

    def test_malformed_yaml_is_not_this_rule_s_problem(self):
        self.assertEqual(lint.check_file(_write("this: [is: not: yaml")), [])


class TestRepoIsClean(unittest.TestCase):
    def test_infra_tree_passes(self):
        """The tree this lint was written against must be clean, or the rule
        is being landed already-red."""
        root = pathlib.Path(__file__).resolve().parents[2] / "infra"
        if not root.is_dir():
            self.skipTest("infra/ not present")
        violations = []
        for ext in ("*.yaml", "*.yml"):
            for f in root.rglob(ext):
                if ".claude/worktrees" in str(f):
                    continue
                violations += lint.check_file(f)
        self.assertEqual(violations, [], "\n".join(violations))


if __name__ == "__main__":
    unittest.main(verbosity=2)

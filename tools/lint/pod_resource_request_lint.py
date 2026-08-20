#!/usr/bin/env python3
"""Every ephemeral-storage LIMIT must have an explicit REQUEST beside it.

WHY THIS EXISTS
---------------
Kubernetes mirrors an omitted request from the limit. So a container that
declares only

    limits:
      ephemeral-storage: "8Gi"

is not asking for "up to 8Gi" — it is RESERVING 8Gi at admission time. On a
node whose ephemeral storage is already heavily reserved, that pod is rejected
before it starts:

    Pod was rejected: Node didn't have enough resource: ephemeral-storage,
    requested: 8589934592, used: 87258300416, capacity: 94518993559

The rejection produces NO container and therefore NO log. The Job just retries
and each attempt dies the same way, so the lane reads as mysteriously dead
rather than as a resource problem. That is what makes this worth a lint: the
symptom does not point at the cause.

The reservation is also fiction. Measured on triform-8, 2026-08-20: 81Gi of
ephemeral RESERVATIONS across the node corresponded to 2.5Gi of actual use,
and the chromeless build image's own writable layer is 522Mi (debian-slim base
82Mi + 440Mi of apt deps, measured with a probe pod running the lane's exact
apt-get line). All heavy build I/O lives on the /work hostPath and never
touches container-ephemeral storage at all.

HISTORY
-------
Three separate times, same defect:
  * chromeless-kaniko-push.yaml  — fixed 2026-06-29
  * build-job-x264-t7.yaml       — fixed 2026-07-02
  * build-job-x264.yaml (t8)     — fixed 2026-08-20, this lint's occasion
and three MORE lanes (nvenc, vaapi, x264-t2) were found carrying it latent,
having never been fired on a full node.

Fixing the instance three times and never the class is what this file is for.

The rule is deliberately narrow: it does NOT require a request for cpu/memory
(those mirror too, but an over-reservation there is visible as a Pending pod
with a clear scheduler message, and the repo's lanes set them explicitly
anyway). It fires only on ephemeral-storage, only when a limit is present with
no matching request.
"""

import sys
import pathlib

try:
    import yaml
except ImportError:                                       # pragma: no cover
    print("pod-resource-request lint: PyYAML not available, skipping")
    sys.exit(0)

# Kinds that carry a pod template, and where the template lives.
_POD_TEMPLATE_PATHS = {
    "Job":         ("spec", "template"),
    "CronJob":     ("spec", "jobTemplate", "spec", "template"),
    "Deployment":  ("spec", "template"),
    "StatefulSet": ("spec", "template"),
    "DaemonSet":   ("spec", "template"),
    "ReplicaSet":  ("spec", "template"),
    "Pod":         (),
}

RESOURCE = "ephemeral-storage"


def _dig(doc, path):
    cur = doc
    for key in path:
        if not isinstance(cur, dict):
            return None
        cur = cur.get(key)
    return cur


def _containers(doc):
    """Yield (section, container) for every container in a pod-bearing doc."""
    kind = doc.get("kind")
    if kind not in _POD_TEMPLATE_PATHS:
        return
    tmpl = _dig(doc, _POD_TEMPLATE_PATHS[kind])
    if tmpl is None:
        return
    spec = tmpl.get("spec") if kind != "Pod" else tmpl
    if not isinstance(spec, dict):
        return
    for section in ("initContainers", "containers"):
        for c in spec.get(section) or []:
            if isinstance(c, dict):
                yield section, c


def is_template(text):
    """True for Helm/Kustomize templates, which are not valid YAML.

    A Helm template starts `{{- if ... }}` and PyYAML cannot parse it. Reporting
    those as violations is a false positive, and CLAUDE.md is explicit that a
    false positive costs more trust than a missed defect costs time. Helm charts
    get their resource blocks from values.yaml anyway, so there is nothing here
    for this rule to check even after rendering.
    """
    return "{{" in text


def check_file(path):
    """Return a list of human-readable violation strings for one YAML file."""
    violations = []
    try:
        with open(path) as fh:
            text = fh.read()
    except OSError as exc:
        return [f"{path}: unreadable: {exc}"]

    if is_template(text):
        return []

    try:
        docs = list(yaml.safe_load_all(text))
    except yaml.YAMLError as exc:
        # A genuinely malformed manifest is a real problem, but it is not THIS
        # rule's problem, and failing here would make this lint the messenger
        # for every unrelated YAML defect in the tree.
        return []

    for doc in docs:
        if not isinstance(doc, dict):
            continue
        name = _dig(doc, ("metadata", "name")) or "<unnamed>"
        for section, c in _containers(doc):
            res = c.get("resources") or {}
            limits = res.get("limits") or {}
            requests = res.get("requests") or {}
            if RESOURCE in limits and RESOURCE not in requests:
                violations.append(
                    f"{path}: {doc.get('kind')}/{name} "
                    f"{section}[{c.get('name', '?')}] declares a {RESOURCE} "
                    f"LIMIT ({limits[RESOURCE]}) with no matching REQUEST.\n"
                    f"    K8s mirrors the limit into the request, so this "
                    f"reserves {limits[RESOURCE]} at admission and is "
                    f"rejected on a busy node with no log to read.\n"
                    f"    Add an explicit '{RESOURCE}:' under requests: "
                    f"(512Mi is the measured floor for the build lanes)."
                )
    return violations


def main(argv):
    roots = argv[1:] or ["infra"]
    files = []
    for root in roots:
        p = pathlib.Path(root)
        if p.is_file():
            files.append(p)
            continue
        for ext in ("*.yaml", "*.yml"):
            # .claude/worktrees/ holds full checkouts of this repo; walking it
            # returns every hit N+1 times. Excluded everywhere in this tree.
            files += [
                f for f in p.rglob(ext)
                if ".claude/worktrees" not in str(f)
            ]

    all_violations = []
    for f in sorted(set(files)):
        all_violations += check_file(f)

    if all_violations:
        print("!!! pod-resource-request lint FAILED\n")
        for v in all_violations:
            print(f"  {v}\n")
        return 1

    print(f">>> pod-resource-request lint clean ({len(set(files))} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

#!/usr/bin/env python3
"""Every file that pins a deployable image must agree with build/guest-release.json.

WHY THIS EXISTS
---------------
On 2026-08-24 a user could not use the standalone stack for an entire day. The
worker Deployment changed image at least six times; `kubectl rollout history`
reached revision 66. No single person was reverting it. THREE mechanisms were
acting at once, and only the crudest was visible:

  1. `deploy.sh` runs `kubectl apply -f stack.yaml`, and `stack.yaml` pinned an
     image 64 commits behind the fix under test. Any apply — a deploy script, a
     smoke test, a teammate — silently rolled the cluster back. A `set image`
     rolled it forward. Neither party had to intend it: kubectl showed TWO
     field managers owning `spec.containers[].image`
     (`kubectl-client-side-apply` and `kubectl-set`), each pulling to its own
     answer.

  2. Two other sessions were deploying to the same namespace. There was no
     lock, no ownership, and nothing announced a change.

  3. Mutable tags. The gateway Deployment reported `standalone-v9` — the right
     tag — while resolving to a digest that was NOT the build that tag was
     created for; another session had rebuilt over it. "The right tag is
     deployed" is therefore not evidence that the right code is running. Only
     a digest is.

The cost was not the flapping itself. It was that every party kept diagnosing a
stack that had already changed underneath them: tests passed against an image
the user never saw, and the user hit a bug the tests had "proved" fixed.

WHAT IT CHECKS
--------------
1. Every listed k8s manifest has an immutable worker image pin agreeing with
   build/guest-release.json. Disagreement is the drift that makes `apply` a
   rollback.
2. guest-release.json records a DIGEST, not only a tag — so the pin identifies
   an artifact rather than a name someone can rebuild over.

Sibling of guest_release_lint.py, which exists because three sources disagreed
about the same question one directory over. Same defect, same fix: make the
disagreement fail a lint instead of a user's afternoon.
"""
from __future__ import annotations

import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
PIN = ROOT / "build" / "guest-release.json"

# Manifests that deploy the worker. Each is applied by deploy.sh or by hand, so
# each is a candidate rollback if it disagrees with the pin.
MANIFESTS = [
    ROOT / "infra" / "k8s" / "standalone" / "stack.yaml",
]

sys.path.insert(0, str(ROOT / "tools"))
from release_pins import immutable_image, worker_images


def main() -> int:
    problems: list[str] = []

    if not PIN.is_file():
        print(f"deploy-pin-lint: missing {PIN.relative_to(ROOT)}", file=sys.stderr)
        return 1

    try:
        pin = json.loads(PIN.read_text())
    except json.JSONDecodeError as exc:
        print(f"deploy-pin-lint: {PIN.relative_to(ROOT)} is not valid JSON: {exc}",
              file=sys.stderr)
        return 1

    pinned_image = pin.get("image", "")
    pinned_tag = pinned_image.rsplit(":", 1)[-1] if ":" in pinned_image else ""
    if not pinned_tag:
        problems.append("build/guest-release.json has no usable `image`")

    try:
        expected = immutable_image(pinned_image, pin.get("digest", ""))
    except (TypeError, ValueError):
        problems.append("build/guest-release.json has an invalid worker image or digest")
        expected = None

    for man in MANIFESTS:
        rel = man.relative_to(ROOT)
        if not man.is_file():
            problems.append(f"missing deployment manifest {rel}")
            continue
        images = worker_images(man.read_text())
        if not images:
            problems.append(f"{rel} has no recognized worker image pin")
        for image in images:
            if expected and image not in (expected, pinned_image + "@" + pin["digest"]):
                problems.append(f"{rel} pins {image!r}; expected immutable worker {expected!r}")

    if problems:
        print("deploy-pin-lint: FAIL", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1

    print(f"deploy-pin-lint: clean (worker pin {pinned_tag} agrees across "
          f"{len(MANIFESTS)} manifest(s) + guest-release.json, digest recorded)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

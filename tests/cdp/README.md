# tests/cdp — chromium embedder CDP smoke

A pytest suite that drives the cb-chromium binary directly via Chrome
DevTools Protocol (CDP), bypassing the platform layer entirely. Catches
embedder regressions in ~5 seconds — the same class of bug that
deploy-and-pray takes 20+ minutes per cycle to surface.

## What it covers

Four CDP operations, each with its own pytest function so a failing log
points at exactly which override regressed:

| Op | Embedder dependency |
|---|---|
| `Target.createBrowserContext` | `CreateBrowserContext` override (bare chromium returns "Not implemented"). |
| `Target.createTarget` (with `browserContextId`) | `CreateNewTarget` accepting a non-default context. |
| `Target.attachToTarget` (`flatten=True`) | Per-target protocol session plumbing. |
| `Page.navigate` | Renderer dispatch + navigation pipeline. |

Each test creates its own browser context. We deliberately do **not**
call `Target.disposeBrowserContext` on cleanup because it currently
crashes the cb-chromium binary (tracked separately as a follow-up to
the `fcbeb16` embedder fix). Leaking contexts is harmless — the
cb-cdp-validation pod runs once per build and is GC'd immediately.
When the dispose crash is fixed, restore module-level cleanup via a
session fixture in `conftest.py`.

## How to run locally

```sh
# 1. Port-forward cb-browserless to your workstation.
kubectl -n triform-wtf port-forward svc/cb-browserless 9222:9222 &

# 2. Install pinned deps in a venv.
python3 -m venv .venv && source .venv/bin/activate
pip install -r tests/cdp/requirements.txt

# 3. Point the suite at the forwarded port.
CB_URL=http://localhost:9222 pytest tests/cdp/ -v
```

A passing run takes ~5 seconds.

## How it's wired into CI

`build/cb-build.sh` Step 10 (`cdp validation`) runs after the kaniko
sidecar has pushed the image. It applies
`infra/k8s/tests/cb-cdp-validation.yaml`, which schedules a
`python:3.11-slim` Job in the `cb-tests` namespace that mounts this
directory and runs `pytest tests/cdp/`. The image tag of the just-pushed
binary is passed to the Job via the `CB_TEST_IMAGE_TAG` env var (NOT sed
substitution into the manifest — the manifest is a stable artifact).

The build script blocks on the Job with a 120 s timeout
(`kubectl wait --for=condition=complete --timeout=120s`) and exits
non-zero if the Job fails or times out, gating image promotion on a
green CDP smoke.

## What it gates

If Step 10 fails, the just-pushed image is **not** promoted to
`cb-chromium:latest` and physics will continue to use whatever the prior
green build was. This protects the platform from embedder regressions
landing in production with no validation.

If you need to bypass the gate (e.g. you're investigating a flake by
hand), run cb-build.sh up through Step 9 with whatever override the
build host supports — but the full `bash build/cb-build.sh` invocation
will always exit non-zero if Step 10 is red.

## Adding tests

New CDP ops added to the embedder belong here, not in
`tests/integration/` (which is for the Go signaling/sender side) and not
in `tests/smoke/` (which is shell + container-level). Mirror the
existing function shape:

- One `@pytest.mark.asyncio` function per CDP op.
- Each test acquires its own browser context and disposes it on the way
  out, so test order independence is preserved.
- Failures call `pytest.fail()` with a verbatim CDP error envelope so
  the CI log is self-explanatory.

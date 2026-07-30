# `build/` — Phase 2 build orchestration

This directory holds everything the K8s build Job (T112, infra-dev's
work) needs to produce the Phase 2 `cloud_browser_worker` runtime
image.

| File                  | Purpose                                                                                |
|-----------------------|----------------------------------------------------------------------------------------|
| `chromeless-build.sh`         | Orchestrator. Runs T101's Day-1 sequence end-to-end inside the build pod.              |
| `Dockerfile.runtime`  | Final runtime image — debian-slim base + `cloud_browser_worker` + supervisord glue.    |
| `README.md`           | This file.                                                                             |

The companion file in `capture/build-integration/build.sh` (T49) is
the **developer-host** wrapper for the same operations
(apply-patches / gen / ninja) when working from inside an existing
Chromium tree. `chromeless-build.sh` calls into it for those three steps so
behaviour stays consistent across the dev-host and Job paths.

## How the K8s Job invokes this

T112's Job spec mounts:

| Mount path     | What                                                                                  |
|----------------|----------------------------------------------------------------------------------------|
| `/workspace`   | This repo, RW (so we can `git -C /workspace ...` for SHA, etc.).                       |
| `/work/src/chromium` | Persistent volume holding the Chromium checkout. Reused across Job retries.    |
| `/work/artifacts`    | Output volume. `chromeless-build.sh` writes the binary tarball + the kaniko build context here. |
| `/work/logs`         | Persistent volume for run logs. `chromeless-build-<timestamp>.log` per invocation.       |
| `/sccache`           | Persistent volume for the sccache cache. The single biggest determinant of build time. |

Then it runs:

```
bash /workspace/build/chromeless-build.sh
```

…with optional env-var overrides:

| Var                       | Default                                  | Purpose                                                  |
|---------------------------|-------------------------------------------|----------------------------------------------------------|
| `CHROMELESS_REPO`                 | `/workspace`                              | This repo's path inside the pod.                         |
| `CHROMELESS_WORK_ROOT`            | `/work`                                   | Override for the work-dir prefix (used by STUB_MODE local runs). |
| `CHROMIUM_BRANCH_NUMBER`  | `7727`                                    | Pinned Chromium release branch (T17 §6). Update at each roll. |
| `SCCACHE_DIR`             | `/sccache`                                | sccache cache root.                                      |
| `NINJA_PARALLELISM`       | (unset)                                   | `-j` arg for autoninja. Default lets ninja auto-pick.    |
| `CHROMELESS_BUILD_TARGETS`        | `cloud_browser_worker cloud_browser_encoder_unittests cloud_browser_framesink_capturer_unittests` | The three Phase-2 targets. |
| `SKIP_FETCH`              | (unset)                                   | `1` skips Step 1 if the Chromium tree is already populated (Job retry). |
| `STUB_MODE`               | (unset)                                   | `1` exercises the script's structure without actually building. The DoD test path. |

The Job's downstream **kaniko sidecar** picks up
`/work/artifacts/context/` and pushes
`chromeless:cr${CHROMIUM_BRANCH_NUMBER}-${CHROMELESS_GIT_SHA}` to the
forgejo registry. Registry auth lives there, not here.

## What each step does + expected runtime

| Step | What | Cold | Warm sccache |
|------|------|------|--------------|
| 1/10 gclient sync       | `gclient config` + `gclient sync --no-history --shallow` to the pinned branch-head. ~30 GB checkout (vs ~80 GB with full history). | 30–60 min | 0–5 min if SKIP_FETCH=1 |
| 2/10 symlink            | `ln -s ${CHROMELESS_REPO} ${CHROMIUM_SRC}/src/cloud-browser`. | <1 s | <1 s |
| 3/10 apply patches      | Delegates to `capture/build-integration/build.sh apply-patches` (T49). | <5 s | <5 s |
| 4/10 sccache setup      | `sccache --start-server` + env vars. | <5 s | <5 s |
| 5/10 gn gen             | Delegates to T49's wrapper. | 1–3 min | 1–3 min |
| 6/10 autoninja          | The big one — `cloud_browser_worker` + the two test targets. **The single biggest variable.** | **3–6 h** | **45–90 min** |
| 7/10 unit tests         | `cloud_browser_encoder_unittests` + `cloud_browser_framesink_capturer_unittests`. ~52 tests per T97 §5. | 1–2 min | 1–2 min |
| 8/10 package binary     | Strip + `tar --zstd -cf` into `/work/artifacts/`, then stage the worker plus ICU/GL runtime assets for the image. | 1–2 min | 1–2 min |
| 9/10 stage image context| Copies Dockerfile.runtime, Phase 2 supervisord config, launcher, Pulse/devtools/lifecycle glue, and streamer assets into the kaniko context. | <5 s | <5 s |
| 10/10 CDP validation    | Runs the cluster CDP validation Job against the just-built image before promotion. | 1–2 min | 1–2 min |
| **Total**              |  | **4–8 h** | **~1 h** |

T17 §4 numbers are the source of truth for the autoninja step. The
warm-cache time depends on hit ratio; T17 §5 cites 70–85% as the
typical band for incremental Chromium patches.

## Triage — common failures

Drawn from T101 §2 ("failure modes to expect") + T49 patches/README
rebase-strategy notes. Look at the matching `chromeless-build-<timestamp>.log`
in `/work/logs/` first.

### Step 1 — gclient sync fails

- **Authentication error against `chromium.googlesource.com`.** Usually
  a Job network policy issue. The Job needs egress to
  `*.googlesource.com` and to `commondatastorage.googleapis.com`
  (Chromium's PGO profiles).
- **Disk full mid-sync.** The `/work/src/chromium` PVC is sized too
  small. Resize to ≥150 GB and retry with `SKIP_FETCH=` (so we
  re-fetch into the now-larger volume).
- **Bad branch number.** A malformed `CHROMIUM_BRANCH_NUMBER` (e.g.,
  pointing at a since-deleted branch) makes `--revision` fail.
  Verify against [chromiumdash.appspot.com/branches](https://chromiumdash.appspot.com/branches).

### Step 3 — patch fails to apply

Per T49's patches/README rebase-strategy: the patch context is
authored against a specific branch-head and **drifts** across
Chromium versions. When `git apply` fails:

1. Check the failing patch's commit-message "Touched files" list.
2. Run `git -C /work/src/chromium/src apply --3way patches/0001-*.patch`
   — the 3-way resolver fixes most context drift automatically.
3. If 3-way fails too, the patch needs hand-rebasing onto the
   pinned branch — file an `roll/m-NNN` follow-up per T17 §6.

### Step 6 — autoninja link fail

- **Out of memory at link time.** The `chrome` link peaks 16-24 GB
  per T17 §4; we don't link `chrome`, but `cloud_browser_worker`'s
  link is in the same ballpark. If the pod is under-resourced, raise
  the `resources.limits.memory` on the Job spec.
- **Symbol-not-found from libwebrtc.** Header-include drift between
  what we authored against (`encoder_factory_stub.cc` etc.) and the
  pinned branch's libwebrtc. Surface the exact mangled name in the
  log and follow the per-encoder rationale doc — most fixes are
  one-line include rewrites or namespace re-exports.
- **Patch file conflict at compile time.** Patch 0001 modifies
  `peer_connection_dependency_factory.cc`; if Chromium reorganised
  that file between rolls, the patch applied cleanly but produced
  invalid code. Re-base the patch.

### Step 7 — unit test fail on a green build

The HAS_NVENC / HAS_VAAPI / HAS_SVT_AV1 gated tests should `GTEST_SKIP`
when their probes don't find the dependency on the build host (the
build host has no GPU; the gates fire false). If they fail instead
of skip, the gate logic regressed — file a follow-up against the
specific encoder.

The always-on tests should pass first try; if they fail, see the
specific encoder's rationale doc for the load-bearing config knobs.

## How to run STUB_MODE locally

Useful for verifying the script's structure without 4 hours of build
time:

```bash
mkdir -p /tmp/chromeless-build-test/work /tmp/chromeless-build-test/sccache
CHROMELESS_REPO=$(pwd) \
CHROMELESS_WORK_ROOT=/tmp/chromeless-build-test/work \
SCCACHE_DIR=/tmp/chromeless-build-test/sccache \
STUB_MODE=1 \
bash build/chromeless-build.sh
```

Each step prints its `=== STEP N/9 ... ===` markers and skips the
heavy work. Useful as a pre-merge smoke test before a CI/CD
push.

## Provenance — `build/guest-release.json`

This repo BUILDS the guest image, so this repo states what shipped.
`build/guest-release.json` is that statement, and it is the intended
source of truth for any consumer that pins the guest image.

### The loop this exists to break

The monorepo's `scripts/chromeless-auto-bump.sh` resolves its target
image from the k8s `BrowserSessionPool/sw-pool` object, declaring that
pool "the source of truth — we follow, never lead". But `sw-pool`'s
template image is only ever hand-edited to match the monorepo's own pin.
So `CURRENT == TARGET` by construction, and the bumper reports "up to
date", exit 0, forever. The chromeless repo was not an input at any
point. That is not a check that broke — it is a check that structurally
**cannot** fail, because nothing in the loop can disagree with anything
else. Adding a producer-side record gives the comparison a second,
independent voice, so it can finally come out false.

### Schema (`chromeless.guest-release/v1`)

| field | meaning |
|---|---|
| `commit` | full 40-char chromeless SHA the guest binary was built from |
| `image` | fully-qualified image ref including tag — what a pin must equal |
| `digest` | `sha256:` manifest digest; the immutable identity (a tag can be re-pushed, a digest cannot) |
| `dockerfile` | which Dockerfile produced it — `build/Dockerfile.runtime` for the real guest |
| `built_at` | RFC3339 UTC |
| `recorded_by` | `chromeless-kaniko-push.sh` for automated writes; `seed-manual` for the hand-seeded first entry |
| `known_gap` | present ONLY while a stated invariant is knowingly violated; see below |

Fields are flat and literal so a checker needs no parsing cleverness.
Two invariants a consumer should assert:

1. the monorepo pin string equals `.image`
2. `.commit` is an ancestor of chromeless `main`

### `known_gap` — read this before "fixing" a red

The currently-recorded image was built from branch `capstats-plus-rearm`,
which was never merged. Invariant 2 is therefore **expected to fail
today**, and the record says so in `known_gap` rather than hiding it.
That red is correct and actionable: production is running a binary whose
exact provenance is not on `main`.

Note the subtlety — the reconcile PR cherry-picked the load-bearing
commit onto `main`, which makes main's guest **source** byte-identical to
the shipped tree, but a cherry-pick mints a *new* SHA. So the recorded
commit still is not an ancestor. Source parity is not ancestry.

It clears exactly one way: rebuild the guest from a `main` SHA and let
the push step rewrite this file. Never by hand-editing the field.

### How it gets written

`infra/k8s/chromeless-build/chromeless-kaniko-push.sh` writes it after a
push whose Job reports `Complete=True`, so the file cannot claim an image
that was never pushed. The SHA comes from the build **tag**, never from
the operator's local checkout — that tree is routinely on some other
branch, which is precisely how the drift arose.

`chromeless-build.sh` deliberately does NOT write it: that script runs
inside the K8s Job, on a build node, as uid 1000, against a bootstrap
clone with no credentials and no working tree. It cannot update a
git-tracked file, and wiring it there would be a mechanism that silently
never runs.

**The honest limitation:** the push script writes the file but does not
commit it — it prints the exact `git commit` command instead. An operator
who pushes and never commits leaves the record stale, and the
consumer-side check then fails on the mismatch. Stale-and-caught is the
designed failure mode; silently-wrong is the one being eliminated.

## Coordination notes

- **T112 + T113 land together.** T112 owns the K8s Job spec; T113
  owns this script. Neither one is useful without the other.
- **Guest provenance.** After any `chromeless-kaniko-push.sh` run,
  commit the regenerated `build/guest-release.json` in the same change
  that rolls the pin. A push that is never committed reads downstream as
  a pin/record disagreement.
- **T114 follows.** Once T113 produces a runtime image, T114
  exercises NVENC on the triform-5 Blackwell pool — see the joint
  task description.
- **T17 pin updates.** When Chromium rolls, bump
  `CHROMIUM_BRANCH_NUMBER` in both this script's default AND the
  Phase 2 Helm chart's image-tag template. Inconsistency = a build
  succeeds but the deploy uses the previous image.
- **Patch series freshness.** Every roll re-tests the patches in
  `patches/` against the new branch-head. Today there's only patch
  0001 (T49 — encoder-factory injection); the rebase cost is small.

# `build/` — Phase 2 build orchestration

This directory holds everything the K8s build Job (T112, infra-dev's
work) needs to produce the Phase 2 `cloud_browser_worker` runtime
image.

| File                  | Purpose                                                                                |
|-----------------------|----------------------------------------------------------------------------------------|
| `cb-build.sh`         | Orchestrator. Runs T101's Day-1 sequence end-to-end inside the build pod.              |
| `Dockerfile.runtime`  | Final runtime image — debian-slim base + `cloud_browser_worker` + supervisord glue.    |
| `README.md`           | This file.                                                                             |

The companion file in `capture/build-integration/build.sh` (T49) is
the **developer-host** wrapper for the same operations
(apply-patches / gen / ninja) when working from inside an existing
Chromium tree. `cb-build.sh` calls into it for those three steps so
behaviour stays consistent across the dev-host and Job paths.

## How the K8s Job invokes this

T112's Job spec mounts:

| Mount path     | What                                                                                  |
|----------------|----------------------------------------------------------------------------------------|
| `/workspace`   | This repo, RW (so we can `git -C /workspace ...` for SHA, etc.).                       |
| `/work/src/chromium` | Persistent volume holding the Chromium checkout. Reused across Job retries.    |
| `/work/artifacts`    | Output volume. `cb-build.sh` writes the binary tarball + the kaniko build context here. |
| `/work/logs`         | Persistent volume for run logs. `cb-build-<timestamp>.log` per invocation.       |
| `/sccache`           | Persistent volume for the sccache cache. The single biggest determinant of build time. |

Then it runs:

```
bash /workspace/build/cb-build.sh
```

…with optional env-var overrides:

| Var                       | Default                                  | Purpose                                                  |
|---------------------------|-------------------------------------------|----------------------------------------------------------|
| `CB_REPO`                 | `/workspace`                              | This repo's path inside the pod.                         |
| `CB_WORK_ROOT`            | `/work`                                   | Override for the work-dir prefix (used by STUB_MODE local runs). |
| `CHROMIUM_BRANCH_NUMBER`  | `7727`                                    | Pinned Chromium release branch (T17 §6). Update at each roll. |
| `SCCACHE_DIR`             | `/sccache`                                | sccache cache root.                                      |
| `NINJA_PARALLELISM`       | (unset)                                   | `-j` arg for autoninja. Default lets ninja auto-pick.    |
| `CB_BUILD_TARGETS`        | `cloud_browser_worker cloud_browser_encoder_unittests cloud_browser_framesink_capturer_unittests` | The three Phase-2 targets. |
| `SKIP_FETCH`              | (unset)                                   | `1` skips Step 1 if the Chromium tree is already populated (Job retry). |
| `STUB_MODE`               | (unset)                                   | `1` exercises the script's structure without actually building. The DoD test path. |

The Job's downstream **kaniko sidecar** picks up
`/work/artifacts/context/` and pushes
`cb-chromium:cr${CHROMIUM_BRANCH_NUMBER}-${CB_GIT_SHA}` to the
forgejo registry. Registry auth lives there, not here.

## What each step does + expected runtime

| Step | What | Cold | Warm sccache |
|------|------|------|--------------|
| 1/9 gclient sync       | `gclient config` + `gclient sync --no-history --shallow` to the pinned branch-head. ~30 GB checkout (vs ~80 GB with full history). | 30–60 min | 0–5 min if SKIP_FETCH=1 |
| 2/9 symlink            | `ln -s ${CB_REPO} ${CHROMIUM_SRC}/src/cloud-browser`. | <1 s | <1 s |
| 3/9 apply patches      | Delegates to `capture/build-integration/build.sh apply-patches` (T49). | <5 s | <5 s |
| 4/9 sccache setup      | `sccache --start-server` + env vars. | <5 s | <5 s |
| 5/9 gn gen             | Delegates to T49's wrapper. | 1–3 min | 1–3 min |
| 6/9 autoninja          | The big one — `cloud_browser_worker` + the two test targets. **The single biggest variable.** | **3–6 h** | **45–90 min** |
| 7/9 unit tests         | `cloud_browser_encoder_unittests` + `cloud_browser_framesink_capturer_unittests`. ~52 tests per T97 §5. | 1–2 min | 1–2 min |
| 8/9 package binary     | Strip + `tar --zstd -cf` into `/work/artifacts/`. | 1–2 min | 1–2 min |
| 9/9 stage image context| Copies Dockerfile.runtime + supervisord.conf + launch-chromium.sh into the kaniko context. | <5 s | <5 s |
| **Total**              |  | **4–8 h** | **~1 h** |

T17 §4 numbers are the source of truth for the autoninja step. The
warm-cache time depends on hit ratio; T17 §5 cites 70–85% as the
typical band for incremental Chromium patches.

## Triage — common failures

Drawn from T101 §2 ("failure modes to expect") + T49 patches/README
rebase-strategy notes. Look at the matching `cb-build-<timestamp>.log`
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
mkdir -p /tmp/cb-build-test/work /tmp/cb-build-test/sccache
CB_REPO=$(pwd) \
CB_WORK_ROOT=/tmp/cb-build-test/work \
SCCACHE_DIR=/tmp/cb-build-test/sccache \
STUB_MODE=1 \
bash build/cb-build.sh
```

Each step prints its `=== STEP N/9 ... ===` markers and skips the
heavy work. Useful as a pre-merge smoke test before a CI/CD
push.

## Coordination notes

- **T112 + T113 land together.** T112 owns the K8s Job spec; T113
  owns this script. Neither one is useful without the other.
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

# `patches/` — Chromium patch series for the cloud-browser worker

This directory holds every change we apply on top of upstream
Chromium to produce our worker binary. Per
[`PROJECT_BRIEF.md`](../PROJECT_BRIEF.md) Risks: *"Pin to a release
branch; track stable on a schedule. Maintain a small, well-documented
patch series."* — that's this directory.

## Conventions

- **One concern per patch.** A patch that touches three unrelated
  things is two patches that haven't been split yet.
- **`NNNN-short-description.patch`** naming, monotonically numbered.
  Patches apply in numerical order.
- **`git format-patch` shape.** Patches carry a from-author /
  subject / commit-message header; the message is the source of
  truth for *why* the patch exists.
- **Upstream-quality commit messages.** "Why this patch exists,"
  "Why not avoid the patch," and (where applicable) "Upstream
  considerations" so the next reviewer doesn't have to re-derive the
  rationale.
- **Touched files listed at the bottom of the message.** Same shape
  as Chromium's own commit messages — rebases tell us at a glance
  which patch a conflict belongs to.

## Applying

```bash
# From the Chromium src/ root, with this repo symlinked at //cloud-browser:
for p in cloud-browser/patches/[0-9]*.patch; do
  git apply --3way --check "$p" && git am --keep-non-patch "$p"
done
```

`build.sh apply-patches` does this with better error messages and a
sanity check that `HEAD` is on a `refs/branch-heads/*` ref.

> **Before applying, ensure your Chromium tree is on
> `refs/branch-heads/<NNNN>` matching the pin in
> [`docs/build/chromium-from-source.md`](../docs/build/chromium-from-source.md)
> §6.** Patches authored against `main` are not portable to release
> branches without context conflicts; we author against the pinned
> branch and never test on `main`.

## Rebase strategy on Chromium rolls

Per `docs/build/chromium-from-source.md` §6 the roll cadence is every
~4 weeks or every other milestone. On each roll:

1. Cut `roll/m-NNN` from the current tip.
2. `gclient sync --with_branch_heads --with_tags --revision src@refs/branch-heads/<new>`.
3. Re-apply this series. **Conflicts are fix-forward, never skip.**
4. Run the full unit + integration suite (T18, T27, T41).
5. Bench the latency harness (T10/T11) on the production target
   instance type. If latency regresses by more than 5 ms p95, treat
   it as a roll blocker — investigate before merging the roll
   branch.
6. Merge the roll branch. CI gate requires `headless_shell` and
   `chrome` to build; per docs/build §6 we don't merge a roll until
   both pass.

If a single patch becomes painful to rebase across two consecutive
rolls, revisit whether it can be reduced (smaller diff, narrower
seam) or upstreamed.

## Per-patch index

| # | Patch | Purpose | Upstream tracking |
|---|-------|---------|-------------------|
| 0005 | `0005-expose-host-frame-sink-manager.patch` | Adds `content::GetEmbedderHostFrameSinkManager()` — a CONTENT_EXPORT free function in `content/public/browser/content_browser_client.h` that re-exports the content-internal `content::GetHostFrameSinkManager()` so the cloud-browser embedder (`capture/build-integration/cb_devtools_agent.cc` — Track E / T55) can build a producer-side `mojo::Remote<viz::mojom::FrameSinkVideoCapturer>` without depending on `//content/browser:browser` (whose visibility list rejects out-of-tree consumers). | None yet — file upstream bug if pattern proves durable across two roll cycles. |

M7 R6: patches 0001 and 0004 retired. With M1 instantiating
`PeerConnectionFactoryInterface` in the browser process inside
`cloud_browser_main_parts.cc`, the renderer-side `GetWebRtcVideoEncoderFactory()`
embedder hook (0001) and its `unique_ptr` include glue (0004) are
unreachable. 0002–0003 still exist on disk and apply during build.
When patch 0002/3 land in this table, append them (don't reorder —
patch numbers are stable references).

## When NOT to add a patch

- **Encoder logic.** Lives in `//cloud-browser/capture/encoder/...`,
  which is our code in our tree. No upstream patch needed.
- **Capture-side `webrtc::VideoTrackSource`.** Same — our code,
  consumes the public Mojo `FrameSinkVideoCapturer` API; no patch.
- **Anything that can be done with a flag, env var, or
  `ContentBrowserClient` virtual that already exists.** Read the
  upstream header before patching it; the embedder seam is often
  already there.

## Verification

Apply via `build/chromeless-build.sh apply-patches` which runs `git am`
against the Chromium tree pinned per `docs/build/chromium-from-source.md`
§6. Conflicts are fix-forward, never skip.

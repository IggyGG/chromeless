# `capture/build-integration/` — Chromium / libwebrtc build scaffold

This directory wires our encoder factory (T19 / T35 / T36) and the
forthcoming FrameSinkVideoCapturer hook (T47) into a real
Chromium-based binary — `cloud_browser_worker` — that we will ship as
the Phase 2 capture-and-encode side of the cloud browser.

> **Status:** scaffold only. Actual compilation is gated on T17's
> build environment being provisioned. Every generated artefact
> carries a `// TODO(T17-build-env): exercise this on a Linux box
> with depot_tools` marker. The point of T49 is to make "produce a
> binary" a one-`bash build.sh` step the day the build host exists.

## How our changes relate to upstream Chromium

We track upstream Chromium on a pinned **release branch**
(`refs/branch-heads/NNNN`, see `docs/build/chromium-from-source.md`
§6). Our changes layer on top as a **small, well-documented patch
series** under [`/patches/`](../../patches/) — every patch is one
file, in `git format-patch` shape, with a numbered prefix and a
reason-for-existence in its commit message.

The patch series is the foreverest part of this stack: per
PROJECT_BRIEF.md's risk table — *"Pin to a release branch; track
stable on a schedule. Maintain a small, well-documented patch
series."* — every patch is a maintenance cost forever, so we keep
the surface tiny:

- Patches expose **embedder hooks**, not behavioural changes.
- One concern per patch.
- Each patch's commit message names the upstream API it touches and
  why we cannot avoid touching it.
- On every Chromium roll (T17 §6 cadence: every ~4 weeks or every
  other milestone), the roll branch re-applies the series; merge
  conflicts are fixed forward, never skipped.

Today the series is three patches — see
[`patches/README.md`](../../patches/README.md) for the index and the
rebase-on-roll procedure:

- [`0002-add-cloud-browser-to-build-graph.patch`](../../patches/0002-add-cloud-browser-to-build-graph.patch)
  hooks `//cloud-browser` into Chromium's build graph.
- [`0003-add-cloud-browser-webrtc-overrides.patch`](../../patches/0003-add-cloud-browser-webrtc-overrides.patch)
- [`0005-expose-host-frame-sink-manager.patch`](../../patches/0005-expose-host-frame-sink-manager.patch)
  exports `content::GetEmbedderHostFrameSinkManager()` so the embedder can
  bind a `FrameSinkVideoCapturer` without depending on `//content/browser`.

Patches 0001 (the renderer-side encoder-factory injection hook) and 0004
were **retired in M7 R6**: with the `PeerConnectionFactory` now built in the
browser process, the renderer-side hook is unreachable. Everything else
lives in our own source tree and links against unmodified upstream APIs.

## How the encoder factory plugs into libwebrtc

libwebrtc's `PeerConnectionFactory` is constructed from a
`PeerConnectionFactoryDependencies` struct that has a
`std::unique_ptr<VideoEncoderFactory> video_encoder_factory` slot.
Our `cloud_browser::CloudBrowserVideoEncoderFactory` (T19 — see
[`capture/encoder/encoder_factory.h`](../encoder/encoder_factory.h))
goes there.

In a from-scratch libwebrtc embedder this is a one-line wire-up. In
**Chromium**, the same factory is created deep in
`content::PeerConnectionDependencyFactory` and the slot is assigned
internally; we do not get a clean callsite. Patch `0001` makes that
callsite consult `ContentBrowserClient::GetWebRtcVideoEncoderFactory()`
before falling back to the built-in factory. Our embedder
(`cloud_browser_worker`) overrides the virtual to hand back a
`CloudBrowserVideoEncoderFactory` configured from flags.

```
                              +---------------------------------------+
                              |  cloud_browser_worker (this binary)   |
                              |                                       |
                              |  CloudBrowserContentBrowserClient     |
                              |     ::GetWebRtcVideoEncoderFactory()  |
                              |       returns                         |
                              |     CloudBrowserVideoEncoderFactory   |
                              |       (T19 / T35 / T36)               |
                              +-----------------+---------------------+
                                                |
            (patch 0001 adds this consultation) |
                                                v
   content::PeerConnectionDependencyFactory ----+
   (upstream, mostly unchanged)                  \
                                                  +--> libwebrtc PeerConnectionFactory
                                                       (uses our factory for
                                                        every PC's VP9 / H.264
                                                        encoder)
```

Capture (T47) plugs in symmetrically: a separate
`ContentBrowserClient` virtual hands a
`viz::FrameSinkVideoCapturer` consumer to a custom
`webrtc::VideoTrackSource`, again with a one-line consultation patch.
That patch lands in T47's implementation task — the design is
finalised in `docs/capture/framesink-design.md`; only the encoder
side is wired today.

## Build invocation

Once the build host is up (per `docs/build/chromium-from-source.md`):

```bash
# 1. Sync depot_tools / Chromium (one time, see chromium-from-source.md §1).
# 2. From your Chromium src/ root, symlink this repo as cloud-browser/:
ln -s /path/to/cloud-browser-webrtc cloud-browser

# 3. Apply the patch series.
bash cloud-browser/capture/build-integration/build.sh apply-patches

# 4. Generate gn build files, importing our args.
gn gen out/cb-release \
    --args='import("//cloud-browser/capture/build-integration/args.gn")'

# 5. Build.
autoninja -C out/cb-release cloud_browser_worker
```

`build.sh` wraps steps 3–5 plus light sanity checks. Run
`bash build.sh --help` for the inventory.

## Cross-references

- [`docs/build/chromium-from-source.md`](../../docs/build/chromium-from-source.md)
  (T17) — the build environment this scaffold targets.
- [`capture/spike-beginframe/findings.md`](../spike-beginframe/findings.md)
  (T29) — why we are committing to from-source builds at all.
- [`docs/capture/framesink-design.md`](../../docs/capture/framesink-design.md)
  (T47) — the capture side that will plug in alongside the encoder
  factory.
- [`capture/encoder/encoder_factory.h`](../encoder/encoder_factory.h)
  (T19) — the factory this binary instantiates.
- [`patches/README.md`](../../patches/README.md) — rebase strategy
  and per-patch rationale.

## What this scaffold does *not* do

- **Does not actually compile yet.** No T17 build host in this
  repo's CI; everything is design-by-spec.
- **Does not include the FrameSinkVideoCapturer wiring.** That is
  T47's implementation follow-up; the BUILD.gn target is shaped to
  accept it (`# TODO(T47): add capture/framesink/...`) without
  reorganisation.
- **Does not include the AV1 encoder.** That waits on T43's Phase 4
  prep; same reservation in BUILD.gn.

## Adding a new file to the build

Add the source file to `BUILD.gn`'s `sources` list, alphabetically.
That is the only place to edit; gn handles the rest. If a new file
needs a third-party dep that is not already listed
(`//third_party/libvpx`, `//third_party/webrtc`, etc.), add it under
`deps` with a one-line comment explaining the addition.

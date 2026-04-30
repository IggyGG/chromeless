# Chromium-from-source build environment

**Status:** Phase 2 prep (research only — no code yet).
**Audience:** the dev who will run the first end-to-end Chromium build for this
project, plus the infra-dev sizing the build machine.
**Cross-references:** `docs/prior-art/selkies.md`, `PROJECT_BRIEF.md` (Risks
table — pinning strategy).

We are committing to a Phase-2 hook into Viz's `FrameSinkVideoCapturer` and a
custom `webrtc::VideoEncoderFactory`. Both require a from-source Chromium
build. This document captures the build environment we want to standardize on
before anyone touches the code.

---

## 1. depot_tools

`depot_tools` is the umbrella for `gclient`, `gn`, `autoninja`, `git-cl`, and
the various Chromium-specific helpers. Install once per build host:

```bash
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git \
    "$HOME/depot_tools"
echo 'export PATH="$HOME/depot_tools:$PATH"' >> ~/.bashrc
export PATH="$HOME/depot_tools:$PATH"
```

Per the [official Linux build
instructions](https://chromium.googlesource.com/chromium/src/+/main/docs/linux/build_instructions.md)
the canonical first sync is:

```bash
mkdir -p ~/chromium && cd ~/chromium
fetch --nohooks chromium
cd src
./build/install-build-deps.sh             # Debian/Ubuntu apt deps
gclient runhooks                          # toolchains, sysroots, test data
```

**Caveats we will hit:**

- `fetch` does a full history clone (~40 GB) before any build artifacts.
- `gclient sync` clobbers `third_party/*`; do not edit those trees until
  a sync is committed in our patch series.
- `--with_branch_heads --with_tags` is required for any sync targeting a
  release branch or tag (see §6).
- `install-build-deps.sh` assumes Debian-family — aligns with our Phase 0
  base image. Budget ~3 GB extra apt cache.
- depot_tools auto-updates on every invocation. In CI pin a known good
  commit and set `DEPOT_TOOLS_UPDATE=0`.

## 2. `args.gn`

Recommended starting `args.gn` for our Phase-2 prototype branch (Linux,
Release, headless-capable, codecs enabled, suitable for capture and encoder
factory injection):

```gn
# Mode
is_debug                 = false
is_official_build        = false   # `true` is for shipping binaries; ours is a server.
is_component_build       = false   # one binary, simplifies deploy + symbol mgmt.

# Symbols / size
symbol_level             = 1       # enough for crashes; full symbols (2) ~doubles disk.
blink_symbol_level       = 0
v8_symbol_level          = 0
strip_debug_info         = false   # leave for now; revisit when we ship.

# Trim what a server doesn't need
enable_nacl              = false
enable_widevine          = false
enable_remoting          = false
use_cups                 = false
optimize_webui           = true

# Codecs (we need H.264 / AAC etc. inside the page, not just for our encoder)
proprietary_codecs       = true
ffmpeg_branding          = "Chrome"

# Headless / server-shaped
# headless_shell builds without GTK; chrome target keeps the full UI.
# We build both so we can A/B the capture path.
use_ozone                = true
ozone_platform_headless  = true
ozone_platform_x11       = true    # Phase 1 still uses Xvfb.
ozone_platform_wayland   = false   # not yet.

# WebRTC: keep all of it. Hardware codecs come later.
rtc_use_h264             = true
rtc_include_tests        = false
enable_libaom            = true
enable_dav1d_decoder     = true

# Toolchain
treat_warnings_as_errors = false   # rolls hurt less.
clang_use_chrome_plugins = true
use_sysroot              = true    # use the bundled sysroot, not host libs.

# Build cache (set when sccache is available — see §5)
# cc_wrapper             = "sccache"
```

Generate with:

```bash
gn gen out/Release --args="$(cat args.gn)"
```

We will keep `args.gn` checked into our patch repo so every build host
produces identical artifacts.

## 3. Build invocation

We need three targets, in roughly this order of usefulness to us:

| Target          | Why we need it                                             |
| --------------- | ---------------------------------------------------------- |
| `headless_shell`| Smallest harness for the DevTools-driven capture spike (Phase 1 alternative path in PROJECT_BRIEF.md). |
| `chrome`        | The full browser; needed once we wire `FrameSinkVideoCapturer` and per-tab capture. |
| `content_shell` | Useful for Viz / compositor experiments without the chrome/ layer. Optional. |

`autoninja` wraps `ninja` and picks parallelism based on `cc_wrapper`:

```bash
autoninja -C out/Release headless_shell
autoninja -C out/Release chrome
autoninja -C out/Release content_shell      # optional, when poking at Viz
```

Note: `headless_shell` is the modern replacement for the deprecated
`--headless=old` mode in `chrome` (removed in M132 — see [headless docs
landing](https://chromium.googlesource.com/chromium/src/+/main/headless/README.md)).
For our Phase 1 fallback ("native sidecar with DevTools
`HeadlessExperimental.beginFrame`"), `headless_shell` is the binary to point
at; `chrome` is where the real Phase 2 work lands.

## 4. Resource requirements

The official docs quote "8 GB RAM minimum, 100 GB disk" — that is the
absolute floor for a clean checkout that finishes a debug build, not a
realistic recommendation for our use.

**Our recommended sizing:**

| Resource | Recommendation                                      | Floor                       |
| -------- | --------------------------------------------------- | --------------------------- |
| RAM      | 32 GB+ (link step on `chrome` peaks ~16–24 GB)      | 16 GB + 32 GB swap (slow)   |
| vCPU     | 16+ (build is embarrassingly parallel until link)   | 8                            |
| Disk     | 200 GB SSD (checkout ~60 GB, `out/` ~50 GB, ccache ~30 GB, headroom) | 150 GB |
| Filesystem | ext4 or xfs; **avoid** networked filesystems (NFS), case-insensitive FS, or any FUSE layer for `src/`. | — |

**Wall-clock build time (Release, no cache):**

- AWS `c5.4xlarge` (16 vCPU / 32 GB): ~2 hours for `chrome`, ~30 min for
  `headless_shell` from a clean `out/Release/`.
- AWS `c5.9xlarge` (36 vCPU / 72 GB): ~50 minutes for `chrome`.
- Local M-series Mac under Docker: not supported as a primary build host;
  cross-compile path is out of scope for Phase 2.

Incremental builds after a small patch land in 1–10 minutes with a warm
ccache.

## 5. Build cache

| Option   | Verdict for us                                                                                 |
| -------- | ----------------------------------------------------------------------------------------------- |
| **ccache** | Cache ratio ~70–85%. Local-disk only; no cross-host sharing without a slow shared volume. Use as the floor. |
| **sccache** | Recommended. Drop-in `cc_wrapper="sccache"`. S3 / GCS backends supported, so one shared bucket serves all contributors + CI. |
| **reclient (RBE)** | Replaced Goma. Requires a remote-execution backend (Google RBE, BuildBuddy, EngFlow, self-hosted). 10–20× speedup but adds a backend service. Defer until build pain at team-of-4 scale. |
| **Goma** | Deprecated externally. Do not adopt. |
| **Siso** | New ninja-replacement orchestrator bundled with depot_tools and paired with RBE. Comes "for free" with reclient. |

**Recommendation for Phase 2:** sccache with an S3 backend, configured via
`cc_wrapper = "sccache"` in `args.gn` and an `RUSTC_WRAPPER`-style env var
on every build host. Revisit reclient when our average CI build time crosses
20 minutes.

## 6. Pinning strategy

PROJECT_BRIEF.md Risks: *"Pin to a release branch; track stable on a
schedule."* Translated into mechanics:

1. **Pin format.** We pin to `refs/branch-heads/NNNN` (the release branch),
   not a single tag. Branch-heads receive security cherry-picks; tags do
   not. Find the current branch number for, e.g., M125 stable on
   [chromiumdash.appspot.com/branches](https://chromiumdash.appspot.com/branches).
2. **`.gclient` snippet** for our build hosts:
   ```python
   solutions = [{
     "name"        : "src",
     "url"         : "https://chromium.googlesource.com/chromium/src.git",
     "managed"     : False,
     "custom_deps" : {},
     "custom_vars" : {"checkout_pgo_profiles": True},
   }]
   target_os = ["linux"]
   ```
   Then sync to a specific branch:
   ```bash
   gclient sync --with_branch_heads --with_tags \
                --revision src@refs/branch-heads/6367
   ```
3. **Roll cadence.** Track stable on a fixed schedule (every 4 weeks or
   every other milestone), not "when something looks broken". Each roll
   lands on a `roll/m-NNN` branch, runs the full harness, and is reviewed
   like any other change. Fix forward if a roll breaks the patch series;
   do not skip rolls.
4. **Patch series layout.** Changes live as `git format-patch`-style files
   under `patches/`, applied in order on top of the pinned commit. Mirrors
   how downstream forks (e.g. ungoogled-chromium, a Triform sibling repo)
   manage changes: small single-purpose patches, one per concern, with a
   top-level `series` file.
5. **CI gate.** Two builds per roll — `headless_shell` + `chrome`. Both
   must pass before merge.
6. **Security cherry-picks.** Between rolls, sync the branch (which
   auto-pulls Google's cherry-pick) and ship a point release.

## 7. Cross-reference: notes from T3 (Selkies)

`docs/prior-art/selkies.md` covers Selkies-GStreamer, which **does not build
Chromium from source.** They scrape an X11 framebuffer with `ximagesrc` (or
their `pixelflux` shim) and feed it to a GStreamer pipeline using
`webrtcbin` for transport. There is no Chromium patch series, no
`FrameSinkVideoCapturer` hook, and no encoder factory injection — they sit
entirely on top of stock Chromium running inside their container.

Implications:

- **No build-environment lessons to lift from Selkies** — their dev loop is
  a `pip install` + `apt install gstreamer*-plugins-*`, not a Chromium
  source build. We will be establishing this build environment from
  scratch, with the official Chromium docs as the reference.
- **It does, however, validate our divergence.** Selkies' choice not to
  patch Chromium is exactly why they cannot do per-tab capture, damage-rect
  routing, or encoder-factory injection. Owning this build environment is
  the price of doing the things we said we would do.

## 8. Open items

- Decide whether the CI build runs in a container (slower clean cycles, more
  reproducible) or directly on the runner (faster, more drift). Recommend
  container, on a sized `c5.9xlarge`-equivalent.
- Confirm sccache S3 backend ownership lands in `infra-dev`'s scope.
- Pick the first pin (likely the current stable as of the Phase 2 kickoff
  date) and capture it in this doc once chosen.

---

**Bottom line:** standardize on depot_tools + sccache + a 32 GB / 16 vCPU
build host, build `headless_shell` and `chrome` from
`refs/branch-heads/NNNN`, manage our patches as a numbered series, and roll
on a calendar — not on vibes.

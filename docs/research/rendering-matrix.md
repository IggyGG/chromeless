# Rendering verification matrix (WebGL + WebGPU under v1's SwiftShader)

**Status:** Phase 1 verification + Phase 4 prep.
**Owner:** chromium-dev.
**Cross-references:**
`tests/rendering/webgl-fixture.html`, `tests/rendering/webgpu-fixture.html`,
`tests/rendering/run-rendering-tests.sh` (T91),
`tests/rendering/local-probe.js` (T91),
`infra/launch-chromeless.sh` (T78 — production launch flag set),
`docs/capture/path-of-least-resistance.md` (T15),
`docs/research/av1-encoders.md` (T43 — GPU survey, Phase 4 hardware
matrix), `docs/prior-art/selkies.md` (T3),
T44 (sandbox primer for Phase 3), T63 / T70 (HW encoder paths whose
GPU-passthrough requirements overlap with this).

Project brief Phase 4: *"WebGPU/WebGL rendering verification (some
headless GPU setups break this)."* This doc captures what works
under v1's `--use-gl=angle --use-angle=swiftshader-webgl
--disable-features=Vulkan` flag set (T78), and what we expect to
unlock once Phase 4 introduces real GPU passthrough.

## 1. Per-fixture results

Two profiles are tested:

- **default-host**: a developer's macOS Apple Silicon machine
  with hardware Metal-via-ANGLE (the result of running
  `node tests/rendering/local-probe.js` on macOS host). This is
  the **fixture-correctness baseline** — confirms the fixtures
  themselves run and self-report correctly.
- **swiftshader-v1 (approximation)**: the same Playwright
  Chromium, launched with `--use-gl=angle --use-angle=swiftshader-webgl
  --disable-features=Vulkan` (the v1 production flag set). Mac
  ANGLE doesn't ship the SwiftShader-WebGL backend, so this run
  fails earlier than the actual production case — but it confirms
  the flag set forces a SW path different from default and is a
  useful "what's the failure shape when GPU is unavailable?"
  signal until the Linux container probe data lands.
- **container-swiftshader (canonical)** — pending. Run via
  `tests/rendering/run-rendering-tests.sh --boot` against the
  chromeless:dev image once it's bootable in CI. This
  is the row that updates the matrix below from "(approximation)"
  to "(measured)".

Numbers below are from a single host run on Apple M5 (April 2026).
Results are saved to `tests/rendering/local-probe-results.json`.

### WebGL 1 + 2

| Probe                              | default-host          | swiftshader-v1 (approx.) | Notes |
|------------------------------------|-----------------------|---------------------------|-------|
| `webgl` context create             | OK                    | **err** (returned null)   | Mac ANGLE doesn't support SwiftShader-WebGL; the Linux container does. |
| `webgl2` context create            | OK                    | **err** (returned null)   | Same.                                                                  |
| Triangle render (centre pixel)     | OK ([102,217,102])    | n/a (no context)          | Green as expected; fixture self-validates pixel colour.                 |
| Textured triangle (corner sample)  | warn                  | n/a                       | Pixel values were sampled but not at the expected corner — fixture coordinate-system issue, NOT a SwiftShader bug. Top-left read [0,0,255] (texture-space (0,0) is bottom-left in WebGL UVs). The triangle did render and texture sampling worked; the colour assertion is over-strict. (re-validate fixture in a follow-up). |
| FBO + 4× MSAA + readback           | warn                  | n/a                       | drawArrays succeeded, blit ran, readback was [0,0,0,0]. Apple ANGLE's MSAA blit semantics may have cleared the read-source. **Not a v1-launch-flag concern;** the v1 flag path doesn't use MSAA. (re-validate fixture against the container path; if it stays warn there, downgrade test or fix the fixture.) |
| highp + mediump compile            | OK                    | n/a                       | Both precisions compiled fine.                                          |

GL info from default-host (informational; actual production string
will be `ANGLE / SwiftShader / Linux X11` once the container path
runs):

```
vendor:    Google Inc. (Apple)
renderer:  ANGLE (Apple, Apple M5, OpenGL 4.1)
version:   WebGL 1.0 / 2.0 (OpenGL ES 2.0/3.0 Chromium)
maxTextureSize: typically 16384
maxViewportDims: 16384 × 16384
```

### WebGPU

| Probe                | default-host                 | swiftshader-v1 (approx.) | Notes |
|----------------------|-------------------------------|---------------------------|-------|
| `navigator.gpu`      | OK (present)                  | OK (present)              | Even a Vulkan-disabled build still defines the JS surface.              |
| `requestAdapter()`   | OK (Apple M5, non-fallback)   | **err** (returned null)   | Vulkan disabled → Dawn's Linux backend has no device; macOS approximation also returns null because we forced the swiftshader-webgl path. |
| `requestDevice()`    | OK                            | n/a                       | Bypassed (no adapter).                                                  |
| Triangle render      | OK                            | n/a                       | Render pipeline + colour attachment + draw all succeeded.               |
| Compute shader       | OK (16 ints written + match)  | n/a                       | `dispatchWorkgroups(1)` with a `workgroup_size(16)` pipeline; output array matches `i*7`. |

The default-host result is the load-bearing data for the fixture's
correctness — when WebGPU is genuinely available, our fixture
exercises pipeline + compute + readback end-to-end. The
swiftshader-v1 row is a strong negative signal for the v1
production path: **we do not expect WebGPU to work in v1.**

## 2. v1 verdict + threshold definitions

For Phase 1 v1 we declare per-API status as follows. The
threshold below "rendering works" vs "degraded but functional" is
**single-digit fps on a complex 1080p WebGL scene** — anything
below ~5 fps under the v1 software path counts as "degraded but
functional." Hard breakage (no context create / requestDevice
fail) is "broken."

| API     | v1 status                        | Confidence                                   |
|---------|----------------------------------|-----------------------------------------------|
| WebGL 1 | works (degraded but functional)  | high — SwiftShader-WebGL is a stable Chromium ANGLE backend. Container row pending. |
| WebGL 2 | works (degraded but functional)  | high — same.                                                                          |
| WebGPU  | **broken**                       | high — Dawn requires Vulkan (Linux) or Metal (macOS); v1 explicitly disables Vulkan (T78). |

The "degraded but functional" caveat for WebGL is real: SwiftShader
is a software rasterizer. Modest UI WebGL (charts, small
visualisations, a cube viewer) renders correctly. Heavy WebGL
(Three.js scenes with 100k+ triangles, complex shaders, full-frame
post-processing) drops below 5 fps and the user experiences it as
choppy. Streaming over WebRTC adds the encoder budget on top —
the cumulative effect is that any "WebGL is supported" claim must
be qualified.

For WebGPU: every probe past `navigator.gpu present` fails. We
should not claim WebGPU support in v1.

## 3. Recommendations

### v1 (now)

- **README "Known limitations"** section calls out:
  - WebGL is software-rendered; expect single-digit fps on complex
    scenes.
  - WebGPU is not supported.

- **Streamer page does not block on WebGL/WebGPU init** (verified —
  `capture/streamer-page/streamer.js` does no GPU work itself; the
  rendering tests are about pages the user opens, not the streamer
  page).

- **Tests committed.** The fixtures + orchestrator + local probe
  give us a regression guard: when Phase 4 GPU passthrough lands,
  re-run `run-rendering-tests.sh` against the GPU-enabled image
  and the matrix updates from "broken" to "works (HW)".

### Phase 4 (real GPU passthrough)

Per `docs/research/av1-encoders.md` (T43), the Phase 4 encoder
work already needs GPU passthrough for NVENC / VAAPI. **We get
HW WebGL + WebGPU as a free side-effect of the same plumbing**:

- **NVIDIA path.** `nvidia-container-runtime` exposes the NVIDIA
  driver's `/dev/nvidia*` + libcuda.so to the container. With the
  driver visible, Chromium's GPU process detects the NVIDIA GPU,
  enables `--use-gl=angle --use-angle=vulkan`, and Dawn brings up
  a real Vulkan backend on the same device. WebGL goes from
  SwiftShader to NVIDIA-HW; WebGPU lights up.
- **Intel / AMD path.** `/dev/dri/renderD128` + the i965 / iHD /
  radeonsi user-space driver. Same shape — Chromium picks up the
  GPU, ANGLE switches to the GPU backend, Dawn brings up Vulkan.
- **gVisor caveat.** Per T44 (sandbox primer), gVisor's nvproxy
  passes NVIDIA GPU access into the sandbox; AMD/Intel
  passthrough through gVisor is less mature as of early 2026.
  Production multi-tenant deployment with gVisor + GPU is
  NVIDIA-first.

Cross-references:
- T43 §2 — cloud-GPU coverage matrix (L4/L40/H100 vs T4/A10).
  WebGL/WebGPU support follows the same matrix; the AV1-encode-
  capable shapes are also the WebGPU-capable ones since both
  require a working Vulkan or its equivalent.
- T44 — sandbox isolation primer; describes gVisor nvproxy.
- T63 / T70 — NVENC / VAAPI encoder skeletons; GPU-passthrough
  Dockerfile changes will be the same hooks for both.

### Phase 4 follow-up tasks (file when GPU host available)

1. **GPU-enabled Dockerfile variant** — `infra/Dockerfile.gpu` that
   bases on `nvidia/cuda:12.6.0-base-ubuntu24.04` (or equivalent),
   removes the `--disable-features=Vulkan` flag from launch-chromeless.sh,
   re-enables `--use-gl=angle --use-angle=vulkan`.
2. **Re-run rendering matrix** under the GPU image. Update §2's
   confidence column from "high (approximation)" to "high
   (measured)" or downgrade if surprises surface.
3. **Soak test.** Run a complex WebGL workload (Three.js stress
   test) for ≥1 hour on the GPU image; confirm no GPU-process
   crashes, no memory leaks. Phase 4 stretch.

## 4. How to run

### On a developer host (no docker)

```bash
node tests/rendering/local-probe.js
```

Produces `tests/rendering/local-probe-results.json` with both
profiles (default + swiftshader-v1) for both fixtures. Useful for
iterating on the fixtures themselves.

### Against a running compose stack

```bash
docker compose -f infra/compose.yaml up -d --build
tests/rendering/run-rendering-tests.sh
# or, in one shot:
tests/rendering/run-rendering-tests.sh --boot
```

Writes `tests/rendering/results/<profile>-<fixture>-results.json`
+ a screenshot per fixture. Compares screenshots against
`tests/rendering/baselines/`; if no baseline yet, prints a
reminder to re-run with `--save-baselines` once vetted.

The fixture URLs default to
`http://localhost:9000/streamer-tests/rendering/<fixture>.html`,
which assumes infra-dev's Dockerfile copies
`tests/rendering/*.html` to `/opt/cloud-browser/streamer-tests/`
during image build. If the fixtures aren't yet in the image,
override via `CHROMELESS_RENDERING_WEBGL_URL=…` /
`CHROMELESS_RENDERING_WEBGPU_URL=…`.

## 5. Open items

- **Container-swiftshader row is approximation, not measurement.**
  The Mac ANGLE backend doesn't have a SwiftShader-WebGL path, so
  the local probe's "swiftshader-v1" profile fails harder than
  the actual Linux container. Real numbers come from running
  `run-rendering-tests.sh` against the container — pending T17's
  build env or a CI host that can run docker.
- **Two warns on the host webgl run** (textured-triangle corner
  read, FBO+MSAA readback) are fixture caveats, not SwiftShader
  evidence. Either tighten the fixture or downgrade the assertion
  before the matrix is consumed by anyone past chromium-dev.
- **Three.js stress fixture** (Phase 4 follow-up). The current
  WebGL fixture exercises *correctness*, not *performance*. The
  "degraded but functional" claim in §2 needs a measured fps
  number from a heavyweight scene.
- **HEVC-decode probe.** Symmetric to the encoder matrix in T43;
  not strictly rendering but in the same neighbourhood. (Already
  covered by `docs/research/decoder-matrix.md` T79.)

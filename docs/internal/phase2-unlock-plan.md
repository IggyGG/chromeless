# Phase 2 unlock plan — what runs the day T17 build env runs

**Status:** sequencing plan. Estimates assume one focused engineer
with admin on a Linux build host + cloud GPU access.
**Owner:** chromium-dev.
**Cross-references:**
`docs/build/chromium-from-source.md` (T17 — build env spec),
`capture/build-integration/README.md` (T49 — build scaffold),
`patches/README.md` (T49 — patch series),
`docs/capture/encoder-factory-catalog.md` (T97 — full factory
inventory),
plus every per-encoder rationale doc cited by name below.

When someone provisions the build host, **a lot flips at once**.
Today every encoder file in `capture/encoder/*.cc` carries a
`TODO(T17-build-env): validate compile + link` marker. Same in
`capture/framesink-capturer/capturer.cc` and the build-integration
target files. This doc says what to do first when those markers
become actionable, in what order, and roughly how long each step
takes.

## 1. Pre-conditions

Per T17 §4 + §5:

- **Host.** Debian 12 or Ubuntu 22.04 LTS, 32 GB RAM (16 GB +
  swap is the floor; the link step on `chrome` peaks 16–24 GB),
  16+ vCPU, 200 GB SSD. AWS `c5.4xlarge` or equivalent.
- **depot_tools.** `git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git`,
  PATH set, `DEPOT_TOOLS_UPDATE=0` in CI.
- **sccache + S3 backend** OR a local cache. `cc_wrapper="sccache"`
  in args.gn (per T17 §5; we recommended sccache over reclient
  for our scale until average CI build crosses 20 minutes).
- **Chromium source** pulled at the pinned `refs/branch-heads/NNNN`
  (T17 §6 picks the first pin; `gclient sync --with_branch_heads
  --with_tags --revision src@refs/branch-heads/NNNN`).
- **This repo** symlinked into Chromium's `src/` as
  `//cloud-browser` (per `capture/build-integration/README.md`
  "Build invocation").

## 2. Day 1 — first compile

The single highest-leverage day. Output: a `cloud_browser_worker`
binary exists.

```bash
# From the Chromium src/ root.
bash cloud-browser/capture/build-integration/build.sh apply-patches
gn gen out/cb-release \
   --args='import("//cloud-browser/capture/build-integration/args.gn")'
autoninja -C out/cb-release cloud_browser_worker
```

Expect 4–8 hours on a clean 16-core box (per T17 §4 wall-clock
table). 1–2 hours with sccache warm. The first build is the worst
build; everything after it is incremental.

What flips after Day 1:

- `TODO(T17-build-env)` in `encoder_factory.{h,_stub.cc}` (T19) —
  factory contract compiles cleanly. ✓
- Same markers in `vp9_encoder.{h,cc}` (T35), `h264_encoder.{h,cc}`
  (T36), `bwe_adapter.{h,cc}` (T58), `simulcast_factory.{h,cc}`
  (T83) — these have no external SDK gates beyond libwebrtc itself.
- `framesink-capturer/capturer.{h,cc}` (T55) — Viz mojom + libwebrtc
  headers resolve.
- Patch `0001-expose-encoder-factory-injection.patch` (T49) — the
  context lines either match or need rebasing onto the pinned
  branch's actual files.

**Failure modes to expect:**
- Patch context drift on `peer_connection_dependency_factory.cc`
  (T49 patch is shape-stable; concrete line numbers drift across
  Chromium versions).
- Header-include drift in `nvenc_encoder.cc` / `vaapi_encoder.cc`
  / `svtav1_encoder.cc` — these are stub-compiled today; if a
  libwebrtc API rotated under us we'll find out here.

If the build succeeds: write the result down in `tests/build/`
(or wherever) and tag the commit. The first green build is the
single biggest milestone of Phase 2.

## 3. Day 2–3 — validate the encoder factory

Run the unit tests T19/T35/T36/T58/T83 already wrote:

```bash
autoninja -C out/cb-release cloud_browser_encoder_unittests
out/cb-release/cloud_browser_encoder_unittests
```

The always-on tests (per `docs/capture/encoder-factory-catalog.md`
T97 §5) should pass first try — they have no external-SDK gates.
Numbers per file:

- `bwe_adapter_test.cc` — 11 (incl. TSAN canary)
- `vp9_encoder_test.cc` — 6
- `h264_encoder_test.cc` — 7
- `simulcast_factory_test.cc` — 11
- `nvenc_encoder_test.cc` — 4 always-on + 5 HAS_NVENC-gated
- `vaapi_encoder_test.cc` — 4 always-on + 5 HAS_VAAPI-gated
- `svtav1_encoder_test.cc` — 3 always-on + 5 HAS_SVT_AV1-gated

Total **~52 tests** to triage. Most should be green; expect 1–3
to need fixture tweaks (the VP9 textured-corner read and FBO+MSAA
warns on the local probe (T91 §5) are a good prior — fixture
caveats, not real bugs).

Same day, run the FrameSink capturer tests:

```bash
autoninja -C out/cb-release cloud_browser_framesink_capturer_unittests
out/cb-release/cloud_browser_framesink_capturer_unittests
```

(7 gtests, T55: Start forwards, frame delivery, Done() RAII
contract, multiple-frames-acked, dropped-frame surfacing, Done-on-
wrap-failure, Stop forwarding.)

Triage failures, file follow-up tasks. Allocate 2 calendar days.

## 4. Day 4–5 — FrameSinkVideoCapturer end-to-end

Now the interesting part: replace the streamer-page's
`getDisplayMedia` (T15 + T86's fake-media stop-gap) with
embedder-backed FrameSink capture per T47's design.

Sequence:

1. Build the embedder glue (`capture/build-integration/embedder/`
   per T49 BUILD.gn) — `BrowserMainParts`,
   `ContentBrowserClient` subclass that overrides
   `GetWebRtcVideoEncoderFactory()` to return our factory, plus
   the equivalent for the capturer (a `media::VideoFrame` source
   wired into `webrtc::PeerConnectionFactoryDependencies`).
2. Wire the existing `CloudBrowserFrameSinkCapturer` (T55) into
   the embedder so its `OnFrame` callback feeds a
   `webrtc::VideoTrackSource` we install on the
   PeerConnectionFactory.
3. Update `infra/launch-chromeless.sh` to invoke the new
   `cloud_browser_worker` binary instead of stock `chromium`.
4. Drop `CHROMELESS_USE_FAKE_MEDIA=1` from `infra/compose.yaml`. The
   real capture path is now the production path.

**First measurement.** Compare against T29 spike's screencast
baseline (p50 17.9 ms, p99 32.8 ms, stdev 9.85 ms — that's the
Mac-laptop baseline; we expect significantly tighter numbers on
the Linux build host's actual production target). The single
load-bearing claim from T47 §4 is "≥30 ms p95 reduction over
`getDisplayMedia` on the same hardware." If we don't see it, T47
§4's exit criteria say we stay on `getDisplayMedia`.

Allocate 2 calendar days; budget for an extra 1–2 if the
embedder glue surfaces churn.

## 5. Day 6+ — HW encoder integration

In dependency order:

### NVENC (T63)

- Provision an NVIDIA L4 cloud instance (per T43's footnote: L4 is
  the cheapest AV1-capable shape on AWS today).
- Install NVIDIA Video Codec SDK (`nvenc_sdk_path` in args.gn —
  per `capture/build-integration/BUILD.gn` config(`nvenc_sdk`)).
- Rebuild with the SDK path set: `HAS_NVENC` flips, the
  HAS_NVENC-gated tests in `nvenc_encoder_test.cc` start firing
  against real hardware.
- Bench encode latency p50/p95/p99 at 1080p; T63's nvenc-tuning-
  rationale.md §"Observability / future work" item 1 is what we're
  populating.

Effort: 1–2 days assuming GPU access is sorted.

### VAAPI (T70)

- Provision an Intel Arc box (or run on the AMD MI300X box if we
  have one). Install `libva-dev` + `intel-media-va-driver`
  (Intel) or `mesa-va-drivers` (AMD).
- Rebuild with `vaapi_libdir` set; `HAS_VAAPI` flips.
- Run the gated tests; bench.

Effort: 1–2 days. Less mature than NVENC in published benches,
expect more triage.

### SVT-AV1 (T75)

- `apt install libsvtav1enc-dev` (Debian 12 ships it).
- Rebuild with `svt_av1_libdir=/usr` (per T75 BUILD.gn).
  `HAS_SVT_AV1` flips; gated tests run.
- Bench against the M8 vs M9 quality / latency tradeoff (T75
  §"Observability" item 2 — re-validate the M8 default on
  representative content).

Effort: 1 day. No GPU dependency.

## 6. First real demo

Once Day 4–5's FrameSink path is green AND at least one encoder
chain compiles + passes its always-on tests, we can run the **first
non-synthetic Phase-1-replacement demo**:

- `cloud_browser_worker` boots in the production container (per
  the modified compose.yaml of step 4).
- Real screen capture from a real Chromium tab.
- VP9 SW encode (the no-HW-dependency path; default codec).
- libwebrtc transport, real client, real video on the user's
  screen.

The harness (T10 / T11 / T39 / T65) takes the **v1-defining
glass-to-glass measurement** against this stack. PROJECT_BRIEF.md
targets are LAN <100 ms, regional <200 ms; the first time we have
real numbers against the real path is right here.

Allocate 1 day for first demo + harness run.

## 7. Calendar estimate

| Step                                         | Calendar days |
|----------------------------------------------|---------------|
| Day 1: first compile                         | 1             |
| Day 2–3: encoder factory + capturer tests    | 2             |
| Day 4–5: FrameSink capture e2e               | 2 (+1–2 buffer) |
| Day 6–10: HW encoder integration (NVENC + VAAPI + SVT-AV1)  | 3–6           |
| First real demo + harness measurement        | 1             |
| **Total** for one focused engineer with cloud GPU access | **~2 weeks**  |

Two weeks is the minimum-viable Phase 2 turn-up. Realistically
+1 week of slack for the unexpected — patch context drift, libvpx
API churn, NVENC SDK version mismatch, the usual "compile against
upstream" tail.

## 8. What does NOT unlock with T17 build env

Important to be honest about scope. T17 unblocks the
**code-authored → code-running** transition for what's already
written. It does not magically unlock work that hasn't started:

- **Damage-rect partial-frame encoding (T85).** Design only
  today. Implementation is per-encoder code that lands *after*
  Phase 2 first-demo proves the rest of the pipeline works.
  Effort estimate from T85 §6: ~50–100 LOC per codec + tuning
  iteration. (re-validate the per-codec QP delta and refresh
  period numbers on real content.)
- **Multi-region deployment (T87).** Gated on K8s clusters in
  multiple regions, TURN-REST federation, signaling cross-region
  routing. T17's build env doesn't speed this up.
- **Phase 4 HDR (T98).** Encoder-side change is small (see T98
  §6) but capturer-side 10-bit emission is ~1–2 weeks of its own
  uncertain work, gated on customer demand anyway.
- **Phase 4 GPU passthrough at scale (T95).** First node with
  GPU is unlocked by Day 6+ above; per-tenant routing across
  MIG slices vs Kata+VFIO cells is the Phase 3 controller work
  T71 will do later.
- **AV1 SVC, custom rendering, advanced ABR.** Phase 4 polish.
  None of it is in the encoder factory today; T83 simulcast
  wrapper is the closest predecessor and explicitly excludes
  SVC (T83 §non-goals).

## 9. Cross-references

- **T17** — build env spec; the prerequisite for everything
  in this doc.
- **T19 / T35 / T36 / T58 / T83** — encoder factory + per-codec
  SW encoders + BWE + simulcast wrapper; the `TODO(T17-build-env)`
  markers in these flip on Day 1.
- **T47 / T55** — FrameSink capturer design + skeleton; Day 4–5
  populates them.
- **T49** — build-integration scaffold, including patch
  `0001-expose-encoder-factory-injection.patch` we apply on
  Day 1.
- **T63 / T70 / T75** — HW + SW AV1 encoders; Day 6+ for the
  HAS_* gates.
- **T85** — damage-rect design; deferred to post-Phase-2-demo.
- **T95** — GPU passthrough recommendation matrix; the
  fleet-shape choices for Phase 3 once Phase 2 is green.
- **T96** — webrtc-dev's signaling buffer fix (Phase 1 demo
  unblock); independent of this plan but referenced because the
  first-real-demo of step 6 above depends on it.
- **T97** — encoder factory catalog; the layer above this plan.
- **T98** — HDR research; explicitly NOT unlocked by T17.
- **T101** — this doc.

---

**Bottom line:** ~2 weeks of focused engineer time + cloud GPU
access turns nine months of authored code into a running
production-shape Phase 1.5 stack. Every step has a measurable
exit criterion. The single load-bearing event in the timeline is
**Day 1's first green build** — once that's done, every other
step is mechanical triage of work we've already designed.

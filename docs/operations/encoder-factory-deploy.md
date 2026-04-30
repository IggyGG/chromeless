# Encoder factory deploy procedure

**Status:** ops recipe. Operational handoff between the
engineering-side build sequencing in T101 and the deployment
runbook in T80.
**Audience:** the on-call engineer shipping a real-encoder-enabled
`cloud_browser_worker` to a running stack.
**Cross-references:**
`docs/internal/phase2-unlock-plan.md` (T101 — engineering
sequencing),
`docs/operations/phase1-deployment-checklist.md` (T80 —
deployment runbook this layers onto),
`docs/operations/runbook.md` (T80 sibling — incident response),
`docs/capture/encoder-factory-catalog.md` (T97 — Config knob
reference),
plus per-codec rationale docs cited inline.

T101 says "compile the binary and validate the encoder factory."
T80 says "ship a Phase 1 stack." This doc says "now ship the
Phase-1.5 binary T101 produced into the Phase 1 stack T80
deployed, observe it, and roll it back if needed."

## 1. Pre-conditions

Tick before starting:

- [ ] `cloud_browser_worker` binary exists and passed the
      Phase-2-unlock validation (T101 Day 1–3). Tag exists in
      our image registry per the T84 CI/CD pipeline.
- [ ] At least one of: `enable_vp9`, `enable_h264`, `enable_svt_av1`
      is true in the build's `args.gn` (defaults from
      `capture/build-integration/args.gn` give VP9 + H.264 SW; AV1
      requires linking libSvtAv1Enc per T75).
- [ ] HW prefer flags decided per region — see §3 below. The
      runtime probe handles unavailable hardware safely; you can
      flip prefer flags on without the SDK present.
- [ ] Per-tenant config decisions reviewed (T67 namespacing). The
      `Config` is shared by every session in a `BrowserSessionPool`
      slice today; per-tenant differentiation lives in the
      controller's pool-routing decision.
- [ ] Helm values updated: `imageTag` rolled to the new build;
      `encoderConfig.preferNvenc*` set per node-pool (per-region
      defaults from T93 / T95 once they land).

## 2. Deploy procedure — single tenant, single region

Follow this for the first real-encoder rollout in any region.
Subsequent rollouts in the same region are mechanical Helm
upgrades.

### Stage A — image build

`gn gen` with the production args.gn (T17 / T49 sequencing) and
`autoninja -C out/cb-release cloud_browser_worker`. The CI pipeline
from T84 runs this on tag push; `--cb_build_with_real_encoders=true`
just means `args.gn` doesn't override `enable_vp9 = false` etc.

The image-build artefact carries the Chromium pin, the
patch-series version (`patches/0001-...`), and the linked codec
versions. Tag the image with all three (e.g.
`cloud-browser-worker:branch-heads-NNNN-patches-rev-N-vpx1.13`)
so a rollback target is obvious.

### Stage B — single-node canary

Pin the new image to **one node** in the target region by
labelling that node with `cb.cloud-browser/canary=encoder-factory-N`
and routing exactly N tenants there via the
`BrowserSessionPool` controller's pool-pinning knob (T71 ships
this; review before this rollout).

```bash
kubectl label node <node> cb.cloud-browser/canary=encoder-factory-1
helm upgrade cloud-browser ./infra/helm/cloud-browser-webrtc \
  --set image.tag=branch-heads-NNNN-patches-rev-N-vpx1.13 \
  --set canary.encoderFactory.enabled=true \
  --set canary.encoderFactory.tenantCount=2
```

Two tenants is enough to surface the obvious problems
(connection-establishment, per-codec correctness, observable
metrics) without exposing real-customer traffic to a fresh build.

### Stage C — observe

Watch the dashboards from T66 for the canary node's region +
session labels:

- **`cb-cluster-overview`** for the rollout view: encoder fps,
  dropped frames, qp distribution, bytes/s, RTT, all
  region-filtered (T94 confirmed every encoder metric carries a
  `region` label).
- **`cb-session-detail`** for each canary session: per-session
  fps, qp, packet loss, client-side fps lag.

Compare against the **synthetic-media baseline**: T65 produces
the v1-defining glass-to-glass number once it lands; until then
the synthetic baseline from the existing harness runs is the
reference (typically ~30 ms p95 encode-to-display under fake
media, per T29's spike measurements adjusted for the production
host's hardware).

Acceptance signals for moving past Stage C:

- Per-session fps holds within ±2 of the configured target for ≥
  10 minutes.
- qp p95 < 38 (any sustained ≥ 38 means encoder saturation; check
  bitrate ceiling).
- Dropped-frames-per-second is < 1.0 over the same window.
- BWE-driven bitrate within ±10% of the target the adapter
  publishes (T58).
- No `CBChromiumEncoderStalled` or `CBVideoBitrateDeviation`
  alerts (T80).

### Stage D — gradual expansion

Once Stage C passes for **24 hours of soak** with at least 50
session-hours observed:

- Expand canary to 10% of the pool's nodes for ≥ 7 days.
- If the bake remains clean, flip the default image-tag in the
  Helm values for the region.

The 24h / 1-week schedule mirrors the Chromium roll cadence
(T17 §6) — a deliberate echo so the on-call engineer's mental
model is consistent.

## 3. Per-encoder enable procedure

Each encoder is gated by build-time link + runtime config. The
table below collapses §3 of the task brief plus the catalog (T97)
into one operational reference.

| Encoder         | Build-time link required          | Runtime flag                     | Default | Hardware required                   |
|-----------------|------------------------------------|----------------------------------|---------|--------------------------------------|
| VP9 SW (T35)    | libvpx (in `args.gn` already)      | `enable_vp9`                     | on      | none                                 |
| H.264 SW (T36)  | x264 (in `args.gn` already)        | `enable_h264`                    | on      | none                                 |
| SVT-AV1 SW (T75)| libsvtav1enc-dev (Debian package)  | `enable_svt_av1`                 | on (Phase 4 stretch — runtime probe still gates) | none |
| NVENC (T63)     | NVIDIA Video Codec SDK + libcuda   | `prefer_nvenc_{h264,hevc,av1}`   | off     | Turing+ for H.264/HEVC; Ada+ for AV1 (per T43)|
| VAAPI (T70)     | libva-dev + libva-drm-dev          | `prefer_vaapi_{h264,hevc,av1,vp9}`| off    | Intel iGPU / Arc; AMD RDNA1+ (per T70 matrix) |

The runtime probes (`NvencEncoder::ProbeAvailable`, etc., T63 /
T70 / T75) **handle unavailable hardware safely** — a
`prefer_nvenc_av1=true` on a node without an NVENC AV1 GPU
silently falls back to the SW path. You can flip prefer flags on
across the fleet and let the runtime probe pick the right path
per node. **Do not** drop SW codec enables to "force" HW use —
SW is the unconditional fallback that keeps the session
connecting when HW probes fail at runtime mid-session.

Per-region recommended defaults (interim, until T93 ships
region-aware controller routing):

- **us-east-1, us-west-2 GPU pools (L4/L40/H100):**
  `prefer_nvenc_h264=true`, `prefer_nvenc_av1=true`.
- **eu-west-1 T4 pool:** `prefer_nvenc_h264=true` only (T4 has
  no AV1; T43 §2 cloud-GPU footnote).
- **Intel Arc / AMD RDNA pools:** `prefer_vaapi_h264=true`, plus
  `prefer_vaapi_av1=true` on Arc / RDNA 3+.
- **CPU-only pools:** all prefer flags off.

## 4. Rollback procedure

Two layers. Pick the lighter one when both work.

### Image-level rollback

When the new build is broken in a way that affects every codec:

```bash
helm upgrade cloud-browser ./infra/helm/cloud-browser-webrtc \
  --set image.tag=<previous-tag> \
  --reset-values=false
```

Existing sessions on the rolled-back nodes disconnect at session
end (no in-place upgrade); new sessions pick up the previous
image. **~30-second blast radius** for a single-node canary;
proportionally larger on a wider rollout but capped by session
turnover.

### Encoder-config-level rollback

When one HW path is broken but the rest are fine — say, NVENC AV1
crashes on a specific driver but NVENC H.264 + the SW paths are
healthy:

```bash
# Disable just NVENC AV1 across the fleet:
helm upgrade cloud-browser ./infra/helm/cloud-browser-webrtc \
  --set encoderConfig.preferNvencAv1=false \
  --reset-values=false
```

No image rebuild. The change propagates via `BrowserSessionPool`
controller config refresh (T71's reconcile loop picks it up
within ~30 s). New sessions skip the broken path; existing
sessions stay on whatever they negotiated. Useful when a vendor
driver regresses but you don't want to give up the rest of the
factory.

The encoder-config rollback is **strictly cheaper** than image
rollback. Reach for it first when the diagnosis points at a
single codec / HW path.

## 5. Observability checklist

For every rollout stage:

- **Per-encoder fps / qp / bytes_sent / dropped frames** —
  visible on `cb-session-detail` (T66). Verified region-tagged
  via T103 review of T94.
- **Per-codec negotiated rate** — T54's `codec_fallback` events
  reach the metrics sidecar via the stats data channel (T72);
  surface as `cb_webrtc_codec_fallback_total{from,to}`. Watch for
  unexpected fallbacks (e.g., AV1 → H.264 means the runtime probe
  flipped after session start).
- **BWE bitrate target vs observed** — T58's BweAdapter
  metrics-sink hookup is build-env-gated (per T58 §7 open items
  and T103 review notes); when it lands, watch for sustained
  divergence > 10% which means damage-rect / pacer / SVC
  interaction is misbehaving.
- **Latency p50 / p95** — T65 harness numbers fed into a regression
  budget; alert when p95 exceeds the brief's target (LAN <100 ms,
  regional <200 ms).
- **Alerts already in T80 alert rules:** `CBChromiumEncoderStalled`,
  `CBClientFpsLow`, `CBVideoBitrateDeviation`.

## 6. Common deploy failure modes

Top three observed in pre-prod or pre-flagged from the encoder
docs:

1. **HW probe says OK but `CreateVideoEncoder` fails at runtime.**
   Driver loaded but unhealthy (NVENC SDK ABI mismatch, MIG
   profile not assigned, VAAPI driver missing the requested
   profile). Symptom: `CBChromiumEncoderStalled` plus
   `cb_webrtc_codec_fallback_total{from="VP9",to="H264"}`
   incrementing. **Mitigation:** the runtime probe + T54
   fallback already handles it gracefully — confirm via metrics,
   then disable the broken HW path via encoder-config rollback
   (§4). File a follow-up against T63 / T70 to harden the probe
   itself.
2. **SDK ABI mismatch.** Image built against NVIDIA SDK 13.0, host
   driver is 12.5 (or vice versa). The probe **should** fail
   clean (return false from `ProbeAvailable`); if it returns true
   then crashes inside `nvEncOpenEncodeSessionEx`, that's a probe
   bug. **Mitigation:** image-level rollback + NVENC follow-up.
3. **Simulcast mid-session resolution change.** T83 wraps every
   codec in `SimulcastEncoder` when `enable_simulcast=true`; some
   per-codec inner encoders (notably libvpx VP9) reject mid-stream
   resolution changes outside a keyframe boundary. Symptom:
   per-session encode-failure metric spikes, RTP stream gap on
   one of the rids. **Mitigation:** simulcast wraps already log
   per-layer Encode failures (T83 — "one layer's Encode failure
   does not abort the others"). Confirm via per-rid metrics
   (post-T83 follow-up tracked separately). For now, disable
   simulcast for the affected tenant via the per-tenant Config
   patch path.

## 7. Cross-references

- **T17** — build env spec; the prerequisite for any real-encoder
  binary.
- **T19** / **T35** / **T36** / **T54** / **T58** / **T63** /
  **T70** / **T75** / **T83** — encoder factory + per-codec
  encoders + multi-codec fallback + BWE adapter + simulcast
  wrapper. Catalogued in T97.
- **T66** — Grafana dashboards (`cb-cluster-overview`,
  `cb-session-detail`).
- **T67** — per-tenant signaling namespacing.
- **T80** — Phase 1 deployment checklist + operational runbook;
  this doc layers on top.
- **T82** — WebRTC stats labels (session_id + tenant_id). Encoder
  metric attribution rests on this.
- **T84** — CI/CD release pipeline that produces the image this
  procedure deploys.
- **T93** — region-aware signaling auth (when it lands, the
  per-region recommended defaults in §3 fold into Helm values
  per region).
- **T97** — encoder factory canonical catalog. Read this if §3's
  table doesn't tell you enough.
- **T101** — Phase 2 unlock plan; the engineering side of this
  ops recipe.
- **T103** — cross-team review with #104 follow-up on T96's ICE
  buffering. Phase 1 demo gating depends on #104; mention here so
  the deploy procedure surfaces the cross-team dependency.

---

**Bottom line:** the encoder-config rollback path is your friend
(faster than image rollback, smaller blast radius, naturally
composes with the runtime probe). Watch the §5 metrics
proactively rather than wait for alerts; the fps/qp/dropped-frames
trio catches almost every encoder-side regression early enough
that the encoder-config rollback is enough.

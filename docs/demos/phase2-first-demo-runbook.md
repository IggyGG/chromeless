# Phase 2 first-demo runbook — encoder + capture acceptance

**Status:** pre-demo runbook for T116. **Distinct from the
actual T116 demo doc** (`docs/demos/phase2-first-demo-2026-04-30.md`,
authored at run-time by qa-tester after the demo executes).
**Audience:** qa-tester driving T116 against the live Triform
cluster; chromium-dev available to answer threshold questions.
**Cross-references:**
T47 (FrameSinkVideoCapturer design),
T55 (capturer skeleton),
T63 (NVENC encoder + Blackwell expectations),
T58 (BweAdapter ±10% target tracking),
T54 + T79 (codec preference),
T78 + T86 (the synthetic-media fallback that this demo *retires*),
T101 §6 (Phase 2 first-demo expectations),
T106 §5 (observability checklist),
T106 §6 (failure modes already catalogued).

The demo's success is end-to-end: a client browser sees real
pixels from a real screen capturer, encoded by a real GPU
encoder. This runbook is the **chromium-dev half** of acceptance:
what to verify on the encoder + capture side, with concrete
probes and threshold gates. qa-tester layers the run procedure
on top.

## 1. Pre-flight checks

Before clicking Connect:

| Check                                      | How                                                                                                                  | Pass criterion                                                                  |
|--------------------------------------------|----------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------|
| Pod image matches T112's build             | `kubectl get pod -l app=chromeless -o jsonpath='{.items[0].spec.containers[0].image}'`                              | Tag = `chromeless:cr7727-<sha>` from the T113 build's `IMAGE_TAG` file.         |
| Pod Ready + healthy                        | `kubectl get pod -l app=chromeless`                                                                                  | `STATUS=Running`, `READY=N/N`, no recent restarts.                                |
| Image is the Phase 2 binary, not Phase 1   | `kubectl exec <pod> -- /usr/local/bin/chromeless --version`                                                 | Prints version + `CHROMELESS_IMAGE_TAG` = current tag. (If `chromium --version` is what runs, the deploy is still on the v1 image — abort.) |
| GPU visible to the Pod                     | `kubectl exec <pod> -- nvidia-smi --query-gpu=name,driver_version --format=csv`                                      | Returns "NVIDIA RTX PRO 6000 Blackwell, 590.48.01" (per the team-lead's cluster note).   |
| NVENC probe passes for H.264 + AV1         | CDP probe (§5.A) running `NvencEncoder::ProbeAvailable("H264")` + `("AV1")` via the worker's `--probe-encoders` flag, OR check the chromium.log for the factory's startup probe-cache populate line. | Both return `true`. (Blackwell supports both per T43.)            |
| Xvfb screen is renderable                  | `kubectl exec <pod> -- env DISPLAY=:99 xrandr 2>&1`                                                                  | Reports `Screen 0: ... 1920 x 1080`. (Regression guard — T78-followup ruled this out as the original NotReadableError cause.) |
| `window.pc` exposed by streamer            | CDP `Runtime.evaluate` (§5.B): `typeof window.pc`                                                                     | `"object"`. (T69 regression guard.)                                              |
| Streamer joined signaling                  | `kubectl logs -l app=chromeless-signaling --tail=50 \| grep "peer joined role=browser"`                                       | At least one match, dated post-Pod-start.                                        |

If any pre-flight check fails, **abort the demo**, file the
specific gap as a follow-up, and reset. Don't paper over a
pre-flight failure — it'll make every downstream measurement
meaningless.

## 2. Capture-side acceptance (T47 + T55)

| Acceptance gate                      | Source                                    | Threshold                                                  |
|--------------------------------------|-------------------------------------------|-------------------------------------------------------------|
| Frame pacing                         | `cb_webrtc_outbound_fps_stdev_ms` (sidecar derived) over 100 consecutive frames | **stdev < 3 ms** at 30 fps target. Per T47 §4 cutover gate. |
| Frame production rate                | `cb_webrtc_outbound_fps`                  | Within ±2 fps of configured target (default 30).            |
| Source resolution honored            | First `OnEncodedImage`'s `_encodedWidth × _encodedHeight` | Matches configured ladder. v1 default = 1920×1080 single-layer. Simulcast (T83) = 1920×1080 + 960×540 + 480×270 across rids. |
| Damage-rect metadata propagated      | `cb_capture_damage_rect_pixels_per_frame` if T85's instrumentation has been wired (Phase 2.5 follow-up — likely **not** yet) | Metric exists. If missing: T85 is design-only as of now; not a demo blocker. |
| `Done()` ack rate matches frame rate | `cb_capture_buffers_done_total` rate ≈ `cb_capture_frames_received_total` rate | Divergence > 5/s sustained = T55 RAII guard regression — abort. |

The T47 §4 ≥30 ms p95 reduction-vs-`getDisplayMedia` claim is
**not** what we measure here. That measurement requires the
synthetic-media path running side-by-side, which T116 doesn't
exercise. T117's harness work produces the comparison number
later.

## 3. Encoder-side acceptance (T63 + T101 §6)

| Acceptance gate                         | Source                                           | Threshold                                                                              |
|-----------------------------------------|--------------------------------------------------|-----------------------------------------------------------------------------------------|
| NVENC AV1 encode latency p95            | `cb_webrtc_outbound_encode_time_ms{quantile="0.95"}` filtered to `codec="AV1"` + `impl=~"nvenc.*"` | **≤ 8 ms** at 1080p30 on Blackwell (per T101 §6 expected number; T63 §performance section). |
| NVENC H.264 encode latency p95          | same metric, `codec="H264"`                     | ≤ 5 ms at 1080p30. (NVENC H.264 has been Turing+ stable for years; bar is tighter than AV1.) |
| QP p95                                  | `cb_webrtc_outbound_qp{quantile="0.95"}`        | **< 38** (any sustained ≥ 38 means encoder saturation; check bitrate ceiling). Per T106 §3 Stage-C signal. |
| BWE target tracking                     | `(cb_webrtc_outbound_bytes_per_second * 8) / cb_bwe_target_bitrate_bps` | **0.9 ≤ ratio ≤ 1.1** sustained over 30 s. Per T58 contract.                            |
| VP9 SW fallback path works              | If AV1 **not** negotiated (e.g., Safari client), confirm VP9 SW (T35) lit up via `cb_webrtc_outbound_implementation == "cloud-browser-vp9-libvpx-lowlatency"` | Implementation string from T35's `GetEncoderInfo`. SW fallback is the unconditional Phase 1 floor. |
| No mid-session encoder crashes          | `kubectl logs -l app=chromeless --since=10m \| grep -E "encoder.+(crash|FATAL|ASAN|signal 11)"` | Empty.                                                                                  |

The 8 ms p95 NVENC AV1 number is the load-bearing first-Phase-2
result. Blackwell is the most-capable cell of T43's matrix; if
we don't hit it on Blackwell we don't hit it anywhere.

## 4. Negotiation acceptance (T30 + T54 + T79)

| Acceptance gate                        | Source                                                                              | Threshold                                                                |
|----------------------------------------|--------------------------------------------------------------------------------------|---------------------------------------------------------------------------|
| Negotiated codec is the expected default | `RTCRtpSender.getStats()` `codec.mimeType` on either peer                          | Default `VIDEO_CODEC_PREFERENCE = ["VP9", "AV1", "H264", "VP8"]` from `client/main.ts`. Demo client = Chrome desktop → expect VP9 (per T79 §3 per-UA preference table). |
| SDP fmtp lines for H.264 (if negotiated) | `pc.localDescription.sdp` and `pc.remoteDescription.sdp`                            | Both carry `profile-level-id=42e01f` (Constrained Baseline 3.1 — T36 default).    |
| SDP fmtp for AV1 (if negotiated)       | same                                                                                 | `profile=0` (Main, 8-bit 4:2:0 — T75 default; HDR is T98 deferred).               |
| No unexpected codec_fallback events    | `cb_webrtc_codec_fallback_total{from,to}` counter                                   | 0 increments during the demo. (Any increment = the runtime probe flipped mid-session, which means HW is unhealthy — see §6.) |
| `window.__cbwrtc_pc.getReceivers()` shape | CDP probe (§5.D) on the **client** browser if `?e2e=1` is set                      | Returns ≥1 video receiver with non-null `track`. Required for E2E spec 03 (post-T34). |

## 5. Concrete CDP probes (qa-tester copy-paste targets)

All probes assume the Pod's DevTools is reachable at
`http://<pod-ip>:9222` per T9-followup. Use the kubectl-exec
form so probe runs against the Pod's loopback regardless of how
the cluster networking is set up.

### A. NVENC probe results

```bash
POD=$(kubectl get pod -l app=chromeless -o name | head -1)
WS=$(kubectl exec "$POD" -- curl -s http://127.0.0.1:9222/json/list \
     | jq -r '.[] | select(.type=="page") | .webSocketDebuggerUrl' | head -1)
# Then in any Runtime.evaluate-capable client (websocat, the same
# Python pattern infra/lifecycle/idle-watchdog.sh uses, etc.):
{"id":1,"method":"Runtime.evaluate",
 "params":{"expression":"window.cbProbe && window.cbProbe.encoders","returnByValue":true}}
```

`window.cbProbe.encoders` is populated by the worker at startup
when `--enable-features=CloudBrowserStartupProbe`. If the field
is undefined, fall back to inspecting startup-log:

```bash
kubectl logs "$POD" | grep -E "ProbeAvailable\(.+\) = (true|false)"
```

### B. window.pc shape

```js
{type:"Runtime.evaluate", expression:`({
  pc_present: typeof window.pc === "object",
  signaling: window.pc?.signalingState,
  ice: window.pc?.iceConnectionState,
  conn: window.pc?.connectionState,
  localSdpBytes: window.pc?.localDescription?.sdp?.length || 0,
  remoteSdpBytes: window.pc?.remoteDescription?.sdp?.length || 0,
})`, returnByValue: true}
```

Run pre-Connect (expect `signaling=stable`, all the rest empty)
AND post-Connect (expect `signaling=stable`, `conn=connected`,
both SDP bytes > 0).

### C. Live encoder stats from getStats()

```js
{type:"Runtime.evaluate", expression:`(async () => {
  const stats = await window.pc.getStats();
  const out = [];
  stats.forEach(s => {
    if (s.type === "outbound-rtp" && s.kind === "video") {
      out.push({
        codec: s.codecId,
        framesEncoded: s.framesEncoded,
        bytesSent: s.bytesSent,
        qpSum: s.qpSum,
        encoderImplementation: s.encoderImplementation,
        totalEncodeTime: s.totalEncodeTime,
      });
    }
  });
  return out;
})()`, returnByValue: true, awaitPromise: true}
```

`encoderImplementation` is the smoking gun for "is the right
encoder in play." Expect `"cloud-browser-nvenc-AV1-lowlatency"`
(T63) or `"cloud-browser-vp9-libvpx-lowlatency"` (T35 fallback).

### D. Client-side receiver shape (E2E hook)

Run on the **client** browser (not the Pod) at
`http://<client-host>:3000/?e2e=1`:

```js
{type:"Runtime.evaluate", expression:`(() => {
  const pc = window.__cbwrtc_pc;
  if (!pc) return null;
  return pc.getReceivers().map(r => ({
    kind: r.track?.kind,
    enabled: r.track?.enabled,
    muted: r.track?.muted,
    decoderImpl: r.transport?.iceTransport?.gatheringState,
  }));
})()`, returnByValue: true}
```

Per T69's hook (the `?e2e=1` gating), `window.__cbwrtc_pc` is
populated client-side. Expect at least one `{kind:"video"}`
receiver with `enabled:true, muted:false`.

## 6. Failure-mode triage

If any §1–4 gate fails, before re-running:

| Symptom                                               | First place to look                                                       | Likely cause                                                                                                  |
|-------------------------------------------------------|---------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------|
| NVENC AV1 latency p95 > 8 ms                          | `cb_webrtc_outbound_qp` (saturation) + GPU utilization via `nvidia-smi`  | Encoder saturation OR Pod scheduled to a non-Blackwell node. Check NodeSelector on the Pod (T95 §4). Encoder-config rollback per T106 §4 to `prefer_nvenc_av1=false` if isolated to AV1. |
| `encoderImplementation` shows SW path                 | startup-log NVENC probe lines                                              | NVENC SDK ABI mismatch (T106 §6 row 2). Image built against SDK X, host driver is Y. Image-level rollback per T106 §4. |
| QP p95 ≥ 38 sustained                                 | `cb_bwe_target_bitrate_bps` vs `cb_webrtc_outbound_bytes_per_second`     | Bitrate ceiling too low for the content. Confirm Config target_bitrate_bps; if BWE is saturated, real bandwidth is the constraint. |
| `iceConnectionState` stuck at `checking`              | Signaling logs: was a `peer joined role=client` line emitted?            | T96/T104's late-client-join path — check that #104 ICE-buffer fix landed in the deployed signaling image. Otherwise the streamer's candidates were dropped. |
| Encoder crashes mid-session (signal 11 / FATAL)       | `kubectl logs --previous`                                                 | Driver / encoder bug. Capture the full backtrace, encoder-config rollback the affected codec, file a follow-up against T63 / T70 / T75 as appropriate. |
| Streamer page shows `NotReadableError`                | `chromium.err.log` for `vkCreateInstance` lines                           | T78-followup regression — Vulkan re-enabled. Confirm launch-chromeless.sh's `--disable-features=Vulkan` flag is in the production launch command (T78). |

T101 §3 has the deeper triage tree for build-time failures
(unit-test fail, link OOM, patch context drift); this table
focuses on runtime symptoms specifically.

## 7. Hand-off to qa-tester for T117

Once §1–4 gates all pass:

1. **Record the run** in `docs/demos/phase2-first-demo-2026-04-30.md`
   per T116's spec (image SHA, network conditions, first-frame
   time, negotiated codec, dashboard screenshot, subjective
   quality, video/audio recording).
2. **Hand off to T117's harness sequence:** the same Pod stays
   running; qa-tester drives the latency-harness flashing-color-
   block page (T10/T11) against the cluster from a real client
   browser, captures glass-to-glass numbers per the T65
   framework, and posts those numbers as the v1-defining
   measurement.
3. **Don't tear down the Pod** between T116 and T117. Recreating
   it loses warmth on the encoder factory's runtime probe cache
   + the Pod's accumulated connection-quality history; the
   T117 numbers should be against the Pod that just passed T116.

## Cross-references

- **T47 §4** — FrameSinkVideoCapturer cutover criteria; the stdev
  < 3 ms threshold above comes from there.
- **T55** — capturer's `Done()` lifecycle; §2's
  `buffers_done_total` divergence guard.
- **T63 §"Observability / future work"** + **T101 §6** — the
  ≤ 8 ms p95 NVENC AV1 latency expectation.
- **T58 §contract** — the BWE ±10% bitrate-tracking gate.
- **T54** + **T79 §3** — codec preference list + per-UA-class
  table the demo's negotiation honors.
- **T78** + **T86** — the synthetic-media stop-gap this demo
  retires. Once T116 + T117 produce real numbers, the README
  "Known limitations" entry about CHROMELESS_USE_FAKE_MEDIA flips.
- **T96** + **T104** — signaling buffer + ICE replay; without
  #104 deployed, the late-client-join path fails connection
  before any encoder gate is exercised.
- **T101 §3** — build-time failure triage tree.
- **T106 §4 + §6** — production rollback procedure + failure
  modes catalog.

---

**Bottom line:** §1 pre-flight is the gate that prevents
wasting a demo run; §3's NVENC AV1 ≤ 8 ms p95 on Blackwell is
the load-bearing measurement the team has been building toward
since T17 was a research doc. §6's triage table tells qa-tester
what to do if any of it doesn't go green, without paging
chromium-dev for every miss.

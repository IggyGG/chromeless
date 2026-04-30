# Harness validation methodology (T12)

> **Scope:** Independent QA review of [T10](../../harness/latency/) (harness
> page) and [T11](../../harness/latency/reconcile.py) (`reconcile.py`).
> **Audience:** every tester running the latency harness, every reviewer
> gatekeeping numbers against [`docs/v1-success-criteria.md`](../../docs/v1-success-criteria.md),
> and the future operator reproducing a regression.
> **Companion artifact:** [`loopback-baseline.sh`](./loopback-baseline.sh)
> — a hermetic CI-friendly check that the reconciler produces sane
> numbers against the bundled deterministic fixtures.

The harness measures glass-to-glass video latency. Both halves can lie
in subtle ways. This document enumerates the lies, says how big they
are, and gives the operator a fixed procedure for distinguishing
real-pipeline latency from measurement noise.

---

## 1. Threats to validity

Five named threats. The first four are the project brief's
non-negotiables; the fifth (refresh-sync) is a real concern I found
during this review and is now first-class.

### 1.1 Frame-rate aliasing

**Mechanism.** The harness flips the block at a configurable period
(default 1000 ms = 1 Hz). The webcam captures at a fixed framerate
(typical 30 or 60 fps). The earliest cam frame to show the new color
defines the cam-side timestamp. Granularity is therefore bounded by
1 / cam_fps:

| cam_fps | 1-frame jitter | implication |
|---------|----------------|-------------|
| 30 fps  | ±33 ms         | unusable for the 5 ms LAN-network sub-budget |
| 60 fps  | ±17 ms         | borderline for LAN P50 < 100 ms |
| 120 fps | ±8 ms          | preferred when available |

**Aliasing case.** If `flash_period < 2 / cam_fps`, multiple state
transitions can fall inside a single cam frame and the reconciler
sees only the last one. With the default 1 Hz flash and a 30 fps cam
this is impossible (1000 ms ≫ 33 ms), but a tester who tightens the
flash period for higher throughput must keep the **2× rule**:
`cam_fps >= 2 * flash_freq`.

**Mitigation.** The harness's `?period=` URL parameter and the
operator's recorder framerate are both adjustable. The operator
checklist (§4) requires the tester to write down both numbers in run
notes; CI will eventually assert them.

**Probe.** Detune the flash period across {500, 1000, 2000} ms and
confirm the recovered p50 is invariant. If it isn't, you've crossed
the 2× rule.

### 1.2 Clock skew

**Mechanism.** Cam-side `cam_epoch_ms` (from the recording host) and
source-side `epochMs` (the harness page's `Date.now()`) live in
two clock domains. The latency the reconciler reports is

```
latency = cam_epoch_ms - emit_epoch_ms - clock_offset_ms
```

Any skew between the two host clocks lands directly in `latency`.

**Magnitude.** Public NTP gets you ≤ 10 ms skew on a clean host;
LAN NTP gets you ≤ 1 ms. A host whose clock has drifted (laptop
asleep for hours, no NTP) can be tens of seconds off — the
reconciler emits massive negative latencies in that case.

**Mitigation.** Three layers:
1. NTP-sync both hosts before each run. [`calibrate.md`](../../harness/latency/calibrate.md)
   Procedure 1 has the commands.
2. Same-host runs (Procedure 2) sidestep skew entirely.
3. Loopback offset measurement (Procedure 3) when NTP isn't
   available; subtract the residual via `--clock-offset-ms`.

**Probe.** `reconcile.py` reports `negative_count`. **Any
`negative_count > 0` is a calibration failure** — fix the clocks
before trusting the run. The bundled `loopback-baseline.sh` asserts
`negative_count == 0` on the deterministic sample.

### 1.3 Rolling shutter

**Mechanism.** Most consumer USB webcams use rolling shutter: each
scanline is exposed sequentially top-to-bottom over ~1 / cam_fps. A
state transition that occurs mid-exposure produces a frame whose top
shows the new color and whose bottom shows the old (or vice versa).

**Magnitude.** At 60 fps cam, the readout sweep is ≈ 17 ms. The
QR is anchored at the **top-left** of the harness page (16 px / 16
px from the corners), so it's exposed in the **first ~0.3 ms** of
the sweep — the freshest portion. This is favourable: by the time
the rest of the frame is read out, the QR (which encodes the
authoritative timestamp) is stable.

The block colour itself fills the viewport, so part of any
mid-transition frame shows ON and part shows OFF. The reconciler
does **not** look at the block colour anywhere — it only decodes
the QR — so this asymmetry doesn't bias the result; it only adds
±0.5 frame of jitter when the QR area happens to span the
transition.

**Mitigation.** Anchor the QR at the top of the page (already done
by T10's CSS). Prefer cameras with shorter readout (higher fps).
Document the cam model + rolling-shutter readout in run notes.

**Probe.** Compare a global-shutter reference cam (e.g. an
industrial Basler) against a rolling-shutter consumer cam in the
same run. The recovered p50 should differ by no more than 1 cam
frame.

### 1.4 Ambient light and exposure

**Mechanism.** A black ↔ red flash drives a large chunk of the cam
sensor through a wide luminance swing. With auto-exposure (AE) on,
the cam adapts after every transition — bumping ISO, lengthening
exposure, sometimes adjusting white balance. AE adaptation:

- delays for ~ 100–500 ms at the start of each new state, biasing
  the **first** cam frame's measurement (overexposed during ON's
  early frames, underexposed during OFF's),
- can trigger frame drops on cheaper cams as the ISP recomputes,
- changes white balance on the QR's white background, which can
  trip pyzbar's threshold and lower the QR decode rate.

**Magnitude.** AE-induced bias is up to ±20 ms on a typical
720p / 30 fps consumer cam. AE-induced drops can spike `min_ms`
and skew p50.

**Mitigation.** **Lock auto-exposure and auto-focus before each
recording** (see `calibrate.md`'s physical-setup checklist).
Exposure should be set high enough that the OFF state isn't
underexposed (block visibly black, not noisy grey). Bright but
indirect ambient light reduces the dynamic range the cam needs.

**Probe.** `reconcile.py` reports `qr_decode_rate`. **Anything
< 0.95 indicates a cam-setup problem** — relight, refocus, or move
the cam closer. A run with < 0.95 decode rate is unfit for the v1
budget; only relative comparisons are valid.

### 1.5 Monitor refresh sync (rAF vs paint)

**Mechanism.** The harness records `epochMs = Date.now()`
synchronously inside a `requestAnimationFrame` callback, *before*
the new pixels are committed to the display. The actual pixel
emission happens at the next compositor commit + vsync, typically
4–16 ms later on a 60 Hz panel.

Result: the embedded QR timestamp is **earlier** than the moment
the bright-red pixel actually showed up on screen. Every measured
latency is biased high by ~1 vsync.

**Magnitude.** Roughly 8 ms median, with bimodal jitter at 0 and
~16 ms depending on whether the swap landed on this vsync or the
next. Higher-refresh panels (120/240 Hz) have proportionally less
bias. The bias is **systematic**, not random.

**Mitigation.** The loopback offset measurement
([`calibrate.md`](../../harness/latency/calibrate.md) Procedure 3)
captures both this rAF/paint bias **and** the cam's own capture
latency in a single number, which is then subtracted via
`--clock-offset-ms`. As long as the same cam, panel, and source
host are used on either side of a measurement, the bias cancels.

**Probe.** Run the loopback procedure twice on the same machine an
hour apart; the two `min_ms` values should agree to within 2 ms.
If they don't, something else is changing (panel-refresh ramp,
power-management throttling on the cam).

---

## 2. Calibration procedure

The harness has a working calibration procedure already documented in
[`harness/latency/calibrate.md`](../../harness/latency/calibrate.md);
this section is the **operator-facing summary** that fits on a single
page.

Before a session that produces numbers we report against the v1
budget:

1. **NTP-sync both hosts** (or use a single host).
   - `timedatectl status` should report "synchronized: yes" on Linux.
   - `sntp -sS time.apple.com` is fine on macOS.
2. **Lock the webcam** in manual mode: AE off, AF off, fixed exposure.
   - On Linux v4l2: `v4l2-ctl --device /dev/video0 --set-ctrl=exposure_auto=1,exposure_absolute=200`
   - Verify in a still preview that the OFF state isn't crushed black
     and the ON state isn't blown red.
3. **Light the room** with bright but indirect light. Avoid windows
   in the cam's field of view.
4. **Frame the cam** so QR + flashing block both fit, with the cam
   slightly off-axis to avoid display reflection.
5. **Run a 60 s loopback** (cam pointed at *source* display, no
   streaming pipeline in between). Run `reconcile.py` against the
   recording. Record the median latency `L_loop`.
6. **Set the offset.** Subtract `L_loop` from real-pipeline runs via
   `--clock-offset-ms L_loop`. Document `L_loop` in the run notes
   alongside cam model, fps, and exposure setting.
7. **Re-run the loopback every hour or after any environmental
   change** (room temperature, cam re-mount, panel-refresh override).

If you skip any of steps 1–6 and report a number against the v1
budget, that number is not trustworthy. Steps 1–6 are not optional.

---

## 3. Recommended ranges

What numbers should we trust, and where does measurement noise
dominate?

### 3.1 Reconciler-level sanity (every run)

Hard pass/fail on:

| Field             | Pass condition                  | If failed: |
|-------------------|--------------------------------|------------|
| `qr_decode_rate`  | ≥ 0.95                         | Cam setup is broken — focus, exposure, framing. Don't trust the run. |
| `negative_count`  | == 0                           | Clocks aren't aligned. Re-NTP and re-run. |
| `frames_seen`     | ≥ 60 × cam_fps × duration_s × 0.95 | Frame drops. Could be USB transfer, AE chase, or recorder backpressure. |
| `min_ms`          | strictly positive               | Negative `min` overrides every other check. Skew. |
| `aliased_warning` | == False                       | cam_fps < 2 × flash_freq — the run is aliased. Re-shoot at higher cam fps or lower flash freq. (T60) |
| `sink_lag_p95_ms` | < 20 ms (LAN), < 50 ms (regional) | The page→sink WebSocket is backed up. Cross-check by comparing JSONL `epochMs` against QR-encoded `epochMs` for the same flash; prefer the QR if they diverge. (T61) |

### 3.2 Per-target ranges

For glass-to-glass latency on the v1 budget (LAN < 100 ms / regional
< 200 ms):

| Run shape                                    | Expected p50      | Expected p95      | Where it's noise-dominated |
|----------------------------------------------|-------------------|-------------------|----------------------------|
| Loopback (cam → source display, no pipeline) | 30–50 ms          | ≤ 70 ms           | n/a — this is the noise floor |
| LAN, software encode, default settings       | 60–95 ms          | ≤ 130 ms          | < 5 ms differences are noise |
| Regional (≤ 30 ms RTT to server)             | 100–180 ms        | ≤ 240 ms          | < 10 ms differences are noise |
| Anything above 250 ms p50                    | broken            | broken            | something is queueing — don't tune until you find it |

### 3.3 Where noise dominates

Below roughly **5 ms** the harness cannot distinguish real
improvements from measurement noise. Drivers of the floor:

- 1 / cam_fps (16 ms at 60, 8 ms at 120),
- rAF-vs-paint systematic bias (~8 ms at 60 Hz, baked into the
  loopback offset),
- rolling-shutter intra-frame jitter (≤ 0.5 cam frame),
- AE / WB micro-adjustments around state transitions (~2 ms).

So a change that moves p50 by < 5 ms on a single 60 fps run is
**not statistically significant** under this harness. To resolve
sub-5-ms differences, either:

1. Run for longer (≥ 5 minutes ≈ 300 transitions) and compare
   distributions, not point estimates;
2. Switch to a 120 / 240 fps cam to reduce the framerate floor;
3. Move to Procedure 4 (external strobe) for sub-millisecond ground
   truth — out of v1 scope.

---

## 4. Run procedure (operator checklist)

Concrete, reproducible. Anyone on the team can drive a run by
ticking through this without re-reading the design doc.

```
[ ] 1. NTP-sync source host  (timedatectl status / sntp)
[ ] 2. NTP-sync cam host     (same)
[ ] 3. Lock cam AE + AF      (v4l2-ctl / cam app)
[ ] 4. Indirect ambient light, no glare on display
[ ] 5. Frame cam, QR + block both visible, slightly off-axis
[ ] 6. python3 server-sink.py --out-dir harness/captures
[ ] 7. (cd harness/latency && python3 -m http.server 8000)
[ ] 8. Open client display: http://<source>:8000/?period=1000&run=<id>
[ ] 9. Press 'f' on source: fullscreen
[ ]10. Start cam recording (note START_MS = $(date +%s%3N))
[ ]11. Record for ≥ 60 s (≥ 300 transitions at 1 Hz)
[ ]12. Stop recording. echo "$START_MS" > cam.start.txt
[ ]13. python3 harness/latency/reconcile.py cam.mp4 \
         --recording-start-epoch-ms $(cat cam.start.txt) \
         --jsonl harness/captures/<id>.jsonl \
         --clock-offset-ms <L_loop>  # from your last loopback
         --out reports/<id>
[ ]14. Eyeball <id>-summary.txt:
       qr_decode_rate ≥ 0.95?  negative_count == 0?  min_ms > 0?
[ ]15. If any sanity check fails, do NOT report numbers. Fix cause
       per §1, re-run.
```

For relative comparisons (encoder A vs encoder B on the same hosts
in the same hour), step 13's `--clock-offset-ms` can be 0 — the
delta is what matters, the absolute offset cancels.

---

## 5. Hermetic regression check

[`tests/harness/loopback-baseline.sh`](./loopback-baseline.sh) runs
the reconciler against the bundled deterministic
[`sample-input/`](../../harness/latency/sample-input/) fixture and
asserts:

- `frames_seen == 4`
- `qr_decode_rate == 1.0`
- `negative_count == 0`
- `30 ≤ min_ms ≤ 60`
- `35 ≤ p50_ms ≤ 65`
- `p95_ms ≤ 100` and `max_ms ≤ 100`

These bounds are tight because the sample is hand-crafted to a known
manifest. Any drift is a **regression in `reconcile.py` itself**
(payload parsing, percentile math, time arithmetic, manifest
handling) — the script is the canonical pre-merge gate for any PR
that touches `harness/`. It does **not** validate physical setup —
that's the operator checklist in §4.

The script exits non-zero with a precise message naming the failing
metric, and dumps the full reconciler summary on failure for triage.

---

## 6. Review notes — T10 (`harness/latency/`) + T11 (`reconcile.py`)

Independent review against the brief. Findings:

### 6.1 What works

- **Three-redundant-encoding design.** QR + binary bars + human-
  readable text. Belt-and-braces against partial cam failure modes.
  Implementation in `index.html` is clean and CSS-anchored —
  resizable display, fixed pixel offsets.
- **Triple-clock recording.** `perfMs` (rAF monotonic), `epochMs`
  (page wall clock), `sinkRecvEpochMs` (sink wall clock). Lets a
  future analyst diagnose whether skew is page→sink or page→cam.
- **Reconciler authoritative-emit precedence.** When a JSONL is
  passed, `reconcile.py` prefers JSONL `epochMs` over QR-payload
  emit time. The QR-only path is the fallback. Right design.
- **Lazy heavy imports.** `reconcile.py --help` works without
  pyzbar/cv2/matplotlib installed. Good first-run UX.
- **Manifest-driven sample.** `sample-input/manifest.jsonl` makes
  the in-tree self-test deterministic, independent of file mtimes.
  Lets CI assert specific numbers.
- **Histogram on output.** `Agg` matplotlib backend, headless-safe,
  artifact-friendly.

### 6.2 Gaps and follow-ups

Three real gaps. Not blocking the harness for v1 use, but worth a
ticket each.

- **G1. Binary-bar fallback is rendered but never read.** The
  page emits 16 binary bars per frame "as a fallback when QR
  detection misses a frame" (per `index.html` comment), but
  `reconcile.py` only does QR detection. The bars are dead code on
  the read path. Either implement bar decoding or demote the bars
  to "diagnostic only" in the docs. Filed as **T59**.
- **G2. No assertion of `cam_fps >= 2 × flash_freq`.** A tester
  can drive the page at 2 Hz against a 1 fps cam and get a totally
  meaningless run — the reconciler will happily emit p50 numbers
  with no warning. `reconcile.py` should compute the cam fps from
  the video file (it has it: `cv2.CAP_PROP_FPS`) and warn (or
  refuse) when the rule is violated. Filed as **T60**.
- **G3. `sinkRecvEpochMs` is recorded but not surfaced in the
  reconciler output.** The summary doesn't include any sink-vs-emit
  delta, so a tester can't see whether their page→sink websocket is
  backed up. Add `sink_lag_p50/p95_ms` to the summary. Filed as
  **T61**.

### 6.3 Non-issues called out so they don't get re-litigated

- **rAF-vs-paint bias is systematic, not random.** Cancelled by
  Procedure 3's offset. Documented above; not a bug.
- **Reconciler keeps negative latencies and counts them.** Right
  call — visibility into clock-skew failures, validated by the
  bundled sample's `negative_count == 0` assertion.
- **Reconciler tolerates dupes when the sink restarts mid-run.**
  `server-sink.py` opens its JSONL in append mode; the reconciler's
  `(runId, frameId)` index naturally dedupes. No action needed.

---

**Bottom line:** The harness methodology is sound for the v1 budget,
**provided** the operator follows the §4 checklist and respects the
§3 sanity bounds. Three minor follow-ups are filed. The reconciler is
the gate for any harness change; `loopback-baseline.sh` enforces the
gate in CI.

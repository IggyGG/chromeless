# Phase 1 latency measurement — TEMPLATE

> Operator template for the **first real glass-to-glass measurement run
> on the assembled Phase 1 stack** (T65). Copy this file to
> `phase1-loopback-YYYY-MM-DD.md` (or `phase1-<network>-YYYY-MM-DD.md`
> for non-loopback runs), fill in every section, then commit alongside
> the recorded JSONL under `harness/captures/` and the baseline JSON
> under `tests/harness/baselines/phase1-*.json`.

---

## 1. Summary

| | |
|-|-|
| **Run ID**         | _e.g. `phase1-loopback-2026-05-02-iggy-mbp`_ |
| **Date / time**    | _ISO 8601, including timezone_ |
| **Target bucket**  | _one of: `loopback`, `lan`, `regional`_ |
| **Operator**       | _name + handle_ |
| **Stack revision** | _git rev-parse HEAD_ |
| **Pass / fail vs budget** | _PASS / FAIL — fill from `phase1-baseline.sh` exit code_ |
| **Recommendation** | _proceed to <next phase> / fix-and-retry / rerun with X_ |

---

## 2. Setup

### 2.1 Hosts

| | source host | cam host |
|-|-|-|
| Machine        |  |  |
| OS / kernel    |  |  |
| CPU / RAM      |  |  |
| Network        | _wired/wifi, link speed_ | _wired/wifi, link speed_ |
| NTP source     | _e.g. `time.cloudflare.com` (LAN) or pool.ntp.org (public)_ | _same?_ |
| Skew at run    | _result of `chronyc tracking` or equivalent_ | _same_ |

(For loopback runs the same host plays both roles; fill one column,
collapse the other.)

### 2.2 Cam

| | |
|-|-|
| Model                 |  |
| Mount type            | _USB-C, USB-A, built-in_ |
| Captured framerate    | _fps_ |
| Captured resolution   | _e.g. 1280×720_ |
| Sensor type           | _rolling shutter / global shutter_ |
| Auto-exposure         | _LOCKED — settings:_ |
| Auto-focus            | _LOCKED — settings:_ |
| White balance         | _LOCKED — settings:_ |
| Recording tool        | _ffmpeg flags / OBS preset / native_ |
| Recording duration    | _seconds_ |

### 2.3 Display + lighting

| | |
|-|-|
| Display panel         | _model, native refresh_ |
| Display refresh used  | _e.g. 60 Hz_ |
| Display resolution    |  |
| Ambient light         | _bright/dim/dark; direct/indirect_ |
| Glare on display      | _yes/no — describe mitigation_ |
| Cam-to-display angle  | _degrees off-axis_ |
| Cam-to-display distance | _cm_ |

### 2.4 Cloud-browser stack

| | |
|-|-|
| Image tag             | _e.g. `cloud-browser-webrtc:dev` SHA_ |
| `CBWRTC_USE_FAKE_MEDIA` | _0 / 1_ — if 1, this run measures synthetic-source latency, not real screen capture |
| Encoder               | _vp9 / x264 / nvenc / vaapi / av1 — and which task wired it_ |
| Audio                 | _enabled / disabled_ |
| Cursor metadata (T26) | _enabled / disabled_ |
| Auth (T48)            | _enabled / disabled_ |
| TURN (T76 issuer)     | _enabled / disabled_ |

### 2.5 Harness page parameters

| | |
|-|-|
| Flash period          | _ms (URL `?period=`)_ |
| ON / OFF colors       |  |
| Fullscreen            | _yes/no_ |
| Run ID URL param      |  |

### 2.6 Loopback offset measurement (Procedure 3)

If this is a non-loopback run, the operator **must** have done a
preceding loopback run on the same hosts in the same hour. Record
its result here:

| | |
|-|-|
| Loopback run ID       |  |
| Loopback p50          | _ms — passed to `--offset-ms` of phase1-baseline.sh for the cross-host run_ |
| Camera spec sheet capture latency | _ms_ |
| Computed clock offset | _ms (loopback p50 minus camera spec) — applied as `--clock-offset-ms` if non-zero_ |

If this **is** the loopback run, leave this section "N/A".

---

## 3. Numbers

Pulled from `harness/captures/<runId>-summary.txt`. Cross-reference
the baseline JSON at `tests/harness/baselines/phase1-<runId>.json`
which records the same numbers in machine-readable form.

| | value | unit |
|-|------:|-|
| `frames_seen`     |  |  |
| `qr_decoded`      |  |  |
| `qr_decode_rate`  |  |  |
| `negative_count`  |  |  |
| `min_ms`          |  | ms |
| `p50_ms`          |  | ms |
| `p95_ms`          |  | ms |
| `p99_ms`          |  | ms |
| `max_ms`          |  | ms |
| `mean_ms`         |  | ms |
| `stdev_ms`        |  | ms |

Histogram: `harness/captures/<runId>-histogram.png` — embed if useful.

### 3.1 Input latency (T39)

If T39's input-latency harness ran in the same session, record its
numbers here.

| | value | unit |
|-|------:|-|
| `input_p50_ms` |  | ms |
| `input_p95_ms` |  | ms |
| `input_p99_ms` |  | ms |
| `input_max_ms` |  | ms |
| Sample count   |  |  |

---

## 4. Comparison vs budget

### 4.1 v1 success criteria
([`docs/v1-success-criteria.md`](../v1-success-criteria.md))

| Path                   | Target  | Measured p50 | Measured p95 | Pass? |
|------------------------|---------|-------------:|-------------:|:-----:|
| Glass-to-glass, LAN    | < 100 ms |             |             |       |
| Glass-to-glass, regional | < 200 ms |           |             |       |
| Input round-trip       | _v1 doc_ |             |             |       |

### 4.2 Tests/harness/validation.md §3 ranges

Cross-check against the recommended ranges:

| Run shape                 | Expected p50    | Measured p50 | In range? |
|---------------------------|-----------------|-------------:|:---------:|
| Loopback (cam → source)   | 30–50 ms        |             |           |
| LAN, software encode      | 60–95 ms        |             |           |
| Regional (≤ 30 ms RTT)    | 100–180 ms      |             |           |

Sanity column from `validation.md` §3.1:

| Field             | Pass condition                  | Measured | Pass? |
|-------------------|--------------------------------|---------:|:-----:|
| `qr_decode_rate`  | ≥ 0.95                         |          |       |
| `negative_count`  | == 0                           |          |       |
| `min_ms`          | strictly positive               |          |       |
| `frames_seen`     | ≥ 95% of cam_fps × duration   |          |       |

If any sanity row is "no", **the absolute numbers above are not
trustworthy** — only relative comparisons are. Note the cause and
re-run.

---

## 5. What surprised, what didn't

**Expected:**
- _e.g. p95 ≈ 1.3× p50_
- _e.g. ICE-gather complete in < 1 s_
- _..._

**Surprising:**
- _e.g. min_ms unusually high — investigate cam pipeline_
- _e.g. p99 fat tail at codec keyframe boundaries_
- _..._

---

## 6. Bottleneck & next-step recommendation

Fill in based on the numbers. Use this section to drive prioritization
for the next sprint of optimizations.

**Top suspects in measured order (where most of the latency budget
goes):**

1. _e.g. encoder: ~30 ms of pipeline time per stats_
2. _e.g. transport: ~15 ms one-way under loopback (?)_
3. _e.g. browser-side decoder + render: ~12 ms_

**Recommendation: PROCEED / FIX-AND-RETRY**

- _If PROCEED:_ list the next-phase work this measurement unblocks.
- _If FIX-AND-RETRY:_ list the specific changes that need to land
  before re-measuring, in priority order. File follow-up tasks for
  each.

---

## 7. Reproducibility

Anyone re-running this measurement should be able to follow this
section verbatim:

```bash
# 1. Bring up the stack at the same revision.
git checkout <stack revision>
docker compose -f infra/compose.yaml up -d --build
# wait for "peer joined role=browser" in signaling logs

# 2. Operator: NTP-sync hosts, lock cam AE/AF, frame the cam, etc.
#    See tests/harness/validation.md §4 for the full checklist.

# 3. Start the source + sink.
( cd harness/latency && python3 -m http.server 8000 ) &
( cd harness/latency && python3 server-sink.py --out-dir ../captures ) &
# Open http://<source>:8000/?run=<runId>&period=1000 fullscreen.

# 4. Record cam (replace device + duration as needed).
START_MS=$(date +%s%3N)
ffmpeg -f v4l2 -framerate 60 -video_size 1280x720 \
       -i /dev/video0 -c:v libx264 -preset ultrafast -crf 18 \
       -t 60 cam.mp4
echo "$START_MS" > cam.start.txt

# 5. Reconcile + record baseline.
bash tests/harness/phase1-baseline.sh \
    --cam       cam.mp4 \
    --jsonl     harness/captures/<runId>.jsonl \
    --start-ms  "$(cat cam.start.txt)" \
    --offset-ms "<L_loop from preceding loopback run>" \
    --target    "<loopback|lan|regional>" \
    --note      "<freeform>"
```

The script writes:
- `harness/captures/<runId>-summary.txt`
- `harness/captures/<runId>-latencies.csv`
- `harness/captures/<runId>-histogram.png`
- `tests/harness/baselines/phase1-<runId>.json`
- `tests/harness/baselines/phase1-latest.json` → `phase1-<runId>.json`
  (symlink; what future regression checks compare against)

Commit all five plus this filled-in markdown in a single PR.

---

## 8. Sign-off

- **Recommended next action:** _PROCEED to <Phase 2 task> / RETRY after fixing X / ESCALATE_
- **Approved by:** _qa-tester / team-lead_
- **Date:** _ISO 8601_

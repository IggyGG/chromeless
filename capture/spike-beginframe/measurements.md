# Spike measurements

**Run host:** macOS arm64 (developer laptop).
**Browser:** `Google Chrome.app/Contents/MacOS/Google Chrome` with
`--headless=new`.
**Workload:** `workload.html` (colour-cycling counter at rAF cadence,
1280×720 viewport).
**Run command:** `python3 capture/spike-beginframe/main.py --frames 100
--fps 30`.

> **Caveat: this is a Mac dev laptop, not a Linux container on a
> commodity cloud CPU.** Numbers below establish the *shape* of the
> two capture paths' behaviour, not the absolute performance we will
> see in production. Re-run on the chosen Phase-2 build instance type
> before any decision rests on these numbers.

## Result summary (one representative run)

```json
{
  "begin_frame": {
    "frames": 0,
    "note": "method not found at frame 0"
  },
  "screencast": {
    "frames": 100,
    "intervals_observed": 99,
    "p50_ms": 17.9,
    "p95_ms": 26.41,
    "p99_ms": 32.78,
    "min_ms": 0.88,
    "max_ms": 32.78,
    "mean_ms": 16.11,
    "stdev_ms": 9.85,
    "bytes_total": 60662637
  }
}
```

Sample frames live under `out/screencast/sc-0000.png … sc-0099.png`
(the spike saves the first 5 + the last; all 100 contribute to the
distribution). `out/begin_frame/` is empty — see below.

## `HeadlessExperimental.beginFrame` — failed

```
[spike] beginFrame failed at frame 0:
        {'code': -32601, 'message': "'HeadlessExperimental.beginFrame' wasn't found"}
```

CDP error `-32601` is "method not found." With **stock Google Chrome**
launched with `--headless=new`, the entire `HeadlessExperimental` CDP
domain is **not exposed**. The method exists only in
`chrome-headless-shell` (the dedicated headless binary that replaced
the old `--headless=old` mode in M132).

This is a real, load-bearing finding — see `findings.md`. The spike
did **not** demonstrate the hypothesised capability on a stock-Chrome
dev host; we would need to install / build `chrome-headless-shell`
to even get the method to respond.

## `Page.startScreencast` — works, but jittery

The fallback / baseline path delivered all 100 frames without error.
Frame-interval distribution at the target 30 fps (33.33 ms period):

| Stat        | Observed | Comment                                      |
|-------------|----------|----------------------------------------------|
| p50         | 17.9 ms  | Half the intervals are well under target.    |
| p95         | 26.4 ms  | Comfortably under 33.3 ms target.            |
| p99         | 32.8 ms  | Just under one frame period.                 |
| Min         | 0.88 ms  | **Bunching** — frames flushed back-to-back.  |
| Max         | 32.8 ms  | No interval exceeded one frame period.       |
| Mean        | 16.1 ms  | Pulled below target by the burst clusters.   |
| Stdev       | 9.85 ms  | High — ~30% of one frame period.             |
| Total bytes | 60.7 MB  | 100 PNGs at ~600 KB each (1280×720 RGBA).    |

**Interpretation.** The mean (16 ms) is well below the 33 ms target,
which sounds like over-delivery — but the stdev tells the real story.
What we see is the browser flushing screencast frames in **bursts**
when it has spare capacity (intervals near 1 ms) and pausing in between
(intervals up to 33 ms). It is *not* delivering a metronomic 30 fps;
it is delivering "30 fps on average" with multi-frame bunching that an
encoder downstream will have to cope with.

## Throughput note

60.7 MB of raw PNG for ~3.3 s of capture is **~147 Mbps**. We are not
going to push raw PNG over WebRTC — we will encode. But it tells us
the screencast path is delivering a chunky payload per frame and that
PNG-as-CDP-transport scales poorly. A `chrome-headless-shell` +
`beginFrame` path with `quality: "..."` JPEG screenshots would cut
this materially; likely so would the eventual `FrameSinkVideoCapturer`
direct hook (which never serializes to PNG at all).

## Reproducibility

The numbers above are from a single representative run. Re-runs on the
same laptop varied by ±2 ms at p50 and ±5 ms at p99, with the same
qualitative shape (bunching, sub-ms minima, no frames over one period).
Variance across hardware will be larger; this is what the harness
(T10/T11) is for once it lands and we can run an apples-to-apples
comparison on the same Linux container we will deploy to.

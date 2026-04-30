# T28 capture spike: `HeadlessExperimental.beginFrame` + screencast

**Status:** spike (throwaway by design — see PROJECT_BRIEF.md Phase 2).
**Owner:** chromium-dev.
**Cross-references:**
`docs/capture/path-of-least-resistance.md` (T15 — the Phase 1 path we
are testing **against**), `measurements.md`, `findings.md`.

## Hypothesis

We can drive Chromium's compositor on demand from outside the browser
using **`HeadlessExperimental.beginFrame`** over the DevTools Protocol
and read back the rasterized result in the same call. If true, this
gives us:

- **Frame-pacing predictability.** We choose when frames happen, not
  the browser. A target 30 fps becomes a 33.33 ms cadence with
  controlled jitter, not "whatever the compositor felt like doing
  this RAF cycle."
- **Recoverable backpressure.** When the encoder lags, we delay the
  next `beginFrame` instead of dropping in the dark.
- **Synchronous frame pull.** No event-driven push — every frame is
  produced because we asked for it and is delivered as the response
  to that request.

If the hook works, we get a clean alternative to the Phase 1
`getDisplayMedia` path (T15) without yet committing to a full
`FrameSinkVideoCapturer` Chromium patch series (which is the Phase 2
endgame).

## What "success" looks like

In rough order of how strict:

1. **The CDP method is reachable.** `HeadlessExperimental.beginFrame`
   responds without `-32601 method not found`.
2. **It returns a screenshot.** When called with
   `{screenshot: {format: "png"}}` we get a non-empty
   `screenshotData`.
3. **Frame intervals stay close to target.** At 30 fps target
   (33.33 ms period), the **p99 interval is within 5 ms of target**
   (i.e. ≤38 ms) under a non-trivial workload.
4. **Latency is competitive.** End-to-end "frame requested → frame
   bytes received" stays under 16 ms p95 on commodity hardware.

The screencast (`Page.startScreencast`) mode is run alongside as the
**baseline** — what the existing CDP capture path delivers without
any frame-pacing control. We expect it to show worse jitter and to be
the strawman beginFrame replaces.

## What this spike intentionally does *not* do

- **No libwebrtc plumbing.** Captured frames are written to disk as
  PNGs, period. Encoder integration happens in a follow-up only if
  the spike's findings recommend it.
- **No production polish.** Single Python file, hard-coded URL, no
  tests. The point is to learn, not to ship.
- **No comparison with `getDisplayMedia` end-to-end.** That requires
  the latency harness (T10/T11) wired against a running pipeline,
  which is a Phase-0 exit-gate piece. See `findings.md` for the
  cross-reference and the gating question.

## Files

| File              | Purpose                                                       |
|-------------------|---------------------------------------------------------------|
| `main.py`         | The spike. Launches headless Chrome, drives CDP, dumps PNGs.   |
| `workload.html`   | Animated colored counter so the captured frames are non-empty. |
| `measurements.md` | Numbers from a representative run.                             |
| `findings.md`     | Recommendation + next gate.                                    |
| `out/`            | Last run's artifacts (gitignored — see project `.gitignore`).  |

## Running

```
pip install aiohttp websockets
python3 capture/spike-beginframe/main.py --frames 100 --fps 30
```

Override the Chrome binary with `CHROME_BIN=/path/to/chrome` if the
auto-detect fails. On macOS the spike defaults to
`/Applications/Google Chrome.app/Contents/MacOS/Google Chrome`. On a
Phase-2 build host that has `chrome-headless-shell`, point
`CHROME_BIN` at it (see `findings.md` — this matters).

The spike writes 100 frames per mode, but only saves a handful (the
first 5 + the last) as PNG samples to keep the working directory
small. All 100 contribute to the timing distribution.

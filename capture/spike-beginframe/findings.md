# Spike findings & recommendation

## TL;DR

`HeadlessExperimental.beginFrame` is **not available** in stock
Google Chrome with `--headless=new` (CDP error `-32601` —
"method not found"). The domain only exists in
**`chrome-headless-shell`**, the dedicated headless binary that
replaced the old `--headless=old` mode in M132.

`Page.startScreencast` works on stock Chrome but delivers frames with
high bunching (stdev ≈ 10 ms at a 33 ms target, intervals from 0.9 ms
to 32.8 ms, see `measurements.md`) — i.e. the browser, not the spike,
controls cadence.

## What this means for the project

Three concrete consequences:

1. **The Phase-2 "native sidecar with `HeadlessExperimental.beginFrame`"
   path from PROJECT_BRIEF.md is conditional on shipping
   `chrome-headless-shell`, not stock Chrome.** That is not a blocker
   — `chrome-headless-shell` is a maintained binary distributed via
   Chrome for Testing — but it is a different artifact from the
   one our Phase 1 container ships today. Treat it as a separate
   package the spike build depends on.

2. **For Phase 1, the choice is already made for us.** The Phase 1
   path is `getDisplayMedia` inside a regular Chromium (T15).
   `HeadlessExperimental.beginFrame` is *not* a viable Phase-1
   alternative on the binary we are using anyway. PROJECT_BRIEF.md
   listed it as an "spike alternative" — the spike says: not on this
   binary.

3. **For Phase 2 capture, the choice is now between two paths, not
   three.** Originally:
   - (a) keep `getDisplayMedia` (Phase 1 path), or
   - (b) `HeadlessExperimental.beginFrame` sidecar (the spike), or
   - (c) `FrameSinkVideoCapturer` Chromium patch series.

   This spike reduces it to (a) vs (c), with (b) downgraded to "an
   option only if we adopt `chrome-headless-shell` for production"
   — which we may not, given that we want a real browser, not a
   testing-shaped binary.

## Recommendation

**Stay on `getDisplayMedia` for Phase 1 (already the plan, T15).**

For Phase 2, **plan for the `FrameSinkVideoCapturer` path, not the
`beginFrame` sidecar.** Reasons:

- The capture spike showed the cheap-prototype path is not actually
  cheap — it requires swapping the Chromium binary, which is a
  bigger architectural change than it sounds. If we are going to
  pay any Chromium-side cost, pay it for the real Phase-2 hook,
  which gets us per-tab capture, damage rects, and zero-copy
  potential — none of which `beginFrame` would have given us
  anyway.
- The screencast baseline (Page.startScreencast) demonstrates that
  even with a working CDP capture path, frame pacing is the
  browser's call — confirming the more general thesis that any
  CDP-based capture is at the mercy of the compositor's scheduling.
  Direct compositor hooks (`FrameSinkVideoCapturer`) are the only
  way to actually own pacing.

## Next gate

Switch off Phase 1 `getDisplayMedia` only when **both** of:

1. The latency harness (T10/T11) shows `getDisplayMedia` glass-to-
   glass latency exceeds the brief's budgets (LAN <100 ms, regional
   <200 ms) on the production target instance type, AND
2. A `FrameSinkVideoCapturer` prototype on a from-source Chromium
   build (per `docs/build/chromium-from-source.md`, T17) shows ≥30 ms
   p95 reduction on the same workload.

If only (1) clears, the answer is "tune encoder, not capture." If
only (2) clears, we don't need it yet. **Both, or neither.**

Do **not** revisit the `beginFrame` sidecar unless someone produces a
strong reason to ship `chrome-headless-shell` instead of full
Chromium. That decision lives outside the capture path.

## Open follow-ups (not gating Phase 1)

- Try the same spike against a **`chrome-headless-shell`** binary, on a
  Linux container, to confirm `HeadlessExperimental.beginFrame` works
  there and to get a real distribution to compare against. This is a
  ~1-day extension of the existing spike code; file it if it becomes
  relevant.
- Add a `getDisplayMedia` arm to the spike (capture from the
  streamer page in a regular Chrome) once the harness lands, so we
  have apples-to-apples numbers from the same workload.
- Note the throughput observation from `measurements.md`: 60 MB of raw
  PNG per 3.3 s of capture = ~147 Mbps uncompressed. Encoded will be
  far lower, but it underlines that **any** capture path that
  serializes through PNG/CDP is paying for it; direct framebuffer
  paths (Phase 2's `FrameSinkVideoCapturer`) avoid the serialization
  step entirely.

## Verdict

**Spike outcome: clear no-go on the `beginFrame` sidecar with stock
Chrome.** Phase 1 unchanged. Phase 2 commits earlier to
`FrameSinkVideoCapturer` than the brief originally implied.

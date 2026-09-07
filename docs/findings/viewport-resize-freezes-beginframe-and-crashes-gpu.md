# A viewport resize freezes the BeginFrame loop; 15 s later the watchdog crashes the GPU process

**Status:** OPEN. Measured 2026-09-07 on the k8s standalone stack, worker
`cr7727-c5f2eb91c6f0`, three fresh processes, three for three. Introduced as a
*reachable* path by `feat(viewport)` (#96, the same day): before it nothing
called `Cb.setViewport`. Lives in `capture/build-integration/` and needs a
lane build; a client-side mitigation is laptop-verifiable (below).

**Impact for a standalone deployment:** the client now sizes the remote
browser to its window on connect, and no real window's stage is exactly
1280×720, so every connect sends a resize. On a **freshly started worker** —
the state every deployment boots into, and the state supervisord returns it
to after each crash — the first viewer's resize freezes the picture within a
second and about 50 seconds later the browser process exits and restarts:
tabs, history and logins gone. Measured three for three on fresh processes.
One long-lived process (pid 120, ~3.5 h and ~15 sessions old) took 14 resizes
in ten minutes and kept producing frames throughout (`tests/interactive`
65/65 during them, video and stats suites included), so the trigger has a
precondition tied to process age or session count that is **not yet
identified**; do not read the fresh-process result as "always", and do not
read the old-process result as "intermittent".

## What happens

One connect, one process, nothing else running. Times are the worker's log:

```
20:07:34.802  CV2-VIEWPORT: Cb.setViewport requested 854x590 -> applied 854x590
              (CbHeadlessScreen primary display 1280x720 -> 854x590,
               capturer resolution 1280x720 -> 854x590, keyframe asked)
20:07:35.822  CbBeginFrameDriver: stall watchdog fired (no BeginFrame ack in 1000ms;
              consecutive-fires-without-ack=1, first_ack_received=1) — NOT re-issuing
   ...one per second, 2 through 14...
20:07:41      [diag] issued=0 (0 fps) acked=0 | frames_received +0 (0 fps captured)
20:07:49.831  stall watchdog fired (... 15000ms across 15 consecutive fires) —
              re-issuing to restart the ack-chain
20:07:49.859  FATAL:components/viz/service/main/viz_main_impl.cc:342]
              Check failed: !has_created_frame_sink_manager_.
20:07:49.947  GPU process exited unexpectedly: exit_code=134
20:08:21      CV2-GPU-DEATH: 6 consecutive diagnostic ticks (30s) with zero captured
              frames — signalling session-unhealthy
20:08:24      supervisord: exited: chromium (exit status 139); spawned pid 254
```

The diag line five seconds *before* the resize read `issued=148 (29.6 fps)
acked=148 | frames_received +49`. The resize is the only event between a
healthy loop and a dead one. Reproduced at 19:56:02 (pid 21, pod `c8d5n`) and
20:26:03 (pid 254, pod `tlrjj`) with the same shape to the second.

Two layers, both in this repo:

1. **The freeze.** `CbViewportController::Apply` (`cb_viewport_controller.cc`)
   updates the headless display, resizes the aura host in pixels, resizes the
   `RenderWidgetHostView`, then re-pins the capturer. Reconfiguring the Display
   under a pending external BeginFrame drops the in-flight ack. This is the
   *same* mechanism `cb_begin_frame_driver.cc` already documents as "the
   2026-06-16 capture-start Show() Display reconfigure dropped the pending
   callback" — a resize is a second instance of that reconfigure, and nothing
   in the viewport path knew the driver existed (`grep -i beginframe
   cb_viewport_controller.cc` finds nothing).
2. **The crash.** The driver's watchdog waits `kWatchdogFiresBeforeReissue`
   (15) one-second fires, then re-issues. Its own comment says a re-issue into
   an *unbound* controller "is the double-issue that crashes the GPU process on
   viz_main_impl.cc:342 `!has_created_frame_sink_manager_`". That is the line
   in the log. So the wait-then-re-issue heuristic, written for the cold-boot
   and warm-restore cases, is wrong for this one: after a resize the controller
   is not merely stalled, it is being rebound, and 15 s is not long enough.

A minor third thing, client-side: every connect sends **two** resizes two
pixels apart (`854x590` then `856x592`, or `974x626` then `976x628`). Applying
the first changes the client layout by 2 px (the stage grows when the status
line changes), which re-fires the `ResizeObserver`. Harmless once the guest
survives a resize; today it doubles the chance.

## Why it looks like something else

- **In the logs it reads as GPU death.** `CV2-GPU-DEATH`, `exit_code=134`,
  `session_unhealthy` — everything points at the GPU/renderer, and the exit is
  the deliberate recycle for that. Look 50 s earlier for `Cb.setViewport`.
- **It reads as an audio bug.** The audio spec (`05`) is what first tripped
  it: viewer 2 of a run lands on the fresh process mid-boot and sees
  `bytesReceived=0`. The audio finding above is real and separate; this one
  was hiding behind it. Block `*/api/viewport` in the test browser to separate
  the two.
- **An old process is fine, so it reads as flaky.** Pod `5jkkr` (pid 120, up
  since 16:14 after ~15 sessions) took 14 resizes between 19:34 and 19:42 —
  the gateway log has every `viewport applied` — and kept decoding frames
  (`tests/interactive` 65/65 across them). Its log went with the pod, so
  whether it stalled and recovered or never stalled is not established. What
  IS established: three fresh processes (19:56, 20:07, 20:26), three crashes,
  identical timelines to the second. The fresh-process case is the one every
  deployment is in at boot and after every crash, so it is the one that
  matters; the old-process case is the clue to the precondition.

## The fix

1. **Make the viewport path BeginFrame-aware** (`cb_viewport_controller.cc`
   `Apply`, given the driver): either bracket the Display/host resize with
   `CbBeginFrameDriver::Stop()` / `Start()` (the driver drops in-flight acks on
   `Stop()` by design — see its `Stop()` comment), or add an
   `ExpectReconfigure()` that puts the watchdog back into the indefinite-wait
   mode it uses before the first ack, cleared by the next ack. The first is
   smaller and uses only existing surface.
2. **Never let the watchdog re-issue into a rebinding controller.** Even with
   (1), a re-issue that can crash the GPU process is a loaded gun; gate the
   re-issue branch on evidence the controller is bound (the ack epoch advanced
   since the stall began, or a compositor "display configured" signal), not on
   elapsed time.
3. **Client-side, today, no lane:** a gateway switch that answers
   `POST /api/viewport` with `501` when `CHROMELESS_VIEWPORT_FOLLOW=0`. The
   follower already stops after one failed request (`client/src/viewport.ts`,
   "one failure stops the follower"), so the guest is never resized and the
   stack behaves as it did before #96. Ten lines in `infra/gateway/viewport.go`
   plus the env line in `stack.yaml`/`compose.yaml`. Remove once (1) ships.
4. Collapse the 2 px oscillation: ignore a resize request whose size differs
   from the last *applied* size by less than, say, 4 px in each dimension.

## Verifying it

`tests/local/rearm-scenarios.spec.ts` gains a resize scenario: connect,
resize the Playwright viewport twice, and assert over the next 60 s that
`framesDecoded` keeps increasing, the worker log has no `stall watchdog fired`
past count 2, and its pid is unchanged. Against today's image that scenario
fails at the frames check ~1 s after the first resize and at the pid check
~50 s later. `tests/interactive`'s video suite should additionally assert
`frames_received` in the `[diag]` line is non-zero 20 s after connect, which
is the only thing that would have caught this at 65/65.

# Clock alignment for the latency harness

> **Companion to:** `reconcile.py` (T11) and the design doc in
> [`README.md`](./README.md).
> **Audience:** anyone running the harness — `qa-tester` primarily,
> plus any developer reproducing a regression locally.

The latency we want to measure is

```
glass_to_glass_ms = cam_capture_epoch_ms  -  source_emit_epoch_ms
```

Both timestamps live in **Unix epoch milliseconds**, but they're
recorded by two different machines (or two processes on one machine
with different clock domains). Whatever skew exists between those two
clocks lands directly in the latency number.

This document describes three procedures for keeping that skew small
enough that we can trust a 5–10 ms latency budget. Pick whichever
fits your environment.

---

## What "good enough" means

The v1 LAN latency target is **< 100 ms median** and the budget
breakdown allocates **5 ms** to one-way LAN network. Clock skew that
drifts slowly is much less of a problem than skew that's tens of
milliseconds and forgotten about. We aim for:

- **Skew between cam host and source host: ≤ 2 ms** — comparable to
  the network budget.
- **Skew drift over a 60-s recording: ≤ 1 ms** — keeps p95 stable.
- **Negative latencies: 0** — any negative number means the cam clock
  is *behind* the source clock by more than the offset you corrected
  for. This is always a bug; reconcile.py reports a count.

If your skew is worse than 2 ms, the harness still works for relative
comparisons (encoder A vs encoder B on the same hosts, same day) but
its absolute latency numbers cannot be trusted against the v1 budget.

---

## Procedure 1 — NTP-sync both hosts (recommended)

The source host (running the harness page / sink) and the cam host
(capturing video, running `reconcile.py`) both sync to the same NTP
server, ideally in the same room or building.

```bash
# Linux (systemd-timesyncd)
sudo timedatectl set-ntp true
timedatectl status     # confirm "System clock synchronized: yes"
chronyc tracking       # if you use chrony

# macOS
sudo sntp -sS time.apple.com
```

A LAN NTP server gets you sub-millisecond skew. Public NTP gets you
~10 ms — borderline. If you need more, use Procedure 2 in addition.

After syncing, pass `--clock-offset-ms 0` to `reconcile.py` (the
default) and run normally.

**How to verify:** record a 60-s loopback (Procedure 3) and confirm
the recovered latency is in the expected loopback range (typically
20–40 ms one-screen, depending on cam framerate). If you get
significantly negative numbers, the clocks are out of sync.

---

## Procedure 2 — Single host, two processes

If both the source page and the cam capture run on the same physical
machine (e.g. the harness page rendered in browser A on the host, and
a USB webcam on the same host pointed at browser B's viewport during
testing of a remote stream), the clocks are identical by construction
and `--clock-offset-ms 0` is correct.

This is the easy case and is what the bundled `sample-input/` self-
test exercises.

---

## Procedure 3 — Loopback offset measurement

When you can't trust NTP — for example, regional measurement across
machines you don't fully control — measure the offset directly with a
loopback recording.

1. **Set up.** Aim the webcam at the *source* display, not the
   client display. Open the harness page in fullscreen on the source.
   The webcam is now seeing the same flashes the source emitted, with
   no streaming or encoding in between — only the camera's own
   capture latency.
2. **Record.** 60 s, ≥ 30 fps.
3. **Reconcile.** Run `reconcile.py` against the recording with
   `--clock-offset-ms 0`. The recovered latency is approximately:

   ```
   loopback_latency = camera_capture_latency  +  clock_skew
   ```

   For typical USB webcams the camera-capture latency is dominated by
   `1 / fps + exposure_time + USB transfer ≈ 30–50 ms`. Anything
   *outside* that range is your skew.

4. **Set the offset.** If the loopback measures 80 ms but the camera
   spec sheet says ~35 ms is expected, your skew is 80 - 35 = 45 ms.
   Pass `--clock-offset-ms 45` for the real (cross-host) measurement.

This procedure is approximate; it's the fallback when NTP isn't
available. Document the camera model, fps, exposure setting, and
measured loopback latency in the run notes so future testers can
reproduce or override the offset.

---

## Procedure 4 — External strobe (for the truly skeptical)

Out of scope for v1, but worth noting: a hardware strobe driven by
GPIO with a known epoch time, photographed simultaneously with the
client display, gives sub-millisecond ground truth. We will revisit
this if Phase 2 measurements need < 5 ms confidence.

---

## Recording the recording's PTS=0 epoch

`reconcile.py` for video inputs needs to know what epoch ms
corresponds to the video's PTS=0. The most reliable way is to log it
when you start the recorder:

```bash
# Linux ffmpeg + jq
START_MS=$(date +%s%3N)
ffmpeg -f v4l2 -framerate 60 -video_size 1280x720 \
       -i /dev/video0 -c:v libx264 -preset ultrafast -crf 18 \
       -t 60 cam.mp4
echo "$START_MS" > cam.start.txt

# Then run reconcile with the recorded start:
DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib python3 reconcile.py \
    cam.mp4 --recording-start-epoch-ms "$(cat cam.start.txt)" \
    --out report-$(date +%s)
```

If you forget, `reconcile.py` falls back to `mtime - duration` and
emits a warning. The fallback is fine for relative comparisons,
unreliable for absolute latency.

---

## Sanity checks reconcile.py prints

After every run `reconcile.py` prints (and writes to `*-summary.txt`):

- `negative_count` — non-zero ⇒ clocks misaligned, fix before
  trusting numbers.
- `qr_decode_rate` — < 0.95 ⇒ cam setup needs attention (focus,
  exposure, lighting, framing) — see the physical-setup checklist
  in `README.md`.
- `min_ms` — should be **strictly positive** and within roughly 1
  vsync of the camera's own capture latency.

If any of these are off, fix the calibration before drawing
conclusions about pipeline latency.

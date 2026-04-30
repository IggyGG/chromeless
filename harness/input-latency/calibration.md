# Calibrating the input-latency harness

> **Companion to:** [`reconcile.py`](./reconcile.py) and the
> methodology doc in [`README.md`](./README.md).
> **Audience:** anyone running an input-latency measurement —
> `qa-tester` primarily, plus any developer reproducing a regression.

The hard part of input-latency measurement is the **keystroke side**.
The screen-response side is handled by the same QR pipeline as the
video-latency harness; clock alignment for that piece is covered in
[`../latency/calibrate.md`](../latency/calibrate.md). Read that
document first if you haven't.

This document describes the three supported methodologies for
identifying the keystroke moment in a webcam recording, when to use
each, and the accuracy you can expect.

---

## Pick a methodology

| Method | Accuracy (typical) | Setup difficulty | When to use |
| ------ | ------------------ | ---------------- | ----------- |
| **A. LED on a mechanical key** | ±5 ms      | Hard (one-time wiring) | Lab-grade runs; the only way under ~10 ms confidence. |
| **B. Phone stopwatch in frame** | ±30 ms p95 | Medium (annotate manifest by hand) | Quick measurements at a remote site. Bound your numbers with the OCR error band. |
| **C. Manual annotation**        | exact, but operator-bound | Easy | Default for everyday runs. Rely on operator cadence, ≥10 keystrokes per run. |

The reconcile script accepts the keystroke moments via either
`--manifest <jsonl>` (B and C) or `--led-roi x,y,w,h` (A); the
output format is the same.

---

## Method A — LED on a mechanical key

Wire an indicator LED across the contacts of a single mechanical key
on a test keyboard. The LED lights the moment the contacts close —
i.e. on the actual physical actuation — well *before* the host OS has
delivered a scancode. This is the closest you can get to "the moment
the user pressed the key" without bespoke hardware.

### Wiring sketch

```
                          5V ──[330Ω]──┐
                                       │
                                      LED (anode toward 5V)
                                       │
   key contact A  ────────────────────┤
                                      │
   key contact B  ─────── GND
```

Most cheap mechanical keyboards have accessible solder pads under the
switch; spacebar is the usual choice because operators can hit it
without looking. Don't drive the LED off the matrix scan signal —
those scans are short enough that a slow camera will miss most of
them. A continuous DC drive across the contacts makes the LED stay
on for the entire keypress, which is exactly what we want.

### Cam framing

Aim the cam so the LED is sharply visible in a small region of the
frame, well away from the screen. The cam must capture the LED **and**
the screen response in the same recording. A 720p cam at 60 fps
typically lets a 40 mm key sit ~80 px square in frame at arm's length;
that's plenty for ROI brightness detection.

### Reconcile

```bash
python3 reconcile.py cam.mp4 \
    --led-roi 80,540,160,160 \
    --led-threshold 200 \
    --recording-start-epoch-ms "$(cat cam.start.txt)" \
    --pair-window-ms 500 \
    --out report-XYZ
```

The `x,y,w,h` of the ROI is in pixel coords on the captured cam frame
(top-left origin). Threshold is on the 0–255 grayscale scale; 180–220
works for most LEDs against a dim background. The reconciler logs
"detected N keystrokes via LED ROI" so you can sanity-check.

### Expected accuracy

- LED rise time: ~1 ms.
- Cam frame discretisation at 60 fps: ±8.3 ms (one frame).
- Brightness threshold edge detection: ±1 frame, so ±8.3 ms more.

Total: ~±5 ms (the cam errors largely cancel between the keystroke
and response halves of the loop, since both come from the same cam).

---

## Method B — Phone stopwatch in frame

If you don't have a wired key, hold a phone running a stopwatch in
the cam's view next to the keyboard. The operator presses the key at
known stopwatch readings (e.g. on each integer second), then a
human transcribes the readings into a manifest JSONL.

### Stopwatch app requirements

- Centisecond display (the major-tick line under the digits doesn't
  count). 24-hour wall clock works too if synced to NTP — set it once
  with `sntp -sS time.apple.com` immediately before the run.
- Bright digits, dark background. Cheap stopwatch apps don't always
  honour the system colour scheme.

### Procedure

1. Start the stopwatch.
2. Start the cam recording. Note the cam's recording-start epoch ms
   (`date +%s%3N > cam.start.txt`).
3. Operator presses the key at 5-second intervals for at least 60 s
   (≥ 12 keystrokes; lets a single misread not dominate p95).
4. Stop the recording, stop the stopwatch.
5. Transcribe stopwatch readings at each keystroke into a JSONL:

   ```json
   {"camEpochMs": 1730290002000, "code": "Space", "keystrokeId": 1}
   {"camEpochMs": 1730290007000, "code": "Space", "keystrokeId": 2}
   ```

   The `camEpochMs` is your cam's recording-start-epoch-ms PLUS the
   stopwatch reading you observed at that keystroke. (Phase 2 may
   add OCR for this; for now, transcribe.)

### Reconcile

```bash
python3 reconcile.py cam.mp4 \
    --manifest keystrokes.jsonl \
    --recording-start-epoch-ms "$(cat cam.start.txt)" \
    --out report-XYZ
```

### Expected accuracy

- Cam frame discretisation at 60 fps: ±8 ms.
- Operator-eye-to-finger reaction is **not** in the loop because we
  read the stopwatch *after* the press, but human transcription
  precision is usually ±20 ms.
- Total: ±30 ms p95 in our notes. Report results with this band
  rather than as a point estimate.

---

## Method C — Manual annotation

Operator presses the key at known cam timestamps without any
external signal. Useful for quick checks where ±20 ms is
acceptable.

The operator notes the cam-clock time at each keystroke (a
clip-counter in the screen response is a good reference) and
transcribes them into the same JSONL format as Method B.

This is the right default for everyday runs where you want a
sanity-check number, not a publishable measurement.

---

## Common mistakes

- **Mixing webcam and phone clocks.** The keystroke time you put
  in the manifest must be in the *cam recording's* wall-clock
  domain, i.e. `recording-start-epoch-ms + position-in-recording`.
  If you accidentally use the phone's clock and the two clocks
  drift by 100 ms, every reported latency is wrong by 100 ms.
- **Shutter speed too long.** A cam in low light will set its
  shutter to 1/30 s, smearing the LED and the screen response
  across multiple frames. Lock the cam to ≤ 1/120 s (manual
  exposure) and add light if needed.
- **Wrong pair window.** The reconciler pairs each keystroke with
  the **first** subsequent QR-decoded response within
  `--pair-window-ms`. If the system is so slow that the first
  response inside the window belongs to the *next* keystroke,
  every pair will be misaligned by one. Tighten or widen
  `--pair-window-ms` to stay above your observed p99 and below
  your inter-keystroke gap.
- **Operator presses keys faster than the system can respond.**
  At very high keypress rates the system's keystroke→response
  pipeline coalesces frames; multiple keystrokes can map to one
  response. The reconciler discards extra responses (one response
  per keystroke). Slow the cadence to ≤ 2 Hz for clean pairs.

---

## Sanity checks reconcile.py prints

After every run `reconcile.py` prints (and writes to
`*-input-summary.txt`):

- `frames_seen`, `qr_response_frames`, `keystrokes`, `pairs` —
  if `pairs` is much less than `min(keystrokes, qr_response_frames)`
  the pair window or cam framing is off.
- `min_ms` — should be **strictly positive** (a negative input
  latency means clocks are misaligned; see
  [`../latency/calibrate.md`](../latency/calibrate.md)).
- `p50_ms` should sit in the budgeted range from
  [`docs/v1-success-criteria.md`](../../docs/v1-success-criteria.md)
  L5: < 80 ms median LAN.

If any of these are off, fix the calibration before drawing
conclusions about pipeline latency.

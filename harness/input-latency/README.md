# Input latency harness

> **Status:** Phase 1 (T39).
> **Owner:** `platform-dev` (parallel rig to T10/T11's video latency
> harness in [`../latency/`](../latency/)).

The project brief is explicit: *"Measure input latency separately
from video latency — they have different optimization paths."* This
directory is the parallel rig that does that.

## What "input latency" means here

End-to-end input latency is the time between the user **physically
pressing a key** and the user **seeing a response on screen**. In the
cloud-browser pipeline that's:

```
finger ──→ key switch ──→ keyboard scancode ──→ OS HID ──→ local
                       browser keydown event ──→ data channel ──→
                       signaling/relay ──→ cloud Chromium keydown ──→
                       JS handler runs + repaint ──→ encode ──→ network ──→
                       decode ──→ display compositor ──→ photons ──→ user
```

We measure **all of it**, like the video harness, by pointing a
webcam at the user's screen *and* the user's keyboard simultaneously.
The cam sees both halves of the loop. We don't trust software-side
keydown timestamps because they don't capture the input-transport
leg.

## How

```
            ┌────────────────────────────────────────────────────┐
            │ webcam pointed at:                                  │
            │                                                     │
            │   [ keyboard with LED      ]    [ client display    │
            │     OR phone stopwatch     ]      showing harness   │
            │     OR manual annotation   ]      page              │
            │                                                     │
            │   keystroke moment              screen-response     │
            │   visible HERE                  visible HERE        │
            │                                                     │
            │   Δt = end-to-end input latency                     │
            └────────────────────────────────────────────────────┘
```

1. The harness page (`index.html` + `page.js`) listens for `keydown`
   in the cloud Chromium tab. On each keystroke it:
   - synchronously sets the entire viewport to a known bright color
     (configurable via `?color=`),
   - on the immediately-following `requestAnimationFrame` draws a
     `v2-input|<runId>|<keydownEpochMs>|<keyCode>|<keystrokeId>` QR in
     the corner so the cam frame containing the response also
     contains a unique identifier,
   - emits a JSONL record to the same WebSocket sink T10's harness
     uses (different `runId` namespace, so the two harness's records
     don't collide).
2. The webcam records the operator's keystroke and the screen
   response in the same video.
3. The reconciliation script (`reconcile.py`):
   - extracts cam-epoch timestamps for each frame (manifest, video
     PTS + start epoch, or file mtime — same pipeline as T11),
   - loads the keystroke moments either from a `--manifest` JSONL
     supplied by the operator, or by detecting an LED ROI brightness
     transition (`--led-roi x,y,w,h --led-threshold 180`),
   - decodes the v2-input QR on each frame (pyzbar),
   - pairs each keystroke with the first subsequent QR-decoded
     response frame within `--pair-window-ms`,
   - reports per-pair latency + p50/p95/p99/max + a CSV +
     a matplotlib histogram.

## Running it

```bash
# Source side (page) — same shape as T10:
cd harness/input-latency
python3 -m http.server 8000
# open http://localhost:8000/?run=demo&color=%23ffffff&hold=200

# Server-side sink (reuse T10's):
cd ../latency
pip install -r requirements.txt
python3 server-sink.py --out-dir ../captures
```

Webcam recording — same as T10:

```bash
ffmpeg -f v4l2 -framerate 60 -video_size 1280x720 \
       -i /dev/video0 -c:v libx264 -preset ultrafast -crf 18 \
       -t 60 cam.mp4
echo $(date +%s%3N) > cam.start.txt    # capture PTS=0 epoch
```

Reconcile — pick one keystroke source:

```bash
# A. Manifest-driven (operator annotates keystrokes — most reliable):
python3 reconcile.py cam.mp4 \
    --manifest keystrokes.jsonl \
    --recording-start-epoch-ms "$(cat cam.start.txt)" \
    --pair-window-ms 500 \
    --out report-XYZ

# B. LED-ROI detection (mechanical key with LED visible at ROI):
python3 reconcile.py cam.mp4 \
    --led-roi 80,540,160,160 --led-threshold 200 \
    --recording-start-epoch-ms "$(cat cam.start.txt)" \
    --out report-XYZ
```

The bundled self-test (DoD) runs against pre-committed sample frames
and recovers known latencies exactly:

```bash
python3 reconcile.py harness/input-latency/sample-input/ \
    --manifest harness/input-latency/sample-input/keystrokes.jsonl
# === reconcile.py (input-latency) summary ===
#                    input: harness/input-latency/sample-input
#                    pairs: 3
#                   min_ms: 55.000
#                   p50_ms: 72.000
#                   max_ms: 89.000
```

## Page parameters

| param   | default                  | meaning                                                 |
| ------- | ------------------------ | ------------------------------------------------------- |
| `run`   | `inputlat-<unix-ms>`     | Run id; embedded in QR + JSONL filename.               |
| `sink`  | `ws://localhost:9001`    | WebSocket sink; same default as T10's flashing harness.|
| `color` | `#ffffff`                | Response flash color.                                  |
| `hold`  | `200` (ms)               | How long the flash stays on after a keystroke.         |
| `key`   | `""` (any)               | Limit triggers to one key (`Space`, `KeyA`, …).        |

`Ctrl+Shift+D` downloads the in-memory log as JSONL even if the sink
is offline.

## Reconcile parameters

See `reconcile.py --help`. The two important ones are:

- **`--pair-window-ms`** — maximum latency a pair can have. A keystroke
  with no response within this window is dropped. Default 500 ms.
  Tighten for high-rate runs, loosen for slow / impaired pipelines.
- **`--led-roi x,y,w,h --led-threshold T`** — detect keystrokes via
  brightness in a rectangular region. The ROI is in pixel coords on
  the captured cam frame; threshold is on the 0–255 grayscale scale.
  Useful when the operator can wire a mechanical key's LED into the
  webcam's view.

## Shared infrastructure

This harness deliberately reuses T10/T11 plumbing wherever the shape
matches:

- **Vendored QR library** — `../latency/vendor-qrcode.js` (reused
  via relative `<script>` import).
- **`encode-timestamp.js`** — the same generic
  `drawQrInto(canvas, payload, moduleSize)` helper. The payload
  schema differs (`v2-input|...` vs `v1|...`) so the reconcile
  script can disambiguate runs.
- **`requirements.txt`** — pyzbar, opencv, numpy, matplotlib already
  pinned in [`../latency/requirements.txt`](../latency/requirements.txt).
- **`server-sink.py`** — the page emits to the same sink as T10; the
  records go to `harness/captures/<runId>.jsonl` and don't collide
  because of the `inputlat-` runId prefix.

## Calibration

The keystroke-detection half is the messy half. See
[`calibration.md`](./calibration.md) for the three supported
methodologies (LED-equipped key, phone stopwatch in frame, manual
annotation) and when to pick each.

## Validation handoff

QA's overall harness validation (T12) covers the screen-response
half — the QR detection, frame timestamps, decode-rate sanity. The
keystroke half is currently validated by:

1. The bundled `sample-input/` self-test (3 known-latency pairs;
   reconciler must recover them exactly). Wired into
   [`tests/harness/input-latency-loopback.sh`](../../tests/harness/input-latency-loopback.sh)
   as a baseline.
2. The phone-stopwatch fallback's accuracy is bounded by the cam's
   ability to OCR the displayed numerals (~30 ms p95 in our notes).
   Runs done with this method should be reported with a confidence
   band rather than a point estimate.
3. Operator-annotated manifests are exact by construction; the only
   error source is the operator pressing a key at a time other than
   the recorded one. We recommend ≥ 10 keystrokes per run so a single
   misread doesn't dominate.

## Known gaps

- **No automatic phone-stopwatch OCR.** The README mentions it as a
  fallback methodology but the operator transcribes timestamps into a
  manifest manually for now.
- **Latency aliasing at low cam framerates.** With a 30 fps webcam
  the granularity is ±33 ms, which is comparable to the input budget.
  We recommend ≥ 60 fps for input-latency runs even though 30 fps is
  fine for video latency.
- **No correction for operator reaction time.** The reported latency
  is from `keystroke moment as seen by cam` to `screen response as
  seen by cam` — the time before the keystroke (operator decides to
  press) is not in the loop. This is the right thing for measuring
  the system, but be careful when comparing to "perceived
  responsiveness" benchmarks that include reaction time.

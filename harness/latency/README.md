# Glass-to-glass latency harness

> **Status:** Phase 0 design + source side.
> **Owner:** `platform-dev` (this directory) + `qa-tester` (validation
> methodology in `tests/harness/`).
> **Companion:** `harness/latency/reconcile.py` (T11) implements the
> receiver side that joins webcam-captured frames to source timestamps.

This harness measures **glass-to-glass** video latency for the cloud
browser: the time between a pixel changing color in the headless
Chromium tab and that color reaching the user's display. It is the
single source of truth for the latency budget defined in
[`docs/v1-success-criteria.md`](../../docs/v1-success-criteria.md).

## Why a flashing block + webcam (vs. clock-stamping in software)?

The fundamental problem with software-only "glass-to-glass" measurement
is that *nothing inside the system measures the actual photons*. Every
intermediate stage has its own clock, buffering, and instrumentation
gaps:

- **Capture** — `getDisplayMedia` doesn't reliably timestamp at the
  underlying surface paint.
- **Encode** — libwebrtc reports per-frame encode time, but mounting
  the encoded frame to a reference clock is implementation-specific.
- **Network** — RTP timestamps are encoder clock, not wall clock.
- **Decode** — browsers don't expose decode-out timestamps to JS at
  pixel granularity.
- **Display** — vsync, compositor, and panel response add 8–24 ms that
  no software clock sees.

A webcam pointed at the client display *is* the user's eye. It can't
lie about what's on the screen. The cost is fiddly physical setup;
the benefit is a measurement we trust.

## How it works

```
  ┌─────────────────────────┐                 ┌──────────────────────┐
  │ Headless Chromium       │                 │ Client browser       │
  │ (server-side, Phase 1)  │                 │ (this page rendered) │
  │                         │   capture +     │                      │
  │  ┌────────────────────┐ │   encode +      │  ┌────────────────┐  │
  │  │ harness/latency/   │ │   stream        │  │ Decoded frames │  │
  │  │   index.html       │─┼────────────────►│  │ shown on real  │  │
  │  │ flashes block,     │ │                 │  │ display        │  │
  │  │ stamps QR + bars   │ │                 │  └────────┬───────┘  │
  │  └─────────┬──────────┘ │                 └───────────┼──────────┘
  │            │ records to │                             │
  │            ▼            │                             ▼
  │  ┌────────────────────┐ │                  ┌─────────────────────┐
  │  │ server-sink.py     │ │                  │ webcam pointed at   │
  │  │ → run-X.jsonl      │ │                  │ client display      │
  │  └─────────┬──────────┘ │                  │ recording at 60 fps │
  │            │            │                  └──────────┬──────────┘
  └────────────┼────────────┘                             │
               │            ┌────────────────────────────┘
               │            │  cam.mp4
               ▼            ▼
       ┌──────────────────────────────┐
       │  reconcile.py (T11)          │
       │  - decode QR per cam frame   │
       │  - join to run-X.jsonl by    │
       │    frameId/runId             │
       │  - latency = camFrameTime    │
       │              - srcEpochMs    │
       │  - emit histogram + p50/p95  │
       └──────────────────────────────┘
```

For Phase 0 we don't yet have a Chromium-in-container source; we
exercise the harness against `index.html` running locally to validate
the methodology. Once Phase 1 ships, the *same* `index.html` is loaded
inside the cloud Chromium tab, the user's local browser becomes the
client, and the webcam points at the local display.

## Components

| File | What it is |
| ---- | ---------- |
| `index.html` | Fullscreen flashing color block. Toggles ON/OFF colors on a configurable period. Records timestamps to a websocket sink and to local memory. |
| `encode-timestamp.js` | Renders a payload string (`v1\|<runId>\|<frameId>\|<ON\|OFF>\|<epochMs>`) into a QR code on a `<canvas>`, using the vendored library. |
| `vendor-qrcode.js` | Vendored copy of `kazuhikoarase/qrcode-generator` (MIT). We do not pull from npm during Phase 0; the file is committed verbatim. |
| `server-sink.py` | Python WebSocket server (`ws://localhost:9001`) that appends each emitted record to `harness/captures/<runId>.jsonl`. |
| `requirements.txt` | Python deps for the sink and the (T11) reconciler. |
| `reconcile.py` | *(T11 — coming next)* OpenCV + pyzbar tool that joins webcam frames to JSONL records and emits latency stats. |

### The on-frame timestamp encoding

Each ON/OFF transition embeds the timestamp in **three** redundant
forms, in roughly decreasing order of robustness:

1. **QR code (top-left).** Primary path. ~250–350 px square. Holds the
   full payload including a run id and the ON/OFF state, so the
   reconciler can disambiguate flips even if it loses sync.
2. **Binary bars (top-right) — *human-diagnostic only*.** 16 black/white
   bars representing the low 16 bits of the frame counter, on a
   neutral grey background. **`reconcile.py` does NOT decode these
   today**; they exist purely so a human reviewing a recording can
   eyeball "did the frame id advance?" without QR machinery. The bars
   were originally pitched as an automatic fallback for motion blur /
   glare / small webcam, but in practice the QR pipeline at error-
   correction level M handles all the cases we've measured, so we
   haven't paid the cost of wiring the decoder. If you find a cam
   setup where QR consistently fails but the bars survive, file an
   issue — that's the signal to upgrade them from diagnostic to
   automatic. (See [`reconcile.py`](./reconcile.py) — the bars are
   not in its read path; QR is the only automated decoder.)
3. **Human-readable text (bottom-right).** Big, high-contrast,
   monospace. Useful for a tester eyeballing a recording or
   debugging when automated decoding fails.

The block itself flashes between two colors (`#000000` ↔ `#ff0000`
by default). The flash *itself* is the canonical latency signal —
the timestamps in the corners describe **which** flash a given
captured frame contains, but the latency value is computed from the
moment the cam first sees the new color.

### The flash timing

`requestAnimationFrame` gives us a `DOMHighResTimeStamp` that, per the
HTML spec, represents the *upcoming* paint time. We swap the block's
background, encode timestamps, and emit a record on the same rAF tick.
The recorded `perfMs`/`epochMs` is therefore the source-side estimate
of when those pixels go to the GPU; webcam-side latency is measured
against this.

This is best-effort but consistent — the systematic offset between
"rAF tick" and "actual pixel emission" on the source side is small
(< 1 vsync), and we calibrate it out during validation by recording
a self-loopback (cam pointed at the *source* display) and subtracting
the residual from regional/LAN measurements.

## Running it

### Source side (the page)

```bash
# from the repo root
cd harness/latency
python3 -m http.server 8000
# then open http://localhost:8000/?period=1000&run=demo-run
```

URL parameters:

| Param     | Default                  | Notes                                   |
| --------- | ------------------------ | --------------------------------------- |
| `period`  | `1000` (ms)              | Flash period. Use `2000` if your cam is < 30 fps. |
| `on`      | `#ff0000`                | ON color.                               |
| `off`     | `#000000`                | OFF color.                              |
| `sink`    | `ws://localhost:9001`    | WebSocket sink URL.                     |
| `run`     | `run-<unix-ms>`          | Run id; embed in QR + JSONL filename.   |

Keys:
- `f` — toggle fullscreen
- `p` — pause flashing (block goes neutral grey, no records emitted)
- `d` — download the in-memory log as JSONL (handy if the sink is offline)

### Sink (records timestamps)

```bash
cd harness/latency
pip install -r requirements.txt
python3 server-sink.py --out-dir ../captures
# writes ../captures/<runId>.jsonl, one record per line
```

### Webcam capture

Use whatever 60 fps recorder you have. Suggested pipeline:

```bash
# Linux, v4l2:
ffmpeg -f v4l2 -framerate 60 -video_size 1280x720 \
       -i /dev/video0 -c:v libx264 -preset ultrafast -crf 18 \
       -t 60 cam.mp4
```

Aim the cam at the *client* display, framing so the QR + flashing
block both fit. Record for at least 60 s for ≥300 transitions
(see L1/L2 in the v1 success criteria).

### Reconcile

*(Coming with T11.)*

```bash
python3 reconcile.py \
    --jsonl ../captures/<runId>.jsonl \
    --video cam.mp4 \
    --out  report-<runId>.html
```

## Physical setup

A few things matter more than people expect:

- **No autoexposure on the webcam.** Lock it (manual mode) — otherwise
  the flashing block triggers exposure changes that bleed into the
  next-frame timestamp.
- **No autofocus.** Same reason.
- **Bright but indirect ambient light.** Direct light glares the
  display; pitch dark causes the cam to underexpose during the OFF
  phase.
- **Cam framerate ≥ 2× flash frequency.** With the default 1 Hz flash
  and a 60 fps cam, we have ~30 frames per state to choose from.
- **Aim slightly off-axis** to avoid the cam picking up its own
  reflection on a glossy display.
- **Confirm the QR is sharp** — read it from a still frame before
  starting the run. If you can't decode it manually, the reconciler
  won't either.

## Validation (handoff to qa-tester / T12)

T12 covers methodology validation: cam-pointed-at-source loopback to
characterize systematic offset, repeatability across testers, and the
per-procedure pass/fail boundaries that feed back into
`docs/v1-success-criteria.md`. The harness itself is the input to
that work; corrections to methodology land here as ADRs under
[`docs/`](../../docs/).

## Open questions / future work

- **Audio latency.** Same harness shape applies (audio click + mic +
  reconcile) but is out of scope for v1; tracked separately.
- **Input round-trip latency.** L5 in the success-criteria doc — the
  reconciler will grow a mode that drives keystrokes and watches for
  rendered glyphs. Phase 1 deliverable.
- **Sub-frame timing.** Rolling-shutter cameras give us better than
  one-frame resolution in principle; we'll evaluate against the budget
  if the basic approach is too coarse.

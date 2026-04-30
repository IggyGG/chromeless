#!/usr/bin/env python3
"""reconcile.py — input-latency reconciliation.

Companion to T11's video-latency reconcile.py. The difference: this
one finds **pairs** of (keystroke flash, screen response flash) per
recording and reports the time between them.

Inputs
------
A directory of frames or a video, where each frame may carry:

  - a "keystroke" indicator — bright LED on a mechanical key, or a
    phone-stopwatch readout in frame, or a manual annotation in the
    optional manifest.jsonl;
  - a "screen response" indicator — a `v2-input|...` QR drawn by
    harness/input-latency/page.js encoding the keydown timestamp and
    a per-press keystrokeId;

…and an optional source-side JSONL emitted by server-sink.py (T10),
which we cross-reference for ground-truth keydownEpochMs and
keystrokeId mapping.

Pairing
-------
Each detected keystroke is paired with the **first** subsequent QR-
detected response frame within --pair-window-ms. The reported latency
is `response_frame_cam_epoch_ms - keystroke_frame_cam_epoch_ms`. We
prefer cam timestamps because those are what the user actually saw —
software-side keydown timestamps don't capture the input-transport leg.

If no keystroke detection is configured, the reconciler falls back to
"every QR-decoded response frame is its own measurement, latency is
inferred from the QR-encoded keydown timestamp" — equivalent to the
T11 approach but with a v2-input payload schema. The result is **not**
a true input-latency number; it characterises only the screen-response
half of the loop. Useful as a smoke test.

Outputs
-------
  - <out>/<runId>-input-latencies.csv (one row per pair)
  - <out>/<runId>-input-summary.txt   (count, p50/p95/p99/max)
  - <out>/<runId>-input-histogram.png (matplotlib)

Usage
-----
    # Self-test against the bundled sample
    python3 reconcile.py harness/input-latency/sample-input/

    # Real video with manifest-driven keystroke times
    python3 reconcile.py cam.mp4 \
        --manifest keystrokes.jsonl \
        --recording-start-epoch-ms 1730290000000 \
        --out report-XYZ

Dependencies are pinned in harness/latency/requirements.txt (shared
with T10/T11).
"""

from __future__ import annotations

import argparse
import csv
import json
import logging
import re
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path

log = logging.getLogger("harness.input")


# ---------------------------------------------------------------------------
# Lazy heavy imports so --help works without zbar.
# ---------------------------------------------------------------------------
def _lazy_imports():
    global cv2, np, pyzbar_decode, plt
    try:
        import cv2 as _cv2
        import numpy as _np
        from pyzbar.pyzbar import decode as _decode
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as _plt
    except ImportError as e:
        sys.stderr.write(
            f"ERROR: missing dependency ({e}). Install with:\n"
            "    pip install -r harness/latency/requirements.txt\n"
            "On macOS, also: brew install zbar (and set\n"
            "DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib).\n"
        )
        sys.exit(2)
    cv2, np, pyzbar_decode, plt = _cv2, _np, _decode, _plt


PAYLOAD_RE = re.compile(
    r"^v2-input\|(?P<run>[^|]*)\|(?P<keydown>\d+)\|(?P<code>[^|]*)\|(?P<id>\d+)$"
)


def parse_payload(payload: str) -> dict | None:
    m = PAYLOAD_RE.match(payload)
    if not m:
        return None
    return {
        "runId": m.group("run"),
        "keydownEpochMs": int(m.group("keydown")),
        "code": m.group("code"),
        "keystrokeId": int(m.group("id")),
    }


def decode_v2input_qr(img) -> str | None:
    for r in pyzbar_decode(img):
        try:
            text = r.data.decode("utf-8", errors="strict")
        except UnicodeDecodeError:
            continue
        if text.startswith("v2-input|"):
            return text
    return None


# ---------------------------------------------------------------------------
# Keystroke detection
# ---------------------------------------------------------------------------
@dataclass
class KeystrokeMark:
    """Time at which a physical keystroke happened, in cam wall-clock.

    Either supplied by an external manifest (preferred — operator
    annotates the recording) or detected via brightness in a region of
    interest (the LED detector below)."""

    cam_epoch_ms: int
    source: str  # "manifest" | "led-roi"
    extra: dict = field(default_factory=dict)


def load_keystroke_manifest(path: Path) -> list[KeystrokeMark]:
    """Load a list of keystroke epochs (ms) from JSONL.

    Format (one record per line):
        {"camEpochMs": 1730290000050, "code": "Space", "keystrokeId": 1}
    """
    out: list[KeystrokeMark] = []
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            out.append(KeystrokeMark(
                cam_epoch_ms=int(rec["camEpochMs"]),
                source="manifest",
                extra={k: v for k, v in rec.items() if k != "camEpochMs"},
            ))
    log.info("loaded %d keystroke marks from %s", len(out), path)
    return out


def detect_led_roi(
    frames: list[tuple[str, "any", int]],  # noqa: F722  (cv2 image typed at runtime)
    roi: tuple[int, int, int, int],
    threshold: float,
) -> list[KeystrokeMark]:
    """Detect keystrokes via brightness in a region of interest.

    The operator points the webcam so a key-actuated LED falls inside
    `roi` (x, y, w, h). We compute the mean brightness in that ROI per
    frame; transitions from below->above `threshold` are taken as
    keystroke moments.

    `threshold` is on the 0–255 grayscale scale.
    """
    out: list[KeystrokeMark] = []
    on = False
    x, y, w, h = roi
    for label, img, cam_epoch_ms in frames:
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        sub = gray[y:y + h, x:x + w]
        if sub.size == 0:
            continue
        v = float(sub.mean())
        if v >= threshold and not on:
            on = True
            out.append(KeystrokeMark(
                cam_epoch_ms=cam_epoch_ms,
                source="led-roi",
                extra={"label": label, "brightness": round(v, 1)},
            ))
        elif v < threshold and on:
            on = False
    log.info("detected %d keystrokes via LED ROI", len(out))
    return out


# ---------------------------------------------------------------------------
# Frame iteration — directory or video
# ---------------------------------------------------------------------------
def iter_dir_frames(root: Path):
    """Yield (label, image_bgr, cam_epoch_ms). Manifest preferred."""
    manifest_path = root / "manifest.jsonl"
    by_name: dict[str, dict] = {}
    if manifest_path.exists():
        with manifest_path.open() as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                rec = json.loads(line)
                by_name[rec["file"]] = rec

    files = sorted(
        p for p in root.iterdir()
        if p.is_file() and p.suffix.lower() in (".png", ".jpg", ".jpeg", ".bmp")
    )
    if not files:
        raise FileNotFoundError(f"no image files in {root}")
    for p in files:
        rec = by_name.get(p.name)
        if rec and "captureEpochMs" in rec:
            cam_epoch_ms = int(rec["captureEpochMs"])
        else:
            cam_epoch_ms = int(p.stat().st_mtime * 1000)
        img = cv2.imread(str(p))
        if img is None:
            continue
        yield p.name, img, cam_epoch_ms


def iter_video_frames(path: Path, start_epoch_ms: int | None):
    cap = cv2.VideoCapture(str(path))
    if not cap.isOpened():
        raise FileNotFoundError(f"open {path}")
    if start_epoch_ms is None:
        duration_ms = (cap.get(cv2.CAP_PROP_FRAME_COUNT)
                       / max(cap.get(cv2.CAP_PROP_FPS), 1.0)) * 1000
        end_epoch_ms = int(path.stat().st_mtime * 1000)
        start_epoch_ms = int(end_epoch_ms - duration_ms)
        log.warning(
            "no --recording-start-epoch-ms; approximating from mtime - duration"
        )
    idx = 0
    try:
        while True:
            ok, frame = cap.read()
            if not ok:
                break
            pos_ms = cap.get(cv2.CAP_PROP_POS_MSEC)
            yield f"video:{idx}", frame, int(start_epoch_ms + pos_ms)
            idx += 1
    finally:
        cap.release()


def iter_input(path: Path, start_epoch_ms: int | None):
    if path.is_dir():
        yield from iter_dir_frames(path)
    elif path.suffix.lower() in (".mp4", ".mov", ".mkv", ".avi", ".webm"):
        yield from iter_video_frames(path, start_epoch_ms)
    else:
        img = cv2.imread(str(path))
        if img is None:
            raise FileNotFoundError(path)
        yield path.name, img, int(path.stat().st_mtime * 1000)


# ---------------------------------------------------------------------------
# Reconciliation
# ---------------------------------------------------------------------------
@dataclass
class ResponseFrame:
    label: str
    cam_epoch_ms: int
    payload: str
    keystroke_id: int
    keydown_epoch_ms: int
    run_id: str
    code: str


@dataclass
class Pair:
    keystroke: KeystrokeMark
    response: ResponseFrame
    latency_ms: int


def reconcile(
    input_path: Path,
    out_dir: Path,
    keystrokes: list[KeystrokeMark],
    pair_window_ms: int,
    led_roi: tuple[int, int, int, int] | None,
    led_threshold: float,
    recording_start_epoch_ms: int | None,
) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)

    # First pass: collect frames with their cam epochs and read QRs.
    response_frames: list[ResponseFrame] = []
    cached_frames: list[tuple[str, "any", int]] = []  # for ROI detection second-pass
    seen = 0

    for label, img, cam_epoch_ms in iter_input(input_path, recording_start_epoch_ms):
        seen += 1
        cached_frames.append((label, img, cam_epoch_ms))
        payload = decode_v2input_qr(img)
        if not payload:
            continue
        parsed = parse_payload(payload)
        if not parsed:
            continue
        response_frames.append(ResponseFrame(
            label=label,
            cam_epoch_ms=cam_epoch_ms,
            payload=payload,
            keystroke_id=parsed["keystrokeId"],
            keydown_epoch_ms=parsed["keydownEpochMs"],
            run_id=parsed["runId"],
            code=parsed["code"],
        ))

    # If no keystrokes were supplied via manifest but a ROI was given,
    # detect them now from the cached frames.
    if not keystrokes and led_roi is not None:
        keystrokes = detect_led_roi(cached_frames, led_roi, led_threshold)

    # Pair: each keystroke gets the first subsequent response within
    # `pair_window_ms`. We sort both lists for a single linear pass.
    pairs: list[Pair] = []
    response_frames.sort(key=lambda r: r.cam_epoch_ms)
    keystrokes_sorted = sorted(keystrokes, key=lambda k: k.cam_epoch_ms)
    j = 0
    for ks in keystrokes_sorted:
        # Skip past any response that's older than this keystroke.
        while j < len(response_frames) and response_frames[j].cam_epoch_ms < ks.cam_epoch_ms:
            j += 1
        if j >= len(response_frames):
            break
        candidate = response_frames[j]
        delta = candidate.cam_epoch_ms - ks.cam_epoch_ms
        if delta > pair_window_ms:
            continue
        pairs.append(Pair(keystroke=ks, response=candidate, latency_ms=delta))
        j += 1  # one response per keystroke

    # Aggregate.
    latencies = [p.latency_ms for p in pairs]
    run_ids = sorted({p.response.run_id for p in pairs}) or ["unknown-run"]
    primary_run = run_ids[0]

    summary: dict = {
        "input": str(input_path),
        "frames_seen": seen,
        "qr_response_frames": len(response_frames),
        "keystrokes": len(keystrokes_sorted),
        "pairs": len(pairs),
        "pair_window_ms": pair_window_ms,
        "run_ids": run_ids,
    }
    if latencies:
        sorted_l = sorted(latencies)
        n = len(sorted_l)

        def pct(p):
            if n == 1:
                return float(sorted_l[0])
            k = (n - 1) * (p / 100.0)
            f = int(k)
            c = min(f + 1, n - 1)
            return sorted_l[f] + (sorted_l[c] - sorted_l[f]) * (k - f)

        summary.update({
            "min_ms": float(sorted_l[0]),
            "p50_ms": float(pct(50)),
            "p95_ms": float(pct(95)),
            "p99_ms": float(pct(99)),
            "max_ms": float(sorted_l[-1]),
            "mean_ms": float(statistics.fmean(sorted_l)),
            "stdev_ms": float(statistics.pstdev(sorted_l)) if n > 1 else 0.0,
        })

    # CSV.
    csv_path = out_dir / f"{primary_run}-input-latencies.csv"
    with csv_path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow([
            "keystroke_cam_epoch_ms", "keystroke_source",
            "response_label", "response_cam_epoch_ms",
            "keystroke_id", "keydown_epoch_ms", "code",
            "latency_ms",
        ])
        for p in pairs:
            w.writerow([
                p.keystroke.cam_epoch_ms, p.keystroke.source,
                p.response.label, p.response.cam_epoch_ms,
                p.response.keystroke_id, p.response.keydown_epoch_ms, p.response.code,
                p.latency_ms,
            ])

    # Histogram.
    hist_path = out_dir / f"{primary_run}-input-histogram.png"
    if latencies:
        fig, ax = plt.subplots(figsize=(8, 4.5))
        bins = min(40, max(5, len(latencies) // 2))
        ax.hist(latencies, bins=bins, edgecolor="black")
        ax.axvline(summary["p50_ms"], linestyle="--",
                   label=f"p50 = {summary['p50_ms']:.1f} ms")
        ax.axvline(summary["p95_ms"], linestyle=":",
                   label=f"p95 = {summary['p95_ms']:.1f} ms")
        ax.set_xlabel("Input latency (ms) — keystroke to screen response")
        ax.set_ylabel("Count")
        ax.set_title(f"Input latency — run={primary_run}, n={len(latencies)}")
        ax.legend()
        ax.grid(True, alpha=0.3)
        fig.tight_layout()
        fig.savefig(hist_path, dpi=120)
        plt.close(fig)
    elif hist_path.exists():
        hist_path.unlink()

    # Summary text.
    summary_path = out_dir / f"{primary_run}-input-summary.txt"
    with summary_path.open("w") as f:
        f.write("Input-latency summary\n")
        f.write("=====================\n")
        for k, v in summary.items():
            f.write(f"{k}: {v}\n")

    summary["csv"] = str(csv_path)
    summary["histogram"] = str(hist_path) if latencies else None
    summary["summary_text"] = str(summary_path)
    return summary


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def parse_roi(s: str) -> tuple[int, int, int, int]:
    parts = s.split(",")
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("ROI must be x,y,w,h")
    return tuple(int(p) for p in parts)  # type: ignore[return-value]


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description="Reconcile webcam-captured frames with v2-input QRs to "
        "compute end-to-end input latency.",
    )
    p.add_argument("input", type=Path,
                   help="Directory of frame PNGs, video file, or single image.")
    p.add_argument("--out", type=Path, default=Path("harness/captures"))
    p.add_argument("--manifest", type=Path, default=None,
                   help="JSONL of keystroke timestamps (preferred input).")
    p.add_argument("--led-roi", type=parse_roi, default=None,
                   help="Detect keystrokes via LED brightness in this ROI: x,y,w,h")
    p.add_argument("--led-threshold", type=float, default=180.0,
                   help="Grayscale threshold (0-255) for LED-on detection. Default 180.")
    p.add_argument("--pair-window-ms", type=int, default=500,
                   help="Max latency a pair can have. Larger windows let "
                   "spurious pairs match — keep this near your expected p99. "
                   "Default 500ms.")
    p.add_argument("--recording-start-epoch-ms", type=int, default=None)
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("--exit-nonzero-if-no-pairs", action="store_true")
    args = p.parse_args(argv)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    _lazy_imports()

    keystrokes: list[KeystrokeMark] = []
    if args.manifest is not None:
        keystrokes = load_keystroke_manifest(args.manifest)

    summary = reconcile(
        input_path=args.input,
        out_dir=args.out,
        keystrokes=keystrokes,
        pair_window_ms=args.pair_window_ms,
        led_roi=args.led_roi,
        led_threshold=args.led_threshold,
        recording_start_epoch_ms=args.recording_start_epoch_ms,
    )

    print()
    print("=== reconcile.py (input-latency) summary ===")
    for k, v in summary.items():
        if isinstance(v, float):
            print(f"  {k:>22}: {v:.3f}")
        else:
            print(f"  {k:>22}: {v}")
    print()

    if args.exit_nonzero_if_no_pairs and summary.get("pairs", 0) == 0:
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

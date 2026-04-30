#!/usr/bin/env python3
"""reconcile.py — recover glass-to-glass latency from a webcam capture.

Pipeline (see harness/latency/README.md for the design rationale):

  1. Take a webcam recording (mp4 / mov / mkv) OR a directory of frame
     PNGs (one frame per file).
  2. For each captured frame, decode the QR code that the harness page
     drew in the corner of the flashing block. The QR payload is
     `v1|<runId>|<frameId>|<ON|OFF>|<emitEpochMs>` — the source-side
     wall-clock time at which the page committed that flash.
  3. Compute the cam-side capture time for the same frame in the same
     wall-clock domain (epoch ms), using one of:
       - a manifest.jsonl in the directory (preferred for reproducible
         tests, including this script's self-test sample),
       - file mtime (when frames are PNGs whose timestamps are
         preserved end-to-end), or
       - video PTS + the recording's start epoch (passed via
         --recording-start-epoch-ms or read from CAP_PROP_POS_MSEC and
         a sidecar `start.txt`).
  4. Latency = cam_capture_epoch_ms - emit_epoch_ms (- clock_offset_ms).
     The clock_offset_ms is the source-vs-cam-host clock skew; see
     calibrate.md for how to measure it.
  5. Emit:
       - <out>/<runId>-latencies.csv  (per-frame)
       - <out>/<runId>-summary.txt    (counts, p50/p95/p99/max)
       - <out>/<runId>-histogram.png  (matplotlib)
     plus a stdout summary so CI can grep it.

Usage:
    # Self-test against the bundled sample frames.
    python3 reconcile.py harness/latency/sample-input/

    # Real run against a webcam mp4 with a known start epoch.
    python3 reconcile.py cam.mp4 \\
        --recording-start-epoch-ms 1714497120000 \\
        --clock-offset-ms 0 \\
        --out harness/captures/report-XYZ

    # Cross-reference with the source-side JSONL emitted by
    # server-sink.py for richer reporting (sink receipt time vs emit
    # time, missing-frame detection, etc.).
    python3 reconcile.py cam.mp4 \\
        --jsonl harness/captures/run-XYZ.jsonl \\
        --recording-start-epoch-ms 1714497120000

Dependencies are pinned in harness/latency/requirements.txt. On macOS,
pyzbar requires `brew install zbar` and may need
`DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib python3 reconcile.py …`.
"""

from __future__ import annotations

import argparse
import csv
import json
import logging
import os
import re
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path

log = logging.getLogger("harness.reconcile")


# ---------------------------------------------------------------------------
# Lazy heavy imports — pyzbar requires libzbar at import time, so we delay
# the import until we actually need it. This lets `--help` work in
# environments where libzbar isn't installed.
# ---------------------------------------------------------------------------
def _lazy_imports():
    global cv2, np, pyzbar_decode, plt
    try:
        import cv2 as _cv2  # noqa
        import numpy as _np
        from pyzbar.pyzbar import decode as _decode
        import matplotlib

        # Use a non-interactive backend so the script works headless / in CI.
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
    cv2 = _cv2
    np = _np
    pyzbar_decode = _decode
    plt = _plt


# ---------------------------------------------------------------------------
# Domain types
# ---------------------------------------------------------------------------
@dataclass
class FlashRecord:
    """Result of decoding one captured frame."""

    source_path: str        # filename or "video:<frameIndex>"
    cam_epoch_ms: int       # when the cam captured this frame, epoch ms
    qr_payload: str | None  # raw decoded payload, if any
    run_id: str | None
    frame_id: int | None
    state: str | None       # "ON" or "OFF"
    emit_epoch_ms: int | None  # source-side flash time
    latency_ms: float | None   # cam_epoch_ms - emit_epoch_ms - clock_offset
    ok: bool                # True iff QR decoded and payload parsed


PAYLOAD_RE = re.compile(
    r"^v1\|(?P<run>[^|]*)\|(?P<fid>\d+)\|(?P<state>ON|OFF)\|(?P<emit>\d+)$"
)


def parse_payload(payload: str) -> dict | None:
    m = PAYLOAD_RE.match(payload)
    if not m:
        return None
    return {
        "runId": m.group("run"),
        "frameId": int(m.group("fid")),
        "state": m.group("state"),
        "emitEpochMs": int(m.group("emit")),
    }


def decode_qr_in_image(img) -> str | None:
    """Return the first decoded QR payload in the image (or None)."""
    results = pyzbar_decode(img)
    for r in results:
        try:
            text = r.data.decode("utf-8", errors="strict")
        except UnicodeDecodeError:
            continue
        if text.startswith("v1|"):
            return text
    return None


# ---------------------------------------------------------------------------
# Frame iterators — image dirs, manifests, and video files
# ---------------------------------------------------------------------------
def iter_dir_frames(root: Path):
    """Yield (label, image_bgr, cam_epoch_ms) per frame in a directory.

    Capture time precedence:
      1. manifest.jsonl entry's `captureEpochMs` field (if present).
      2. The PNG/JPG file's mtime, in epoch ms.

    The manifest path makes the sample self-test deterministic; CI can
    overwrite mtimes during checkout.
    """
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
        log.info("loaded manifest: %d entries from %s", len(by_name), manifest_path)

    image_files = sorted(
        p for p in root.iterdir()
        if p.is_file() and p.suffix.lower() in (".png", ".jpg", ".jpeg", ".bmp")
    )
    if not image_files:
        raise FileNotFoundError(f"no image files in {root}")

    for p in image_files:
        manifest_rec = by_name.get(p.name)
        if manifest_rec and "captureEpochMs" in manifest_rec:
            cam_epoch_ms = int(manifest_rec["captureEpochMs"])
        else:
            cam_epoch_ms = int(p.stat().st_mtime * 1000)
        img = cv2.imread(str(p))
        if img is None:
            log.warning("could not read %s", p)
            continue
        yield p.name, img, cam_epoch_ms


def iter_video_frames(path: Path, start_epoch_ms: int | None):
    """Yield (label, frame_bgr, cam_epoch_ms) for each frame in a video.

    `start_epoch_ms` is the epoch ms at PTS=0. If None we fall back to
    the file's mtime minus its duration, which is approximate.
    """
    cap = cv2.VideoCapture(str(path))
    if not cap.isOpened():
        raise FileNotFoundError(f"could not open video {path}")

    if start_epoch_ms is None:
        # Approximate: assume mtime ≈ end-of-recording.
        duration_ms = cap.get(cv2.CAP_PROP_FRAME_COUNT) / max(
            cap.get(cv2.CAP_PROP_FPS), 1.0
        ) * 1000
        end_epoch_ms = int(path.stat().st_mtime * 1000)
        start_epoch_ms = int(end_epoch_ms - duration_ms)
        log.warning(
            "no --recording-start-epoch-ms; approximating as mtime - duration "
            "(%d). Consider passing --recording-start-epoch-ms for accuracy.",
            start_epoch_ms,
        )

    idx = 0
    try:
        while True:
            ok, frame = cap.read()
            if not ok:
                break
            pos_ms = cap.get(cv2.CAP_PROP_POS_MSEC)
            cam_epoch_ms = int(start_epoch_ms + pos_ms)
            yield f"video:{idx}", frame, cam_epoch_ms
            idx += 1
    finally:
        cap.release()


def iter_input(input_path: Path, start_epoch_ms: int | None):
    if input_path.is_dir():
        yield from iter_dir_frames(input_path)
    elif input_path.suffix.lower() in (".mp4", ".mov", ".mkv", ".avi", ".webm"):
        yield from iter_video_frames(input_path, start_epoch_ms)
    else:
        # Single image file.
        img = cv2.imread(str(input_path))
        if img is None:
            raise FileNotFoundError(f"cannot read {input_path}")
        cam_epoch_ms = int(input_path.stat().st_mtime * 1000)
        yield input_path.name, img, cam_epoch_ms


# ---------------------------------------------------------------------------
# Optional source-side JSONL cross-reference
# ---------------------------------------------------------------------------
def load_source_jsonl(path: Path) -> dict[tuple[str, int], dict]:
    """Index source-side records by (runId, frameId)."""
    by_key: dict[tuple[str, int], dict] = {}
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if rec.get("type") != "flash":
                continue
            key = (rec.get("runId", ""), int(rec["frameId"]))
            by_key[key] = rec
    log.info("loaded source jsonl: %d records from %s", len(by_key), path)
    return by_key


# ---------------------------------------------------------------------------
# Nyquist 2× rule (G2)
# ---------------------------------------------------------------------------
#
# Per tests/harness/validation.md §1.1, a tester running with
#   cam_fps < 2 × flash_freq
# produces aliased measurements that look superficially valid but are
# meaningless: the cam misses every other transition and the
# reconciler emits big spikes with no warning. We surface the warning
# at runtime so the operator catches the misconfiguration.
#
# The check needs two numbers:
#   - cam_fps   — inferred from per-frame cam_epoch_ms deltas (works
#                 for both video files and frame directories).
#   - flash_freq — preferred: from a --jsonl source-side log of every
#                  emitted flash (authoritative). Fallback: inferred
#                  from QR-decoded emit_epoch_ms deltas in the
#                  recording — but note this is the cam-aliased rate,
#                  not necessarily the true flash rate.
#
# Both inferences need a minimum sample size to be trustworthy. We
# require >= 5 frames before issuing a warning; smaller sample sizes
# silently skip the check (true for the bundled sample-input self-
# test, which has only 4 frames).
def infer_cam_fps_hz(records: list[FlashRecord], min_frames: int = 5) -> float | None:
    if len(records) < min_frames:
        return None
    deltas: list[int] = []
    prev_cam = records[0].cam_epoch_ms
    for r in records[1:]:
        d = r.cam_epoch_ms - prev_cam
        if d > 0:
            deltas.append(d)
        prev_cam = r.cam_epoch_ms
    if len(deltas) < min_frames - 1:
        return None
    median_ms = statistics.median(deltas)
    if median_ms <= 0:
        return None
    return 1000.0 / median_ms


def infer_flash_freq_hz_from_records(
    records: list[FlashRecord], min_frames: int = 5
) -> float | None:
    """Fallback inference of flash frequency from QR-decoded emit
    timestamps in the recording. NOTE: this is the *cam-observed*
    flash rate, which may be aliased lower than the true rate. Use
    --jsonl for authoritative inference instead when available."""
    decoded = [r for r in records if r.emit_epoch_ms is not None]
    if len(decoded) < min_frames:
        return None
    # Distinct emit times in encounter order — collapse multiple
    # cam frames showing the same flash.
    distinct: list[int] = []
    for r in decoded:
        if not distinct or r.emit_epoch_ms != distinct[-1]:
            distinct.append(r.emit_epoch_ms)
    if len(distinct) < min_frames - 1:
        return None
    deltas = [b - a for a, b in zip(distinct, distinct[1:]) if b > a]
    if not deltas:
        return None
    median_ms = statistics.median(deltas)
    if median_ms <= 0:
        return None
    return 1000.0 / median_ms


def infer_flash_freq_hz_from_jsonl(jsonl_records: list[dict],
                                   min_records: int = 5) -> float | None:
    """Authoritative inference of flash frequency from a source-side
    JSONL log of every emit. Each record carries `epochMs`; consecutive
    deltas give the period."""
    epochs = sorted(int(r["epochMs"]) for r in jsonl_records if "epochMs" in r)
    if len(epochs) < min_records:
        return None
    deltas = [b - a for a, b in zip(epochs, epochs[1:]) if b > a]
    if not deltas:
        return None
    median_ms = statistics.median(deltas)
    if median_ms <= 0:
        return None
    return 1000.0 / median_ms


# ---------------------------------------------------------------------------
# Main reconciliation
# ---------------------------------------------------------------------------
def reconcile(
    input_path: Path,
    out_dir: Path,
    clock_offset_ms: float,
    recording_start_epoch_ms: int | None,
    source_jsonl: Path | None,
) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    source_index = load_source_jsonl(source_jsonl) if source_jsonl else {}
    # Keep a flat list of flash records too, for Nyquist inference.
    source_jsonl_flash_records: list[dict] = []
    if source_jsonl is not None and source_jsonl.exists():
        with source_jsonl.open() as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if rec.get("type") == "flash":
                    source_jsonl_flash_records.append(rec)

    records: list[FlashRecord] = []
    decoded = 0
    seen = 0

    for label, img, cam_epoch_ms in iter_input(input_path, recording_start_epoch_ms):
        seen += 1
        payload = decode_qr_in_image(img)
        parsed = parse_payload(payload) if payload else None

        rec = FlashRecord(
            source_path=label,
            cam_epoch_ms=cam_epoch_ms,
            qr_payload=payload,
            run_id=parsed["runId"] if parsed else None,
            frame_id=parsed["frameId"] if parsed else None,
            state=parsed["state"] if parsed else None,
            emit_epoch_ms=parsed["emitEpochMs"] if parsed else None,
            latency_ms=None,
            ok=False,
        )

        if parsed:
            decoded += 1
            # Prefer source-side JSONL emit time when available — it's
            # the authoritative timestamp; the QR payload may have been
            # truncated or corrupted in transit.
            key = (parsed["runId"], parsed["frameId"])
            authoritative_emit = source_index.get(key, {}).get("epochMs")
            emit = authoritative_emit if authoritative_emit else parsed["emitEpochMs"]
            rec.emit_epoch_ms = int(emit)
            rec.latency_ms = float(cam_epoch_ms - emit - clock_offset_ms)
            # Negative latencies are physically impossible — they mean
            # the cam clock is behind the source clock by more than the
            # offset we corrected for. Flag but keep them.
            rec.ok = True

        records.append(rec)

    # ---- Aggregate ----
    latencies = [r.latency_ms for r in records if r.latency_ms is not None]
    run_ids = sorted({r.run_id for r in records if r.run_id})
    primary_run = run_ids[0] if run_ids else "unknown-run"

    summary = {
        "input": str(input_path),
        "frames_seen": seen,
        "qr_decoded": decoded,
        "qr_decode_rate": (decoded / seen) if seen else 0.0,
        "latency_count": len(latencies),
        "run_ids": run_ids,
        "clock_offset_ms": clock_offset_ms,
    }

    if latencies:
        sorted_l = sorted(latencies)
        n = len(sorted_l)

        def pct(p):
            # Linear-interpolation percentile, suitable for small N.
            if n == 1:
                return sorted_l[0]
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
            "negative_count": sum(1 for x in latencies if x < 0),
        })

    # ---- Nyquist 2× rule (G2) ----
    cam_fps = infer_cam_fps_hz(records)
    if source_jsonl_flash_records:
        flash_freq = infer_flash_freq_hz_from_jsonl(source_jsonl_flash_records)
        flash_source = "jsonl"
    else:
        flash_freq = infer_flash_freq_hz_from_records(records)
        flash_source = "qr-deltas"

    if cam_fps is not None:
        summary["cam_fps_hz"] = round(cam_fps, 3)
    if flash_freq is not None:
        summary["flash_freq_hz"] = round(flash_freq, 3)
        summary["flash_freq_source"] = flash_source

    aliased = False
    if cam_fps is not None and flash_freq is not None:
        if cam_fps < 2.0 * flash_freq:
            aliased = True
            log.warning(
                "cam_fps=%.3f < 2 × flash_freq=%.3f; results aliased",
                cam_fps, flash_freq,
            )
    summary["aliased_warning"] = aliased

    # ---- CSV ----
    csv_path = out_dir / f"{primary_run}-latencies.csv"
    with csv_path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow([
            "source_path", "cam_epoch_ms", "run_id", "frame_id", "state",
            "emit_epoch_ms", "latency_ms", "qr_payload",
        ])
        for r in records:
            w.writerow([
                r.source_path, r.cam_epoch_ms, r.run_id or "",
                r.frame_id if r.frame_id is not None else "",
                r.state or "",
                r.emit_epoch_ms if r.emit_epoch_ms is not None else "",
                f"{r.latency_ms:.3f}" if r.latency_ms is not None else "",
                r.qr_payload or "",
            ])

    # ---- Histogram PNG ----
    hist_path = out_dir / f"{primary_run}-histogram.png"
    if latencies:
        fig, ax = plt.subplots(figsize=(8, 4.5))
        # Sensible default bin count for small/large N.
        bins = min(40, max(5, len(latencies) // 2))
        ax.hist(latencies, bins=bins, edgecolor="black")
        ax.axvline(summary["p50_ms"], linestyle="--", label=f"p50 = {summary['p50_ms']:.1f} ms")
        ax.axvline(summary["p95_ms"], linestyle=":", label=f"p95 = {summary['p95_ms']:.1f} ms")
        ax.set_xlabel("Glass-to-glass latency (ms)")
        ax.set_ylabel("Count")
        ax.set_title(f"Latency — run={primary_run}, n={len(latencies)}")
        ax.legend()
        ax.grid(True, alpha=0.3)
        fig.tight_layout()
        fig.savefig(hist_path, dpi=120)
        plt.close(fig)
    else:
        # Don't leave a stale histogram around.
        if hist_path.exists():
            hist_path.unlink()

    # ---- Summary text ----
    summary_path = out_dir / f"{primary_run}-summary.txt"
    with summary_path.open("w") as f:
        f.write("Glass-to-glass latency summary\n")
        f.write("==============================\n")
        for k, v in summary.items():
            f.write(f"{k}: {v}\n")
        # The Nyquist warning gets a top-level prefixed line so
        # operators grepping the summary for "WARN:" find it
        # regardless of which key it came from. Format is stable;
        # tests/harness/aliased-warning-baseline.sh greps for it.
        if aliased:
            f.write(
                f"WARN: cam_fps={cam_fps:.3f} < 2 × flash_freq={flash_freq:.3f}; "
                "results aliased\n"
            )

    summary["csv"] = str(csv_path)
    summary["histogram"] = str(hist_path) if latencies else None
    summary["summary_text"] = str(summary_path)
    return summary


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description="Reconcile webcam-captured frames with source-side "
        "timestamps to compute glass-to-glass latency.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        "input",
        type=Path,
        help="Path to a directory of frame PNGs, a video file, or a single image.",
    )
    p.add_argument(
        "--out", type=Path, default=Path("harness/captures"),
        help="Output directory for CSV/histogram/summary (default: harness/captures).",
    )
    p.add_argument(
        "--clock-offset-ms", type=float, default=0.0,
        help="Subtract this from cam_epoch_ms before computing latency. "
        "Use the value measured during loopback calibration "
        "(see calibrate.md). Default: 0.",
    )
    p.add_argument(
        "--recording-start-epoch-ms", type=int, default=None,
        help="For video inputs: epoch ms at the recording's PTS=0. "
        "If omitted, approximated from the file's mtime minus duration "
        "(less accurate).",
    )
    p.add_argument(
        "--jsonl", type=Path, default=None,
        help="Optional source-side JSONL (from server-sink.py). When "
        "provided, emit_epoch_ms is taken from the JSONL rather than "
        "the QR payload.",
    )
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument(
        "--exit-nonzero-if-no-decodes", action="store_true",
        help="Exit with code 3 if zero QR codes decoded (useful in CI).",
    )
    p.add_argument(
        "--strict", action="store_true",
        help="Exit with code 4 when the Nyquist 2× rule "
        "(cam_fps >= 2 × flash_freq) is violated. By default we "
        "still emit a WARN line but exit 0.",
    )

    args = p.parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    _lazy_imports()

    summary = reconcile(
        input_path=args.input,
        out_dir=args.out,
        clock_offset_ms=args.clock_offset_ms,
        recording_start_epoch_ms=args.recording_start_epoch_ms,
        source_jsonl=args.jsonl,
    )

    print()
    print("=== reconcile.py summary ===")
    for k, v in summary.items():
        if isinstance(v, float):
            print(f"  {k:>22}: {v:.3f}")
        else:
            print(f"  {k:>22}: {v}")
    if summary.get("aliased_warning"):
        # Mirror the summary-text WARN format on stdout so CI greps
        # find it whether they read the summary file or stdout.
        print(
            f"\nWARN: cam_fps={summary['cam_fps_hz']:.3f} < "
            f"2 × flash_freq={summary['flash_freq_hz']:.3f}; results aliased"
        )
    print()

    if args.exit_nonzero_if_no_decodes and summary.get("qr_decoded", 0) == 0:
        return 3
    if args.strict and summary.get("aliased_warning"):
        return 4
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

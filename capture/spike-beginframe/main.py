#!/usr/bin/env python3
"""
T28 capture spike — drive Chromium frame production via DevTools and
measure the result.

Two capture modes are exercised here:
  * begin_frame: HeadlessExperimental.beginFrame with `screenshot` set,
    which lets us pace frame production deterministically and read back
    the rasterized result in the same call. This is what we'd want for
    a Phase-2 capture path: predictable frame intervals.
  * screencast: Page.startScreencast, which gives us frames at the
    browser's natural cadence — no pacing control. Included as the
    baseline that "stock CDP screencast" would deliver.

This is a research spike per PROJECT_BRIEF.md Phase 2 — the goal is to
prove the hooks work and produce real frame-interval numbers, not to
build production capture. See README.md and findings.md.

Cross-references:
  - docs/capture/path-of-least-resistance.md (T15 — Phase 1 path we are
    measuring against)
  - capture/spike-beginframe/measurements.md
  - capture/spike-beginframe/findings.md
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from contextlib import asynccontextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Any

try:
    import aiohttp
    import websockets
except ImportError as exc:  # pragma: no cover
    sys.exit(f"missing dep: {exc}; pip install aiohttp websockets")


CHROME_CANDIDATES = [
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
    "/usr/bin/chromium",
    "/usr/bin/chromium-browser",
    "/usr/bin/google-chrome",
    "/usr/bin/google-chrome-stable",
]


def find_chrome() -> str:
    for cand in CHROME_CANDIDATES:
        if Path(cand).exists():
            return cand
    if (which := shutil.which("chromium")) or (which := shutil.which("google-chrome")):
        return which
    sys.exit("no Chrome/Chromium binary found; set CHROME_BIN env var")


@asynccontextmanager
async def launch_chrome(*, port: int, headless_mode: str = "new",
                        width: int = 1280, height: int = 720):
    """Launch a fresh Chromium with --remote-debugging-port. Yields the
    Popen handle; tears down on exit."""
    chrome = os.environ.get("CHROME_BIN") or find_chrome()
    profile = tempfile.mkdtemp(prefix="spike-profile-")
    args = [
        chrome,
        f"--headless={headless_mode}",
        f"--remote-debugging-port={port}",
        f"--user-data-dir={profile}",
        f"--window-size={width},{height}",
        "--no-first-run",
        "--no-default-browser-check",
        "--disable-features=Translate,MediaRouter",
        "--disable-background-networking",
        "--disable-sync",
        "--no-sandbox",
        "--hide-scrollbars",
        "--enable-features=NetworkService,NetworkServiceInProcess",
        # Open a colourful animated page so frame production is non-trivial.
        f"file://{(Path(__file__).parent / 'workload.html').resolve()}",
    ]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        # Wait for the debug port to be reachable.
        deadline = time.time() + 15
        async with aiohttp.ClientSession() as sess:
            while time.time() < deadline:
                try:
                    async with sess.get(f"http://127.0.0.1:{port}/json/version", timeout=1) as r:
                        if r.status == 200:
                            break
                except (aiohttp.ClientError, asyncio.TimeoutError):
                    pass
                await asyncio.sleep(0.1)
            else:
                raise RuntimeError("Chrome remote-debug port did not open")
            async with sess.get(f"http://127.0.0.1:{port}/json/list") as r:
                pages = await r.json()
        target = next((p for p in pages if p.get("type") == "page"), None)
        if not target:
            raise RuntimeError(f"no page target found: {pages!r}")
        yield proc, target
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(profile, ignore_errors=True)


@dataclass
class CdpClient:
    ws: Any  # websockets connection
    next_id: int = 1
    pending: dict[int, asyncio.Future] = None
    events: asyncio.Queue = None

    async def call(self, method: str, params: dict | None = None) -> dict:
        msg_id = self.next_id
        self.next_id += 1
        fut: asyncio.Future = asyncio.get_event_loop().create_future()
        self.pending[msg_id] = fut
        await self.ws.send(json.dumps({"id": msg_id, "method": method,
                                        "params": params or {}}))
        return await fut

    async def reader(self) -> None:
        try:
            async for raw in self.ws:
                msg = json.loads(raw)
                if "id" in msg:
                    fut = self.pending.pop(msg["id"], None)
                    if fut and not fut.done():
                        if "error" in msg:
                            fut.set_exception(RuntimeError(msg["error"]))
                        else:
                            fut.set_result(msg.get("result", {}))
                else:
                    await self.events.put(msg)
        except websockets.ConnectionClosed:
            pass


@dataclass
class FrameMetrics:
    frames: int
    intervals_ms: list[float]
    bytes_total: int

    def summary(self) -> dict:
        if len(self.intervals_ms) < 2:
            return {"frames": self.frames, "note": "too few intervals"}
        s = sorted(self.intervals_ms)
        n = len(s)
        return {
            "frames": self.frames,
            "intervals_observed": n,
            "p50_ms": round(s[n // 2], 2),
            "p95_ms": round(s[int(n * 0.95)], 2),
            "p99_ms": round(s[min(n - 1, int(n * 0.99))], 2),
            "min_ms": round(s[0], 2),
            "max_ms": round(s[-1], 2),
            "mean_ms": round(statistics.fmean(s), 2),
            "stdev_ms": round(statistics.pstdev(s), 2),
            "bytes_total": self.bytes_total,
        }


async def run_begin_frame(client: CdpClient, *, frames: int, target_fps: int,
                          out_dir: Path) -> FrameMetrics:
    """Pace frames at target_fps via HeadlessExperimental.beginFrame."""
    await client.call("Page.enable")
    period_s = 1.0 / target_fps
    intervals: list[float] = []
    total_bytes = 0
    out_dir.mkdir(parents=True, exist_ok=True)
    last_t = None
    captured = 0
    for i in range(frames):
        target = (i + 1) * period_s
        sched = time.perf_counter()
        try:
            res = await client.call("HeadlessExperimental.beginFrame", {
                "frameTimeTicks": (time.time() * 1000),
                "interval": period_s * 1000,
                "noDisplayUpdates": False,
                "screenshot": {"format": "png", "quality": 80},
            })
        except RuntimeError as e:
            print(f"[spike] beginFrame failed at frame {i}: {e}", file=sys.stderr)
            return FrameMetrics(frames=captured, intervals_ms=intervals,
                                bytes_total=total_bytes)
        now = time.perf_counter()
        if last_t is not None:
            intervals.append((now - last_t) * 1000.0)
        last_t = now
        data = res.get("screenshotData")
        if data:
            raw = base64.b64decode(data)
            total_bytes += len(raw)
            if captured < 5 or captured == frames - 1:
                # Keep a few sample images, not all 100.
                (out_dir / f"frame-{captured:04d}.png").write_bytes(raw)
            captured += 1
        # Keep cadence: sleep until next deadline if we're running early.
        slack = target - (time.perf_counter() - (sched - i * period_s))
        if slack > 0:
            await asyncio.sleep(min(slack, period_s))
    return FrameMetrics(frames=captured, intervals_ms=intervals,
                         bytes_total=total_bytes)


async def run_screencast(client: CdpClient, *, frames: int,
                         out_dir: Path) -> FrameMetrics:
    """Passive capture via Page.startScreencast — the browser drives."""
    await client.call("Page.enable")
    intervals: list[float] = []
    total_bytes = 0
    out_dir.mkdir(parents=True, exist_ok=True)
    captured = 0
    last_t = None
    await client.call("Page.startScreencast", {
        "format": "png",
        "everyNthFrame": 1,
        "maxWidth": 1280,
        "maxHeight": 720,
    })
    deadline = time.perf_counter() + 30  # 30s upper bound for the spike.
    while captured < frames and time.perf_counter() < deadline:
        try:
            msg = await asyncio.wait_for(client.events.get(), timeout=2)
        except asyncio.TimeoutError:
            continue
        if msg.get("method") != "Page.screencastFrame":
            continue
        params = msg.get("params", {})
        sid = params.get("sessionId")
        data = params.get("data")
        now = time.perf_counter()
        if last_t is not None:
            intervals.append((now - last_t) * 1000.0)
        last_t = now
        if data:
            raw = base64.b64decode(data)
            total_bytes += len(raw)
            if captured < 5 or captured == frames - 1:
                (out_dir / f"sc-{captured:04d}.png").write_bytes(raw)
            captured += 1
        if sid is not None:
            try:
                await client.call("Page.screencastFrameAck", {"sessionId": sid})
            except RuntimeError:
                pass
    await client.call("Page.stopScreencast")
    return FrameMetrics(frames=captured, intervals_ms=intervals,
                         bytes_total=total_bytes)


async def main(args: argparse.Namespace) -> int:
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    results: dict[str, dict] = {}
    async with launch_chrome(port=args.port) as (_proc, target):
        ws_url = target["webSocketDebuggerUrl"]
        async with websockets.connect(ws_url, max_size=64 * 1024 * 1024) as ws:
            client = CdpClient(ws=ws, pending={}, events=asyncio.Queue())
            reader_task = asyncio.create_task(client.reader())
            try:
                if "beginframe" in args.modes:
                    print(f"[spike] beginFrame run: {args.frames} frames @ {args.fps} fps")
                    bf = await run_begin_frame(client, frames=args.frames,
                                                target_fps=args.fps,
                                                out_dir=out / "begin_frame")
                    results["begin_frame"] = bf.summary()
                    print(json.dumps(results["begin_frame"], indent=2))
                if "screencast" in args.modes:
                    print(f"[spike] screencast run: up to {args.frames} frames")
                    sc = await run_screencast(client, frames=args.frames,
                                                 out_dir=out / "screencast")
                    results["screencast"] = sc.summary()
                    print(json.dumps(results["screencast"], indent=2))
            finally:
                reader_task.cancel()
                try:
                    await reader_task
                except asyncio.CancelledError:
                    pass
    summary_path = out / "summary.json"
    summary_path.write_text(json.dumps(results, indent=2))
    print(f"[spike] wrote {summary_path}")
    return 0


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser()
    p.add_argument("--frames", type=int, default=100)
    p.add_argument("--fps", type=int, default=30)
    p.add_argument("--port", type=int, default=9222)
    p.add_argument("--out", default="capture/spike-beginframe/out")
    p.add_argument("--modes", nargs="+",
                   choices=["beginframe", "screencast"],
                   default=["beginframe", "screencast"])
    return p.parse_args()


if __name__ == "__main__":
    sys.exit(asyncio.run(main(parse_args())))

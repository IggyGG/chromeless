#!/usr/bin/env python3
"""server-sink.py — record flash timestamps from index.html to JSONL.

The harness page (harness/latency/index.html) opens a WebSocket to
ws://localhost:9001 and emits one JSON record per flash transition:

    {"type": "flash", "frameId": 17, "state": "ON",
     "color": "#ff0000", "perfMs": 12345.678, "epochMs": 1714497123456,
     "runId": "run-1714497120000",
     "code": "v1|run-1714497120000|17|ON|1714497123456"}

We append each record verbatim (one record per line) to a JSONL file
named after the runId, alongside server-side receipt timestamps. The
reconciliation script (reconcile.py — T11) reads these files and joins
them with the webcam-captured frame timestamps.

Run:
    python3 server-sink.py [--host 0.0.0.0] [--port 9001] [--out-dir captures]

Dependencies:
    Python 3.8+ standard library only (asyncio, http.server-style ws
    via the third-party `websockets` package — listed in
    harness/latency/requirements.txt).
"""

from __future__ import annotations

import argparse
import asyncio
import datetime as dt
import json
import logging
import os
import signal
import sys
import time
from pathlib import Path

try:
    import websockets
    from websockets.server import WebSocketServerProtocol
except ImportError:
    sys.stderr.write(
        "ERROR: install dependencies first:\n"
        "    pip install -r harness/latency/requirements.txt\n"
    )
    sys.exit(2)


log = logging.getLogger("harness.sink")


def _open_log(out_dir: Path, run_id: str):
    """Open a JSONL file for the given run, creating parents as needed."""
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / f"{run_id}.jsonl"
    # Append-mode: re-running the harness with the same run_id continues
    # the file rather than truncating. The reconciler tolerates dupes.
    return path.open("a", buffering=1, encoding="utf-8"), path


async def handle(ws: "WebSocketServerProtocol", out_dir: Path) -> None:
    """One WebSocket connection. May span an entire harness run."""
    peer = f"{ws.remote_address[0]}:{ws.remote_address[1]}"
    log.info("client connected %s", peer)

    fp = None
    path = None
    run_id_seen: str | None = None
    n = 0

    try:
        async for msg in ws:
            # Records are JSON; we tolerate non-JSON noise but log it.
            try:
                rec = json.loads(msg)
            except json.JSONDecodeError:
                log.warning("dropping non-JSON message from %s: %r", peer, msg[:80])
                continue

            run_id = rec.get("runId") or "unknown-run"

            # Lazy-open: the first record's runId chooses the output file.
            if fp is None:
                fp, path = _open_log(out_dir, run_id)
                run_id_seen = run_id
                log.info("→ writing to %s", path)
            elif run_id != run_id_seen:
                # Run id changed mid-connection (page reload with new
                # runId); rotate the file.
                fp.close()
                fp, path = _open_log(out_dir, run_id)
                run_id_seen = run_id
                log.info("→ rotated to %s", path)

            # Stamp our own receipt time so reconciliation can detect
            # network skew between the harness page and the sink.
            rec["sinkRecvEpochMs"] = int(time.time() * 1000)
            rec["sinkRecvIso"] = dt.datetime.utcnow().isoformat() + "Z"

            fp.write(json.dumps(rec, separators=(",", ":")) + "\n")
            n += 1

            if n % 50 == 0:
                log.info("recorded %d records (run=%s)", n, run_id_seen)
    except websockets.ConnectionClosedOK:
        pass
    except websockets.ConnectionClosedError as e:
        log.warning("connection from %s closed with error: %s", peer, e)
    finally:
        if fp is not None:
            fp.close()
        log.info("client disconnected %s, recorded %d records", peer, n)


async def main_async(host: str, port: int, out_dir: Path) -> None:
    log.info("listening on ws://%s:%d  out=%s", host, port, out_dir)

    async def handler(ws):
        await handle(ws, out_dir)

    stop = asyncio.Event()

    def _stop(*_):
        stop.set()

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            # Windows or restricted env — fall back to KeyboardInterrupt.
            pass

    async with websockets.serve(handler, host, port, max_size=2**20):
        await stop.wait()
        log.info("shutting down")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="Latency harness WebSocket sink.")
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=9001)
    p.add_argument(
        "--out-dir",
        default=os.environ.get("HARNESS_OUT_DIR", "harness/captures"),
        help="Where to write JSONL files (default: harness/captures).",
    )
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args(argv)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    out_dir = Path(args.out_dir).resolve()
    try:
        asyncio.run(main_async(args.host, args.port, out_dir))
    except KeyboardInterrupt:
        log.info("interrupted")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

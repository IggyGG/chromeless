#!/usr/bin/env python3
"""Serve the streamer page and a same-origin CDP discovery endpoint."""

from __future__ import annotations

import argparse
import http.server
import json
import pathlib
import socketserver
import urllib.error
import urllib.parse
import urllib.request


class StreamerHandler(http.server.SimpleHTTPRequestHandler):
    cdp_url = "http://127.0.0.1:9222"

    def end_headers(self) -> None:
        self.send_header("Access-Control-Allow-Origin", "*")
        super().end_headers()

    def do_OPTIONS(self) -> None:
        self.send_response(204)
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self) -> None:
        if urllib.parse.urlsplit(self.path).path == "/cdp/json/version":
            self.proxy_cdp_version()
            return
        super().do_GET()

    def proxy_cdp_version(self) -> None:
        url = self.cdp_url.rstrip("/") + "/json/version"
        try:
            with urllib.request.urlopen(url, timeout=2.0) as resp:
                body = resp.read()
        except urllib.error.URLError as exc:
            payload = json.dumps({"error": f"cdp unavailable: {exc}"}).encode()
            self.send_response(502)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class ReusableTCPServer(socketserver.TCPServer):
    allow_reuse_address = True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--directory", default="/opt/cloud-browser")
    parser.add_argument("--cdp-url", default="http://127.0.0.1:9222")
    args = parser.parse_args()

    directory = pathlib.Path(args.directory).resolve()

    class Handler(StreamerHandler):
        cdp_url = args.cdp_url

        def __init__(self, *handler_args, **handler_kwargs):
            super().__init__(
                *handler_args,
                directory=str(directory),
                **handler_kwargs,
            )

    with ReusableTCPServer((args.bind, args.port), Handler) as httpd:
        print(
            f"[streamer-static] serving {directory} on "
            f"{args.bind}:{args.port}; cdp={args.cdp_url}",
            flush=True,
        )
        httpd.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

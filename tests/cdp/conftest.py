"""Pytest fixtures for the CDP smoke suite.

Mirrors the entry-point convention from `tests/smoke/container-boot.sh`:
discover the WebSocket debugger URL via `GET /json/version`, then rewrite
the `ws://localhost:9222` host portion to whatever cluster-reachable address
the caller passed in via `CB_URL`.

Usage outside pytest:
    CB_URL=http://localhost:9222 pytest tests/cdp/

Inside the cluster Job:
    CB_URL=http://cb-browserless.triform-wtf.svc.cluster.local:9222

The `Host: localhost` header on the discovery request avoids chromium's
DNS-rebinding rejection: the embedder pins remote-debugging to listen with
the localhost host check enabled, so the JSON discovery endpoint will
return 403 without the override. Once we have the rewritten ws:// URL,
the WebSocket upgrade itself is host-agnostic.
"""
from __future__ import annotations

import os
from urllib.parse import urlparse

import pytest
import requests


def _rewrite_host(ws_url: str, cb_url: str) -> str:
    """Replace the host portion of `ws_url` with the host:port from `cb_url`.

    Chromium's `/json/version` returns one of two shapes for
    `webSocketDebuggerUrl`, depending on the version and bind address:
      - `ws://localhost/devtools/browser/<uuid>`           (no port)
      - `ws://localhost:9222/devtools/browser/<uuid>`      (with port)

    Both are rewritten to the cluster-reachable host:port pair.
    """
    parsed_target = urlparse(cb_url)
    host = parsed_target.hostname or "localhost"
    port = parsed_target.port or 9222
    target = f"ws://{host}:{port}"
    parsed_ws = urlparse(ws_url)
    # urlunparse gets pedantic; simpler to splice the path back on.
    return f"{target}{parsed_ws.path}" + (
        f"?{parsed_ws.query}" if parsed_ws.query else ""
    )


def discover_ws_url(cb_url: str, timeout: float = 5.0) -> str:
    """Resolve the cluster-reachable WebSocket debugger URL.

    GETs `<cb_url>/json/version` with `Host: localhost` to bypass chromium's
    DNS-rebinding host check, then rewrites the returned ws:// URL so it's
    reachable from the test pod.
    """
    resp = requests.get(
        f"{cb_url.rstrip('/')}/json/version",
        headers={"Host": "localhost"},
        timeout=timeout,
    )
    resp.raise_for_status()
    payload = resp.json()
    raw_ws = payload.get("webSocketDebuggerUrl")
    if not raw_ws:
        raise RuntimeError(
            f"/json/version returned no webSocketDebuggerUrl: {payload!r}"
        )
    return _rewrite_host(raw_ws, cb_url)


@pytest.fixture(scope="session")
def cb_url() -> str:
    """Base HTTP URL of the cb-browserless DevTools endpoint."""
    return os.environ.get(
        "CB_URL",
        "http://cb-browserless.triform-wtf.svc.cluster.local:9222",
    )


@pytest.fixture(scope="session")
def ws_url(cb_url: str) -> str:
    """Resolved WebSocket browser-target URL."""
    return discover_ws_url(cb_url)

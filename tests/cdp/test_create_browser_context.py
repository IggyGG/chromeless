"""CDP smoke against the deployed cb-chromium binary.

Exercises the four CDP operations that physics actually issues against the
embedder. Each operation is its own pytest function so failures localize
cleanly: when Step 10 of cb-build.sh prints which test failed, we know
which embedder override regressed without re-reading the log.

The four ops, in dependency order:
  1. `Target.createBrowserContext` — proves the embedder's
     CreateBrowserContext override is hooked in. Stock chromium (without
     our patch) returns "Not implemented" here.
  2. `Target.createTarget` (in the new context) — proves CreateNewTarget
     accepts a non-default browserContextId. Regressing the override
     surfaces here as "Failed to find browser context".
  3. `Target.attachToTarget` (flatten=True) — proves the protocol session
     plumbs through.
  4. `Page.navigate` — proves the dispatch to the renderer process and
     the navigation pipeline work end-to-end.

The four tests run sequentially in this file (pytest preserves declaration
order within a module). Each acquires its own browser context so a failure
in test 3 doesn't leak state into test 4 — but they share the same
WebSocket connection within the function via the helper.

Failure output: each assertion prints both the operation name and the
verbatim CDP error envelope so a CI failure log immediately shows whether
it was a protocol error, a renderer crash, or a transport timeout.
"""
from __future__ import annotations

import asyncio
import json

import pytest
import websockets


CDP_RECV_TIMEOUT_S = 10.0
NAV_RECV_TIMEOUT_S = 30.0
# Page.captureScreenshot can produce multi-MB frames; we don't take any
# here but match container-boot.sh's 100 MiB cap so the ceiling stays
# consistent across the suite.
WS_MAX_SIZE = 100 * 1024 * 1024


def _open_ws(ws_url: str):
    """Return the `websockets.connect()` async-context-manager.

    Callers do `async with _open_ws(ws_url) as ws:`. We deliberately
    don't `await` the connect call here — `websockets.connect()` returns
    an awaitable AND an async context manager; the `async with` form is
    what we want for a clean shutdown on test exit.
    """
    return websockets.connect(ws_url, max_size=WS_MAX_SIZE)


async def _send_and_wait(ws, msg: dict) -> dict:
    """Send a CDP command, drain unrelated events, return the matching reply."""
    await ws.send(json.dumps(msg))
    while True:
        raw = await asyncio.wait_for(ws.recv(), timeout=CDP_RECV_TIMEOUT_S)
        payload = json.loads(raw)
        if payload.get("id") == msg["id"]:
            return payload


def _assert_no_cdp_error(op: str, response: dict) -> None:
    if "error" in response:
        pytest.fail(
            f"{op} returned CDP error: {json.dumps(response['error'], indent=2)}\n"
            f"full response: {json.dumps(response, indent=2)}"
        )


@pytest.mark.asyncio
async def test_target_create_browser_context(ws_url: str) -> None:
    """`Target.createBrowserContext` returns a browserContextId."""
    async with _open_ws(ws_url) as ws:
        response = await _send_and_wait(
            ws,
            {"id": 1, "method": "Target.createBrowserContext", "params": {}},
        )
        _assert_no_cdp_error("Target.createBrowserContext", response)
        ctx_id = response.get("result", {}).get("browserContextId")
        assert ctx_id, (
            "Target.createBrowserContext returned no browserContextId: "
            f"{json.dumps(response, indent=2)}"
        )
        # NOTE: we intentionally do NOT call Target.disposeBrowserContext
        # here. The plan flags it as a known-broken path: it crashes the
        # cb-chromium binary, which then returns ConnectionRefused for the
        # remaining tests in this module. Leaking contexts across tests is
        # cheap — the validation Job pod runs once per build and is GC'd
        # immediately. When the dispose crash is fixed in a follow-up, the
        # cleanup can be reinstated module-wide via a fixture.


@pytest.mark.asyncio
async def test_target_create_target_in_new_context(ws_url: str) -> None:
    """`Target.createTarget` succeeds against a freshly-created context."""
    async with _open_ws(ws_url) as ws:
        ctx_resp = await _send_and_wait(
            ws,
            {"id": 1, "method": "Target.createBrowserContext", "params": {}},
        )
        _assert_no_cdp_error("Target.createBrowserContext", ctx_resp)
        ctx_id = ctx_resp["result"]["browserContextId"]

        target_resp = await _send_and_wait(
            ws,
            {
                "id": 2,
                "method": "Target.createTarget",
                "params": {"url": "about:blank", "browserContextId": ctx_id},
            },
        )
        _assert_no_cdp_error("Target.createTarget", target_resp)
        target_id = target_resp.get("result", {}).get("targetId")
        assert target_id, (
            "Target.createTarget returned no targetId: "
            f"{json.dumps(target_resp, indent=2)}"
        )
        # See `test_target_create_browser_context` for why dispose is skipped.


@pytest.mark.asyncio
async def test_target_attach_to_target(ws_url: str) -> None:
    """`Target.attachToTarget` returns a sessionId for a fresh page."""
    async with _open_ws(ws_url) as ws:
        ctx_resp = await _send_and_wait(
            ws,
            {"id": 1, "method": "Target.createBrowserContext", "params": {}},
        )
        _assert_no_cdp_error("Target.createBrowserContext", ctx_resp)
        ctx_id = ctx_resp["result"]["browserContextId"]

        target_resp = await _send_and_wait(
            ws,
            {
                "id": 2,
                "method": "Target.createTarget",
                "params": {"url": "about:blank", "browserContextId": ctx_id},
            },
        )
        _assert_no_cdp_error("Target.createTarget", target_resp)
        target_id = target_resp["result"]["targetId"]

        attach_resp = await _send_and_wait(
            ws,
            {
                "id": 3,
                "method": "Target.attachToTarget",
                "params": {"targetId": target_id, "flatten": True},
            },
        )
        _assert_no_cdp_error("Target.attachToTarget", attach_resp)
        sess_id = attach_resp.get("result", {}).get("sessionId")
        assert sess_id, (
            "Target.attachToTarget returned no sessionId: "
            f"{json.dumps(attach_resp, indent=2)}"
        )
        # See `test_target_create_browser_context` for why dispose is skipped.


@pytest.mark.asyncio
async def test_page_navigate(ws_url: str) -> None:
    """End-to-end: createBrowserContext + createTarget + attach + Page.navigate.

    Drains the receive loop until either the navigation response (id=4)
    or the recv timeout. Failure modes:
      - Page.navigate returns an error envelope (e.g. cert error,
        DNS failure, embedder regressing the target dispatch).
      - No response within `NAV_RECV_TIMEOUT_S` (renderer crash, hung
        WebSocket, embedder dropping the message silently).
    """
    async with _open_ws(ws_url) as ws:
        ctx_resp = await _send_and_wait(
            ws,
            {"id": 1, "method": "Target.createBrowserContext", "params": {}},
        )
        _assert_no_cdp_error("Target.createBrowserContext", ctx_resp)
        ctx_id = ctx_resp["result"]["browserContextId"]

        target_resp = await _send_and_wait(
            ws,
            {
                "id": 2,
                "method": "Target.createTarget",
                "params": {"url": "about:blank", "browserContextId": ctx_id},
            },
        )
        _assert_no_cdp_error("Target.createTarget", target_resp)
        target_id = target_resp["result"]["targetId"]

        attach_resp = await _send_and_wait(
            ws,
            {
                "id": 3,
                "method": "Target.attachToTarget",
                "params": {"targetId": target_id, "flatten": True},
            },
        )
        _assert_no_cdp_error("Target.attachToTarget", attach_resp)
        sess_id = attach_resp["result"]["sessionId"]

        # Page.navigate is dispatched on the per-target session, so it
        # carries `sessionId`. The reply also carries the same sessionId.
        await ws.send(
            json.dumps(
                {
                    "id": 4,
                    "method": "Page.navigate",
                    "params": {"url": "https://example.com"},
                    "sessionId": sess_id,
                }
            )
        )

        nav_response: dict | None = None
        deadline = asyncio.get_event_loop().time() + NAV_RECV_TIMEOUT_S
        while asyncio.get_event_loop().time() < deadline:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=2.0)
            except asyncio.TimeoutError:
                continue
            payload = json.loads(raw)
            if payload.get("id") == 4:
                nav_response = payload
                break

        assert nav_response is not None, (
            f"Page.navigate did not reply within {NAV_RECV_TIMEOUT_S}s"
        )
        _assert_no_cdp_error("Page.navigate", nav_response)
        # `frameId` is the canonical success indicator on Page.navigate.
        assert nav_response.get("result", {}).get("frameId"), (
            "Page.navigate returned no frameId: "
            f"{json.dumps(nav_response, indent=2)}"
        )
        # See `test_target_create_browser_context` for why dispose is skipped.

#!/usr/bin/env python3
"""Boot-and-paint probe for the chromeless worker image.

The k8s twin of tests/smoke/container-boot.sh. It runs as a sidecar sharing the
worker's network namespace, so 127.0.0.1:9222 IS the worker's DevTools —
Chromium 147 binds loopback only, ignoring --remote-debugging-address, which is
why neither this nor the shell version can use a published port.

Deliberately stdlib-only (no websockets, no requests): the pod runs a stock
python image with no install step, and a pip install is one more thing that can
fail for reasons unrelated to the browser.

Steps, in the order a failure is most informative:
  1. Poll /json/version until DevTools answers.       → Chromium started at all
  2. Assert the payload names a Browser.              → it is the right binary
  3. Find a page target.                              → a WebContents exists
  4. Page.navigate to a real URL.                     → the renderer works
  5. Poll document.readyState == complete.            → the page actually loaded
  6. Page.captureScreenshot.                          → the compositor produces pixels
  7. Assert PNG magic + size > 5 KiB.                 → those pixels are real

Exit 0 = the image is good. Any non-zero exit means stop and fix the image
before blaming the standalone stack.
"""
import base64
import json
import os
import socket
import struct
import sys
import time
import urllib.request

DEVTOOLS_PORT = 9222
DEVTOOLS = f"http://127.0.0.1:{DEVTOOLS_PORT}"
NAV_URL = os.environ.get("SMOKE_NAVIGATE_URL", "https://example.com")
READY_TIMEOUT_S = int(os.environ.get("SMOKE_READY_TIMEOUT_S", "120"))
LOAD_TIMEOUT_S = int(os.environ.get("SMOKE_LOAD_TIMEOUT_S", "45"))
MIN_PNG_BYTES = 5 * 1024
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def log(msg):
    print(f"[smoke] {msg}", flush=True)


def die(step, msg):
    print(f"[smoke] FAIL at step {step}: {msg}", file=sys.stderr, flush=True)
    sys.exit(1)


def http_get(path, timeout=5):
    # Host: localhost defeats chromium's DNS-rebinding check. The embedder
    # enables it, and without this header /json answers 403 with a body that
    # does not explain why. Same trap the gateway hits (infra/gateway/cdp.go).
    req = urllib.request.Request(DEVTOOLS + path, headers={"Host": "localhost"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


# ---- 1. wait for DevTools -------------------------------------------------
log(f"waiting up to {READY_TIMEOUT_S}s for DevTools at {DEVTOOLS}")
deadline = time.time() + READY_TIMEOUT_S
version = None
while time.time() < deadline:
    try:
        version = http_get("/json/version")
        break
    except Exception:
        time.sleep(1)
if version is None:
    die(1, f"DevTools never answered within {READY_TIMEOUT_S}s — Chromium did not start")
log("DevTools answered")

# ---- 2. it is the right binary -------------------------------------------
# Key PRESENCE, not truthiness. This embedder answers with Browser="" and
# User-Agent="" — it is a content-shell-style embedder, not a Chrome build, so
# it never populates the product string. Everything else in the payload is
# real (Protocol-Version 1.3, a genuine V8-Version, a live browser debugger
# URL). tests/smoke/container-boot.sh:217 checks presence for exactly this
# reason; requiring a non-empty value fails a perfectly healthy worker.
if "Browser" not in version:
    die(2, f"/json/version has no Browser key at all: {version}")
log(f"V8 {version.get('V8-Version', '?')}, protocol {version.get('Protocol-Version', '?')}")

# ---- 3. find a page target ------------------------------------------------
# /json only. /json/protocol CHECK-FATALs this worker (see CLAUDE.md and the
# nine test files carrying `local: true` for the same reason).
targets = http_get("/json")
page = next((t for t in targets if t.get("type") == "page" and t.get("webSocketDebuggerUrl")), None)
if page is None:
    die(3, f"no page target among {[t.get('type') for t in targets]}")
ws_url = page["webSocketDebuggerUrl"]
log(f"page target: {page.get('url', '?')}")


# ---- minimal RFC 6455 client ---------------------------------------------
class WS:
    """Just enough WebSocket for request/response CDP over loopback."""

    def __init__(self, url):
        # ws://localhost/devtools/page/<id>  — note: OFTEN NO PORT.
        #
        # Chromium advertises the debugger URL with the host it bound and
        # frequently omits the port entirely. Parsing naively and defaulting to
        # 80 dials nothing (ECONNREFUSED) or, worse, something else listening
        # locally. Always force the DevTools port we actually reached — the
        # same rewrite infra/gateway/cdp.go does, and the same trap documented
        # in tests/cdp/conftest.py.
        rest = url.split("://", 1)[1]
        hostport, path = rest.split("/", 1)
        host = hostport.partition(":")[0]
        self.sock = socket.create_connection((host, DEVTOOLS_PORT), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(
            f"GET /{path} HTTP/1.1\r\nHost: {hostport}\r\n"
            f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode()
        )
        resp = self._read_until(b"\r\n\r\n")
        if b"101" not in resp.split(b"\r\n")[0]:
            raise RuntimeError(f"upgrade failed: {resp.split(chr(13).encode())[0]!r}")
        self.buf = b""

    def _read_until(self, marker):
        data = b""
        while marker not in data:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("socket closed during handshake")
            data += chunk
        return data

    def send(self, obj):
        payload = json.dumps(obj).encode()
        mask = os.urandom(4)
        n = len(payload)
        if n < 126:
            header = struct.pack("!BB", 0x81, 0x80 | n)
        elif n < (1 << 16):
            header = struct.pack("!BBH", 0x81, 0x80 | 126, n)
        else:
            header = struct.pack("!BBQ", 0x81, 0x80 | 127, n)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def _recv_exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("socket closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self):
        b0, b1 = struct.unpack("!BB", self._recv_exact(2))
        length = b1 & 0x7F
        if length == 126:
            length = struct.unpack("!H", self._recv_exact(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self._recv_exact(8))[0]
        return json.loads(self._recv_exact(length))

    def call(self, mid, method, params=None, timeout=30):
        self.send({"id": mid, "method": method, **({"params": params} if params else {})})
        self.sock.settimeout(timeout)
        while True:
            msg = self.recv()
            # The socket also carries events (Page.frameNavigated et al), which
            # have no id. Correlate on id or an event is mistaken for the reply.
            if msg.get("id") == mid:
                if "error" in msg:
                    raise RuntimeError(f"{method}: {msg['error']}")
                return msg.get("result", {})


ws = WS(ws_url)

# ---- 4. navigate ----------------------------------------------------------
log(f"Page.navigate {NAV_URL}")
ws.call(1, "Page.enable")
ws.call(2, "Page.navigate", {"url": NAV_URL})

# ---- 5. wait for the load ------------------------------------------------
log(f"waiting up to {LOAD_TIMEOUT_S}s for readyState=complete")
deadline = time.time() + LOAD_TIMEOUT_S
mid = 10
ready = False
while time.time() < deadline:
    mid += 1
    res = ws.call(mid, "Runtime.evaluate",
                  {"expression": "document.readyState", "returnByValue": True})
    if res.get("result", {}).get("value") == "complete":
        ready = True
        break
    time.sleep(1)
if not ready:
    die(5, f"document.readyState never reached complete within {LOAD_TIMEOUT_S}s")
log("page loaded")

# ---- 6/7. screenshot ------------------------------------------------------
log("Page.captureScreenshot")
shot = ws.call(99, "Page.captureScreenshot", {"format": "png"})
png = base64.b64decode(shot["data"])
if not png.startswith(PNG_MAGIC):
    die(7, f"not a PNG: first bytes {png[:8]!r}")
if len(png) < MIN_PNG_BYTES:
    # A blank or 0x0 surface still encodes as a valid PNG, just a tiny one —
    # so size is what separates "the compositor produced pixels" from "it
    # produced a valid empty image".
    die(7, f"PNG is only {len(png)} bytes (< {MIN_PNG_BYTES}) — the page rendered blank")

log(f"PNG OK: {len(png)} bytes")
log("PASS — the worker image boots, loads a real page, and paints.")

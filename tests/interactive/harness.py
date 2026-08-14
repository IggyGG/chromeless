#!/usr/bin/env python3
"""Interactive end-to-end tests for a live chromeless deployment.

WHAT MAKES THIS DIFFERENT FROM THE PLAYWRIGHT SPECS: those assert on the
CLIENT's own view — "the data channel says open", "getStats reports frames".
That proves the plumbing is connected. It does NOT prove that clicking
actually clicks: the client would report a perfectly healthy input channel
while every event vanished inside the browser process.

So every assertion here uses an INDEPENDENT ORACLE. Input is dispatched as
genuine DOM events on the client's <video> element (which is what
client/src/input.ts listens to — mousemove/mousedown/mouseup/wheel on the
element, keydown/keyup on the window), and the result is read back through the
WORKER's own DevTools: document.activeElement, window.scrollY, the value of the
input the keystrokes landed in. Two independent paths have to agree.

Structure:
  ClientDriver  — real Google Chrome, driven over CDP, running the actual
                  client bundle served by the gateway. It is the user.
  WorkerOracle  — the worker's DevTools, port-forwarded. It is the truth.

ONE SESSION PER WORKER PROCESS. After a `bye` the worker will not start
another, so the whole suite runs inside a single connect, and a failure that
tears down the session ends the run rather than silently testing nothing.

Usage:
    python3 tests/interactive/harness.py            # all suites
    python3 tests/interactive/harness.py --only mouse,keyboard
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import socket
import struct
import subprocess
import sys
import time
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
GATEWAY = os.environ.get("CHROMELESS_GATEWAY", "https://localhost:8443")
WORKER_CDP = os.environ.get("CHROMELESS_WORKER_CDP", "http://127.0.0.1:19222")
CHROME = os.environ.get(
    "CHROMELESS_TEST_CHROME",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")
CLIENT_CDP_PORT = int(os.environ.get("CHROMELESS_CLIENT_CDP_PORT", "9444"))


# --------------------------------------------------------------------------
# A minimal CDP client. stdlib only: this runs against whatever python is on
# the machine, and a pip install is one more thing that can fail for reasons
# unrelated to the browser.
# --------------------------------------------------------------------------
class CDP:
    def __init__(self, ws_url: str, force_port: int | None = None):
        rest = ws_url.split("://", 1)[1]
        hostport, path = rest.split("/", 1)
        host = hostport.partition(":")[0]
        # Chromium advertises the debugger URL with the host it BOUND and
        # frequently omits the port. Dialing it verbatim reaches nothing, or
        # something else listening locally — the same trap
        # infra/gateway/cdp.go and tests/cdp/conftest.py both handle.
        port = force_port if force_port else int(hostport.partition(":")[2] or 80)
        self.sock = socket.create_connection((host, port), timeout=15)
        self.sock.settimeout(60)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(
            f"GET /{path} HTTP/1.1\r\nHost: {hostport}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n\r\n".encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += self.sock.recv(4096)
        self.buf = b""
        self._id = 0

    def _send(self, obj):
        payload = json.dumps(obj).encode()
        mask = os.urandom(4)
        n = len(payload)
        if n < 126:
            header = struct.pack("!BB", 0x81, 0x80 | n)
        elif n < (1 << 16):
            header = struct.pack("!BBH", 0x81, 0x80 | 126, n)
        else:
            header = struct.pack("!BBQ", 0x81, 0x80 | 127, n)
        self.sock.sendall(
            header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def _exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("cdp socket closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _recv(self):
        b0, b1 = struct.unpack("!BB", self._exact(2))
        ln = b1 & 0x7F
        if ln == 126:
            ln = struct.unpack("!H", self._exact(2))[0]
        elif ln == 127:
            ln = struct.unpack("!Q", self._exact(8))[0]
        return json.loads(self._exact(ln))

    def call(self, method, params=None, timeout=60):
        self._id += 1
        mid = self._id
        self._send({"id": mid, "method": method,
                    **({"params": params} if params else {})})
        end = time.time() + timeout
        while time.time() < end:
            msg = self._recv()
            # The socket also carries events, which have no id. Correlating on
            # id is what keeps an event from being mistaken for the reply.
            if msg.get("id") == mid:
                if "error" in msg:
                    raise RuntimeError(f"{method}: {msg['error']}")
                return msg.get("result", {})
        raise RuntimeError(f"{method}: timed out")

    def eval(self, expr, timeout=60, await_promise=True):
        r = self.call("Runtime.evaluate",
                      {"expression": expr, "returnByValue": True,
                       "awaitPromise": await_promise}, timeout=timeout)
        res = r.get("result", {})
        if r.get("exceptionDetails"):
            raise RuntimeError(f"eval threw: {r['exceptionDetails'].get('text')}")
        return res.get("value")


def http_json(url, host_header=None, timeout=10):
    # Chromium's DNS-rebinding check answers 403 to any Host but localhost,
    # with a body that does not explain why.
    #
    # Set the HEADER, not Request.host: assigning req.host changes where urllib
    # CONNECTS (it would dial localhost:80 and get ECONNREFUSED), not what it
    # puts in the request line.
    headers = {"Host": host_header} if host_header else {}
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


# --------------------------------------------------------------------------
# The oracle: the worker's own DevTools.
# --------------------------------------------------------------------------
class NoFrames(RuntimeError):
    """No video reached the client. Its own message explains the usual cause."""


class WorkerOracle:
    def __init__(self, base=WORKER_CDP):
        self.base = base.rstrip("/")
        targets = http_json(f"{self.base}/json", host_header="localhost")
        page = next(t for t in targets
                    if t["type"] == "page" and t.get("webSocketDebuggerUrl"))
        port = int(self.base.rsplit(":", 1)[1])
        self.cdp = CDP(page["webSocketDebuggerUrl"], force_port=port)
        self.cdp.call("Page.enable")
        self.cdp.call("Runtime.enable")

    def eval(self, expr, **kw):
        return self.cdp.eval(expr, **kw)

    def url(self):
        return self.eval("location.href")

    def rect(self, element_id):
        """Centre of an element, as REMOTE viewport fractions (0..1).

        Fractions, not pixels: the client sends normalised coordinates so the
        same gesture works at any window size, and hand-converting at each call
        site is how a test ends up clicking somewhere plausible but wrong.
        """
        return json.loads(self.eval(
            "(() => { const r = document.getElementById('%s')"
            ".getBoundingClientRect();"
            " return JSON.stringify({x:(r.left+r.width/2)/innerWidth,"
            "                        y:(r.top+r.height/2)/innerHeight}); })()"
            % element_id))

    def wait_for(self, expr, want, timeout=20, poll=0.4):
        """Poll an expression on the REMOTE page until it matches."""
        end = time.time() + timeout
        last = None
        while time.time() < end:
            last = self.eval(expr)
            if last == want if not callable(want) else want(last):
                return True, last
            time.sleep(poll)
        return False, last


# --------------------------------------------------------------------------
# The user: real Chrome running the real client bundle.
# --------------------------------------------------------------------------
class ClientDriver:
    def __init__(self, user, password):
        self.profile = "/tmp/chromeless-itest-profile"
        subprocess.run(["rm", "-rf", self.profile], check=False)
        # Reap anything a previous run left behind BEFORE starting a new one.
        # Chrome outlives an interrupted harness, and each survivor holds the
        # session's single `client` slot on the broker — so the next run gets
        # no video and looks like a product failure. 57 of them had piled up
        # before this was noticed.
        self._reap_strays()
        self.proc = subprocess.Popen(
            [CHROME, f"--remote-debugging-port={CLIENT_CDP_PORT}",
             f"--user-data-dir={self.profile}", "--headless=new",
             "--no-first-run", "--no-default-browser-check",
             "--ignore-certificate-errors",     # the gateway's cert is self-signed
             "--autoplay-policy=no-user-gesture-required",
             "--window-size=1400,900", "about:blank"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 30
        while True:
            try:
                http_json(f"http://127.0.0.1:{CLIENT_CDP_PORT}/json/version", timeout=2)
                break
            except Exception:
                if time.time() > deadline:
                    raise SystemExit("test chrome never came up")
                time.sleep(0.5)
        page = next(t for t in http_json(f"http://127.0.0.1:{CLIENT_CDP_PORT}/json")
                    if t["type"] == "page")
        self.cdp = CDP(page["webSocketDebuggerUrl"], force_port=CLIENT_CDP_PORT)
        self.cdp.call("Page.enable")
        self.cdp.call("Runtime.enable")
        self.user, self.password = user, password

    @staticmethod
    def _reap_strays():
        """Kill every chrome started by this harness, from any run.

        Matched on the profile path, which is unique to this harness — so an
        unrelated chrome the user has open is never touched.
        """
        subprocess.run(["pkill", "-9", "-f", "chromeless-itest-profile"],
                       check=False, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        time.sleep(0.5)

    def close(self):
        # terminate() alone leaves chrome's CHILD processes running: the
        # launcher exits, the renderers and the network service do not, and
        # they keep the WebSocket to the broker open. Kill the whole family.
        try:
            self.proc.terminate()
            self.proc.wait(timeout=5)
        except Exception:
            pass
        self._reap_strays()

    def login_and_connect(self, timeout=90):
        # Log in on a page that does NOT auto-connect.
        #
        # /login is safe; /?e2e=1 is not. Loading the client bundle twice makes
        # TWO signaling sessions, and the second one is poisoned: the broker
        # replays the FIRST session's buffered offer and ICE to the reconnecting
        # client ("replayed: 10"), so it answers a dead offer and its relay
        # candidates pair against candidates that no longer exist. The symptom
        # is ice=checking with a single host/tcp pair and req=0 — no
        # connectivity check is ever attempted — which reads exactly like a NAT
        # or TURN failure and is neither.
        #
        # So: authenticate here, and load the client exactly once, below.
        self.cdp.call("Page.navigate", {"url": f"{GATEWAY}/login"})
        time.sleep(2)
        self.cdp.eval(f"""(async () => {{
            const b = new URLSearchParams({{username:{json.dumps(self.user)},
                                            password:{json.dumps(self.password)}}});
            const r = await fetch('/login', {{method:'POST', body:b,
                                              credentials:'same-origin'}});
            return r.status; }})()""")
        # ?e2e=1 exposes window.__cbwrtc_pc (client/main.ts maybeExposePcForE2e).
        self.cdp.call("Page.navigate", {"url": f"{GATEWAY}/?e2e=1"})
        time.sleep(3)
        self.cdp.eval("document.getElementById('connect').click(); 1")

        end = time.time() + timeout
        while time.time() < end:
            frames = self.cdp.eval("""(async () => {
                const pc = window.__cbwrtc_pc; if (!pc) return -1;
                const s = await pc.getStats(); let f = 0;
                s.forEach(r => { if (r.type === 'inbound-rtp' && r.kind === 'video')
                                   f = Math.max(f, r.framesDecoded || 0); });
                return f; })()""")
            if isinstance(frames, (int, float)) and frames > 0:
                return frames
            time.sleep(2)
        raise NoFrames(
            "client never decoded a frame. Two causes account for almost every "
            "occurrence, and NEITHER is a bug in what you are testing:\n"
            "  1. The worker already served a session. It serves exactly ONE "
            "per process (docs/findings/one-session-per-worker-process.md), so "
            "a second run against the same pod always lands here. Fix:\n"
            "       kubectl rollout restart "
            "deploy/chromeless-standalone-worker -n chromeless\n"
            "  2. A stray test chrome from an interrupted run still holds the "
            "session's single `client` slot. This harness now reaps those at "
            "startup, so it should not recur — verify with:\n"
            "       pgrep -fl chromeless-itest-profile")

    # ---- input: genuine DOM events on the video element ----
    #
    # NOT synthetic CDP Input.dispatch* on the client — that would bypass
    # client/src/input.ts entirely and test nothing. These are the events the
    # real listeners are bound to (input.ts:709-715).

    def _video_point(self, fx, fy):
        """Fractional page coords -> client coords over the video element."""
        return self.cdp.eval(f"""(() => {{
            const v = document.getElementById('remote');
            const r = v.getBoundingClientRect();
            return JSON.stringify({{
                x: r.left + r.width  * {fx},
                y: r.top  + r.height * {fy},
                w: r.width, h: r.height,
                vw: v.videoWidth, vh: v.videoHeight }}); }})()""")

    def mouse_move(self, fx, fy):
        p = json.loads(self._video_point(fx, fy))
        self.cdp.eval(f"""(() => {{
            const v = document.getElementById('remote');
            v.dispatchEvent(new MouseEvent('mousemove', {{
                clientX: {p['x']}, clientY: {p['y']}, bubbles: true }}));
            return 1; }})()""")
        return p

    def click(self, fx, fy, button=0):
        p = json.loads(self._video_point(fx, fy))
        self.cdp.eval(f"""(() => {{
            const v = document.getElementById('remote');
            const o = {{ clientX: {p['x']}, clientY: {p['y']},
                         button: {button}, bubbles: true }};
            v.dispatchEvent(new MouseEvent('mousemove', o));
            v.dispatchEvent(new MouseEvent('mousedown', o));
            v.dispatchEvent(new MouseEvent('mouseup', o));
            return 1; }})()""")
        return p

    def wheel(self, fx, fy, dy, dx=0):
        p = json.loads(self._video_point(fx, fy))
        self.cdp.eval(f"""(() => {{
            const v = document.getElementById('remote');
            v.dispatchEvent(new WheelEvent('wheel', {{
                clientX: {p['x']}, clientY: {p['y']},
                deltaX: {dx}, deltaY: {dy}, deltaMode: 0,
                bubbles: true, cancelable: true }}));
            return 1; }})()""")

    def key(self, code, key, mods=0, hold_ms=12):
        """One keystroke. mods is the DOM-side modifier set."""
        alt = "true" if mods & 1 else "false"
        ctrl = "true" if mods & 2 else "false"
        meta = "true" if mods & 4 else "false"
        shift = "true" if mods & 8 else "false"
        self.cdp.eval(f"""(() => {{
            const o = {{ code: {json.dumps(code)}, key: {json.dumps(key)},
                         altKey: {alt}, ctrlKey: {ctrl},
                         metaKey: {meta}, shiftKey: {shift}, bubbles: true }};
            window.dispatchEvent(new KeyboardEvent('keydown', o));
            return 1; }})()""")
        time.sleep(hold_ms / 1000)
        self.cdp.eval(f"""(() => {{
            const o = {{ code: {json.dumps(code)}, key: {json.dumps(key)},
                         altKey: {alt}, ctrlKey: {ctrl},
                         metaKey: {meta}, shiftKey: {shift}, bubbles: true }};
            window.dispatchEvent(new KeyboardEvent('keyup', o));
            return 1; }})()""")

    def type(self, text, per_key_ms=35):
        for ch in text:
            code = (f"Key{ch.upper()}" if ch.isalpha()
                    else f"Digit{ch}" if ch.isdigit()
                    else "Space" if ch == " " else "")
            self.key(code, ch)
            time.sleep(per_key_ms / 1000)

    def cursor_shape(self):
        """What the cursor channel last told the client to render.

        Reads `[data-role="cursor-overlay"]`'s data-shape, which is what
        client/src/cursor.ts actually sets. NOT getComputedStyle(...).cursor:
        the client draws an inline-SVG glyph rather than relying on the CSS
        cursor property (cursor.ts explains why — CSS `cursor` outside a real
        hover is unreliable). Reading the CSS property returned the page's
        default 'auto' no matter what the channel delivered, i.e. a check that
        could only ever fail.
        """
        return self.cdp.eval("""(() => {
            const el = document.querySelector('[data-role="cursor-overlay"]');
            return el && el.dataset ? (el.dataset.shape || null) : null; })()""")

    def channel_states(self):
        return self.cdp.eval(
            "document.getElementById('state-dc') ? "
            "document.getElementById('state-dc').textContent : null")

    def client_log(self, n=25):
        return (self.cdp.eval("document.getElementById('log').innerText") or
                "").splitlines()[-n:]


# --------------------------------------------------------------------------
def fixture_url(html: str) -> str:
    """Publish an HTML fixture where the WORKER can fetch it.

    Not a data: URL — the gateway's navigation allowlist permits only
    http/https, and that refusal is a feature worth testing through rather than
    around. Not a server on this laptop either: the worker is a cluster pod
    behind NAT and cannot dial back here.

    The gateway can serve it, and the worker can reach the gateway Service, so
    the fixture is POSTed through the authenticated TLS port and fetched by the
    worker over a separate PLAIN-HTTP listener. Plain HTTP because the worker
    is Chromium and validates certificates: pointed at the https:// fixture it
    fails the handshake with ERR_CERT_AUTHORITY_INVALID and renders an error
    page, which then reads as a mouse failure. Enabled only when the gateway
    runs with CHROMELESS_ENABLE_TEST_FIXTURE=1.
    """
    import ssl
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    req = urllib.request.Request(
        f"{GATEWAY}/api/test-fixture", data=html.encode(),
        headers={"Content-Type": "text/html", "Cookie": _COOKIE})
    with urllib.request.urlopen(req, timeout=20, context=ctx) as r:
        json.loads(r.read())
    # The worker fetches it by Service name, which is what it can resolve.
    return "http://chromeless-standalone-gateway:8081/api/test-fixture"


def _req(verb, timeout=45):
    """POST one of the gateway's parameterless nav verbs (back/forward/reload)."""
    import ssl
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    req = urllib.request.Request(f"{GATEWAY}/api/{verb}", data=b"",
                                 headers={"Cookie": _COOKIE})
    with urllib.request.urlopen(req, timeout=timeout, context=ctx) as r:
        return json.loads(r.read())


def navigate(url, timeout=45):
    """Drive the gateway's navigation endpoint, as the address bar does."""
    body = json.dumps({"url": url}).encode()
    req = urllib.request.Request(f"{GATEWAY}/api/navigate", data=body,
                                 headers={"Content-Type": "application/json",
                                          "Cookie": _COOKIE})
    import ssl
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    with urllib.request.urlopen(req, timeout=timeout, context=ctx) as r:
        return json.loads(r.read())


_COOKIE = ""


def login_cookie(user, password):
    import ssl
    import http.cookiejar
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(
        urllib.request.HTTPSHandler(context=ctx),
        urllib.request.HTTPCookieProcessor(jar))
    data = urllib.parse.urlencode({"username": user, "password": password}).encode()
    try:
        opener.open(f"{GATEWAY}/login", data=data, timeout=15)
    except urllib.error.HTTPError:
        pass
    for c in jar:
        if c.name == "chromeless_session":
            return f"chromeless_session={c.value}"
    raise SystemExit("login failed — check the credentials file")

#!/usr/bin/env python3
"""Interactive suites against a live chromeless deployment.

Every check dispatches real DOM events at the client and verifies the outcome
through the WORKER's own DevTools — two independent paths that must agree.
A client that reports a healthy input channel while every event vanishes
inside the browser process fails here, which is the entire point.

    kubectl port-forward -n chromeless svc/chromeless-standalone-gateway 8443:8443 &
    kubectl port-forward -n chromeless deploy/chromeless-standalone-worker 19222:9222 &
    python3 tests/interactive/run.py
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.parse

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
import harness as H  # noqa: E402

# Data URLs, so the interaction tests do not depend on a third party being up,
# unblocked, or unchanged. The website VARIETY is covered separately below;
# these exist to test the mechanics precisely.
# Served over HTTP by a local server (see fixture_server below) rather than as
# a data: URL: the gateway's navigation allowlist permits only http/https, and
# rightly so — file:, data: and javascript: are exactly what it exists to
# refuse. Testing through the real allowlist is the point.
FORM_HTML = ("""
<html><head><title>form</title><style>
 body{font:16px sans-serif;margin:0;padding:24px}
 #target{width:520px;height:90px;font-size:20px}
 #box{width:40px;height:40px}
 .tall{height:2400px;background:linear-gradient(#fff,#ddd)}
 #hot{width:260px;height:120px;background:#cfe;cursor:pointer}
 #linky{cursor:pointer}
</style></head><body>
 <h1 id="h">interaction target</h1>
 <input id="target" placeholder="type here">
 <input type="checkbox" id="box">
 <textarea id="area" rows="3" cols="40"></textarea>
 <div id="hot">hover me (cursor:pointer)</div>
 <a href="#one" id="linky">a link</a>
 <div id="clicks">0</div>
 <div class="tall"></div>
 <script>
   window.__clicks = 0; window.__lastKey = ''; window.__wheeled = 0;
   document.getElementById('hot').addEventListener('click', () => {
     window.__clicks++; document.getElementById('clicks').textContent = window.__clicks; });
   window.addEventListener('keydown', e => { window.__lastKey = e.key; });
   window.addEventListener('wheel', () => { window.__wheeled++; }, {passive:true});
 </script></body></html>""")

# Real sites, for variety. Each is checked for "did it actually render" rather
# than for specific content, since third-party pages change.
SITES = [
    ("example.com",        "https://example.com/"),
    ("wikipedia (text)",   "https://en.m.wikipedia.org/wiki/WebRTC"),
    ("mdn (dense)",        "https://developer.mozilla.org/en-US/docs/Web/API/RTCPeerConnection"),
    ("duckduckgo (form)",  "https://lite.duckduckgo.com/lite/"),
    # A form-heavy page, for input targeting against real controls. Was
    # httpbin.org/forms/post until 2026-08-17, when httpbin began flapping
    # between 200 and 503 and cost two false failures — the suite now detects
    # that (see _site_reachable) but a stable third party is better than a
    # well-handled unstable one.
    ("w3schools (forms)",  "https://www.w3schools.com/html/html_forms.asp"),
    ("wikipedia main",     "https://en.m.wikipedia.org/wiki/Main_Page"),
]

results = []


def check(name, ok, detail="", pass_detail=""):
    """Record and print one check.

    `detail` is printed ONLY on failure. That is a deliberate change: it used
    to print unconditionally, so a detail phrased as a diagnosis ("the input
    path missed the target") appeared next to PASS and read as a
    contradiction — which is exactly the kind of noise that trains people to
    skim past the output. Pass `pass_detail` for something worth showing on
    success (a measured value, a count).
    """
    results.append((name, bool(ok), detail))
    shown = pass_detail if ok else detail
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {shown}" if shown else ""),
          flush=True)
    return ok


# ---------------------------------------------------------------- suites
def suite_video(client, worker):
    print("\n[video]")
    frames1 = client.cdp.eval("""(async () => { const pc=window.__cbwrtc_pc;
        const s=await pc.getStats(); let f=0; s.forEach(r=>{
          if(r.type==='inbound-rtp'&&r.kind==='video') f=Math.max(f,r.framesDecoded||0);});
        return f; })()""")
    time.sleep(5)
    frames2 = client.cdp.eval("""(async () => { const pc=window.__cbwrtc_pc;
        const s=await pc.getStats(); let f=0; s.forEach(r=>{
          if(r.type==='inbound-rtp'&&r.kind==='video') f=Math.max(f,r.framesDecoded||0);});
        return f; })()""")
    check("frames keep decoding", frames2 > frames1, f"{frames1} -> {frames2}")

    dims = client.cdp.eval("""(() => { const v=document.getElementById('remote');
        return v.videoWidth + 'x' + v.videoHeight; })()""")
    check("video has real dimensions", dims not in ("0x0", None), dims)

    # A frame that decodes but is a uniform colour is the "black video" failure.
    variance = client.cdp.eval("""(() => {
        const v=document.getElementById('remote');
        const c=document.createElement('canvas'); c.width=160; c.height=90;
        const x=c.getContext('2d'); x.drawImage(v,0,0,160,90);
        const d=x.getImageData(0,0,160,90).data;
        let min=255,max=0;
        for(let i=0;i<d.length;i+=4){const g=(d[i]+d[i+1]+d[i+2])/3;
          if(g<min)min=g; if(g>max)max=g;}
        return max-min; })()""")
    # about:blank IS a flat white frame, correctly. Navigate somewhere with
    # content first, or this measures the wrong thing.
    H.navigate("https://example.com/")
    time.sleep(6)
    variance2 = client.cdp.eval("""(() => {
        const v=document.getElementById('remote');
        const c=document.createElement('canvas'); c.width=160; c.height=90;
        const x=c.getContext('2d'); x.drawImage(v,0,0,160,90);
        const d=x.getImageData(0,0,160,90).data;
        let min=255,max=0;
        for(let i=0;i<d.length;i+=4){const g=(d[i]+d[i+1]+d[i+2])/3;
          if(g<min)min=g; if(g>max)max=g;}
        return max-min; })()""")
    check("frame carries real page content", (variance2 or 0) > 8,
          f"luma spread on about:blank={variance}, on example.com={variance2}")


def suite_navigation(client, worker):
    print("\n[navigation]")
    for label, url in SITES:
        try:
            H.navigate(url)
        except Exception as e:
            # A third-party outage is NOT a chromeless failure, and reporting it
            # as one is worse than useless — it turns a red suite into noise
            # that gets ignored. Observed 2026-08-17: httpbin.org returned 503
            # for every request, the worker hung waiting for it, the gateway's
            # CDP read deadline expired, and the NEXT site failed too as
            # collateral from the same stuck connection. Two red checks, zero
            # defects.
            #
            # So: ask the site directly, from here. If it is also unreachable
            # from this machine, the site is down and the check is skipped
            # loudly rather than failed silently-wrongly.
            reachable, detail = _site_reachable(url)
            if not reachable:
                print(f"  SKIP  navigate: {label}   third-party site "
                      f"unreachable from here too ({detail}) — not a "
                      f"chromeless failure")
                continue
            check(f"navigate: {label}", False,
                  f"gateway error {e} (but the site IS reachable from here: "
                  f"{detail})")
            continue
        # A redirect is not a failure: en.m.wikipedia.org -> en.wikipedia.org,
        # http -> https, and locale redirects are all normal. What matters is
        # that the browser left the previous page and rendered something.
        ok, got = worker.wait_for(
            "location.href",
            lambda v: v and v != "about:blank" and _same_site(v, url),
            timeout=35)
        if not ok:
            check(f"navigate: {label}", False, f"remote url={got!r}")
            continue
        # Rendered, not merely navigated. Text length alone is a bad oracle:
        # a search page is mostly one input, and lite.duckduckgo.com renders
        # ~17 characters of body text while being perfectly correct. Accept a
        # real <title> OR meaningful text OR interactive elements.
        _, evidence = worker.wait_for(
            """(() => {
                 const t = (document.title || '').trim().length;
                 const b = document.body ? document.body.innerText.trim().length : 0;
                 const e = document.querySelectorAll('input,button,a,form').length;
                 return t + ':' + b + ':' + e; })()""",
            lambda v: v and any(int(p) > 0 for p in str(v).split(":")[:1] + str(v).split(":")[1:]),
            timeout=25)
        t, b, e = (int(x) for x in str(evidence or "0:0:0").split(":"))
        rendered = t > 0 or b > 40 or e > 0
        check(f"navigate: {label}", rendered, f"title={t}c text={b}c elements={e}")

    # Back/forward drive REAL CDP history, not a client-side vector.
    before = worker.url()
    H._req("back")
    ok, after = worker.wait_for("location.href", lambda v: v != before, timeout=20)
    check("history: back changes the page", ok, f"{_short(before)} -> {_short(after)}")
    H._req("forward")
    ok2, fwd = worker.wait_for("location.href", lambda v: v != after, timeout=20)
    check("history: forward returns", ok2, _short(fwd))


def suite_mouse(client, worker):
    print("\n[mouse]")
    H.navigate(H.fixture_url(FORM_HTML))
    worker.wait_for("document.getElementById('hot') ? 1 : 0", 1, timeout=25)
    time.sleep(1.5)

    # Where is the clickable box, in REMOTE viewport fractions?
    box = worker.rect("hot")
    client.click(box["x"], box["y"])
    ok, clicks = worker.wait_for("window.__clicks || 0", lambda v: (v or 0) >= 1, timeout=15)
    check("click lands on the right element", ok, f"remote click count={clicks}")

    # Double-click via two clicks; the count must advance again.
    client.click(box["x"], box["y"])
    ok2, c2 = worker.wait_for("window.__clicks || 0", lambda v: (v or 0) >= 2, timeout=15)
    check("second click registers", ok2, f"count={c2}")

    # Hover fires mouseover on ENTERING an element. The two clicks above left
    # the pointer already inside #hot, so moving to the same coordinates
    # correctly fires nothing — an earlier version of this check did exactly
    # that and reported a product bug that did not exist. Move away first.
    worker.eval("window.__hovered=0; document.getElementById('hot')"
                ".addEventListener('mouseover',()=>{window.__hovered=1});1")
    h1 = worker.rect("h")  # the <h1>, well clear of #hot
    client.mouse_move(h1["x"], h1["y"])
    time.sleep(0.4)
    client.mouse_move(box["x"], box["y"])
    ok3, hov = worker.wait_for("window.__hovered || 0", 1, timeout=15)
    check("mouse move produces a remote hover", ok3, f"hovered={hov}")


def suite_scroll(client, worker):
    print("\n[scroll]")
    # The fixture must be present and scrollable. Re-navigating here rather
    # than trusting the previous suite means a worker restart between suites
    # (which is now NORMAL — the embedder exits when a viewer disconnects, see
    # docs/findings/one-session-per-worker-process.md) does not silently turn
    # this into a scroll test against about:blank, where scrollY is always 0
    # and both checks fail for a reason that has nothing to do with the wheel.
    H.navigate(H.fixture_url(FORM_HTML))
    worker.wait_for("!!document.querySelector('.tall')", True, timeout=20)
    worker.eval("window.scrollTo(0,0); 1")
    time.sleep(0.6)
    before = worker.eval("Math.round(window.scrollY)")
    for _ in range(4):
        client.wheel(0.5, 0.5, 320)
        time.sleep(0.35)
    ok, after = worker.wait_for("Math.round(window.scrollY)",
                                lambda v: (v or 0) > (before or 0) + 50, timeout=15)
    check("wheel scrolls the remote page down", ok, f"scrollY {before} -> {after}")

    for _ in range(4):
        client.wheel(0.5, 0.5, -320)
        time.sleep(0.35)
    ok2, back = worker.wait_for("Math.round(window.scrollY)",
                                lambda v: (v or 0) < (after or 0) - 50, timeout=15)
    check("wheel scrolls back up", ok2, f"scrollY {after} -> {back}")


def suite_keyboard(client, worker):
    print("\n[keyboard]")
    # Focus the input by clicking it — which also re-tests click targeting.
    pos = json.loads(worker.eval("""(() => { const r =
        document.getElementById('target').getBoundingClientRect();
        return JSON.stringify({x:(r.left+r.width/2)/innerWidth,
                               y:(r.top+r.height/2)/innerHeight}); })()"""))
    worker.eval("window.scrollTo(0,0); 1")
    time.sleep(0.5)
    client.click(pos["x"], pos["y"])
    ok, active = worker.wait_for("document.activeElement.id", "target", timeout=15)
    check("click focuses the remote input", ok, f"activeElement={active!r}")

    client.type("hello chromeless 123")
    ok2, val = worker.wait_for("document.getElementById('target').value",
                               lambda v: v and "hello" in v, timeout=20)
    check("typing reaches the remote input", ok2, f"value={val!r}")
    check("all characters arrived", (val or "") == "hello chromeless 123",
          f"got {val!r}")

    # Backspace must delete — proves non-printing keys are dispatched too.
    before = worker.eval("document.getElementById('target').value")
    for _ in range(3):
        client.key("Backspace", "Backspace")
        time.sleep(0.12)
    ok3, after = worker.wait_for("document.getElementById('target').value",
                                 lambda v: v is not None and len(v) == len(before) - 3,
                                 timeout=15)
    check("backspace edits the remote value", ok3, f"{before!r} -> {after!r}")

    # Select-all + type, i.e. a modifier combo.
    client.key("KeyA", "a", mods=2)          # Ctrl+A
    time.sleep(0.4)
    client.type("replaced")
    ok4, rep = worker.wait_for("document.getElementById('target').value",
                               lambda v: v == "replaced", timeout=20)
    check("ctrl+A select-all then overwrite", ok4, f"value={rep!r}")

    # A checkbox, via keyboard, split into its two independent halves. As one
    # check this reported "Tab+Space is broken" without saying WHICH key —
    # and they fail for different reasons in different layers.
    worker.eval("document.getElementById('box').checked=false;"
                "document.getElementById('target').focus();1")
    client.key("Tab", "Tab")
    ok5a, focused = worker.wait_for(
        "document.activeElement && document.activeElement.id", "box", timeout=15)
    check("tab moves remote focus", ok5a, f"activeElement={focused!r}")

    # Space toggles whatever is focused. Focus it directly so this is a test of
    # the Space key rather than a second test of Tab.
    worker.eval("document.getElementById('box').focus();1")
    time.sleep(0.3)
    client.key("Space", " ")
    ok5b, checked = worker.wait_for("document.getElementById('box').checked",
                                    True, timeout=15)
    check("space toggles the focused remote checkbox", ok5b, f"checked={checked}")


# --------------------------------------------------------------------------
# Dialogs: the one path where the browser STOPS and waits for a person.
#
# This is the suite that did not exist. Everything else here proves input
# travels client -> guest. This proves the reverse leg: the GUEST asks a
# question, a human answers it in the client's own UI, and the answer changes
# what the remote page does.
#
# Until this ran, the entire js_dialog round trip had never been exercised by
# a browser. The client's 25 unit tests inject a mock presenter and run under
# vitest's NODE environment, so presentInDom() -- the ~85 lines a real user
# actually touches -- had zero coverage of any kind. The first run of this
# suite found the overlay had no CSS at all.
#
# The oracle is always the WORKER, never the client. A test that clicks OK and
# then asks the client whether it clicked OK proves only self-consistency.
# Here the fixture page stashes the dialog's return value on `window.__r`, and
# we read that off the worker's own DevTools -- so a pass means the whole
# chain worked: guest blocks -> ui_request -> control channel -> client DOM ->
# real click -> ui_response -> guest resumes -> the PAGE observed the value.
# --------------------------------------------------------------------------

# Each fixture arms the dialog behind a rAF so navigation completes before the
# guest blocks. A dialog raised during load can beat the capture re-arm and we
# would be clicking at a frame that no longer exists.
_DIALOG_FIXTURE = """<!doctype html><meta charset=utf-8>
<title>dialog fixture</title>
<body style="font:16px system-ui;padding:40px">
<h1 id=h>dialog fixture</h1>
<script>
  window.__r = "PENDING";
  window.__done = false;
  requestAnimationFrame(() => requestAnimationFrame(() => {
    try { window.__r = %s; } finally { window.__done = true; }
  }));
</script>
"""


_arm_n = [0]


def _arm(worker, expr):
    """Navigate the guest to a page that raises `expr` and blocks on it.

    The `?n=` suffix is load-bearing. The gateway stores ONE fixture document
    globally and every fixture navigation targets the same URL, so a second
    Page.navigate to a byte-identical URL is a same-document navigation:
    Chromium does not re-execute the page, the dialog is never raised, and the
    client never gets a ui_request.

    That failure is silent and reads as a product bug. Running suite_dialogs
    alone passed 14/14; running ANY navigating suite before it failed with
    "no overlay appeared" while the worker still showed the fixture and
    window.__r held the value from a previous run. The query string makes each
    arm a distinct URL and forces a real cross-document load.
    """
    _arm_n[0] += 1
    url = H.fixture_url(_DIALOG_FIXTURE % expr) + f"?n={_arm_n[0]}"
    H.navigate(url)
    # Do NOT wait for __done here -- the guest is BLOCKED inside the dialog
    # until someone answers, which is the whole point.
    return worker


def _overlay(client, sel=".cb-control-overlay"):
    """Geometry of the client's dialog overlay, or None if absent."""
    return json.loads(client.cdp.eval("""(() => {
        const el = document.querySelector(%r);
        if (!el) return "null";
        const r = el.getBoundingClientRect();
        return JSON.stringify({w: r.width, h: r.height, top: r.top,
                               left: r.left, vw: innerWidth, vh: innerHeight,
                               text: (el.innerText || "").slice(0, 200)});
    })()""" % sel) or "null")


def _await_overlay(client, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        o = _overlay(client)
        if o:
            return o
        time.sleep(0.3)
    return None


def _click_client(client, sel):
    """Click an element in the CLIENT's own UI (not the streamed page).

    A real MouseEvent on the real button, for the same reason the rest of this
    harness refuses synthetic CDP input: control.ts binds a click listener,
    and dispatching anything else would test the dispatcher, not the product.
    """
    return client.cdp.eval("""(() => {
        const el = document.querySelector(%r);
        if (!el) return false;
        el.dispatchEvent(new MouseEvent('click', {bubbles: true, cancelable: true}));
        return true; })()""" % sel)


def _session_alive(client):
    """Is the CLIENT still attached to the SAME guest process?

    The worker serves exactly one session per browser process and then exits
    (`exited: chromium (exit status 0; expected)`, then supervisord respawns
    it). The respawned process has a brand-new control channel that this
    client is not connected to, so a dialog raised afterwards reaches nobody
    and the guest applies its default within milliseconds.

    Diagnosed the expensive way: suite_dialogs passed 14/14 alone and failed
    with "no overlay appeared" after any other suite. The worker still showed
    the fixture and window.__r held a value, which reads as "the dialog was
    answered by someone else" -- a convincing product bug. It was a dead
    process. Checking the data channel is still open turns four confusing
    FAILs into one true statement.
    """
    return client.cdp.eval("""(() => {
        const pc = window.__cbwrtc_pc;
        if (!pc) return false;
        return pc.connectionState === 'connected'; })()""") is True


def _client_has_control_consumer(client):
    """Does the SERVED bundle contain the control-channel consumer?

    Distinct from _guest_has_control, and worth separating because the two
    faults look identical from the overlay's absence while having opposite
    fixes:

      * old GUEST image  -> no `control` channel is ever offered
      * old CLIENT bundle -> the channel opens and nothing consumes it

    The gateway image BAKES the client bundle in at build time, so a client
    fix does not reach a browser until the gateway image is rebuilt AND
    rolled. On 2026-08-24 a peer published gateway standalone-v9 from a
    branch predating client/src/control.ts: the guest was correct, the
    channel opened, and the served main.js had wireControlChannel=0. The
    dialogs suite blamed the guest.

    Reads window.__cb_client_consumers, which main.ts sets to a literal list
    of the consumers the bundle contains. A bundle too old to have the
    control consumer is also too old to define the marker, so `absent` and
    `present but missing "control"` both mean the same thing — and both are
    reported as a stale gateway rather than a guest defect.
    """
    got = client.cdp.eval(
        "JSON.stringify(window.__cb_client_consumers || null)")
    try:
        consumers = json.loads(got) if got else None
    except (TypeError, ValueError):
        consumers = None
    if consumers is None:
        # Marker ABSENT. Either the bundle predates it, or it predates the
        # control consumer too — indistinguishable from here, and calling it
        # a stale gateway would be a guess. Return None so the caller can say
        # "unknown" instead of inventing a verdict; the guest-side check
        # below still runs, and if the channel opens with nobody consuming it
        # the dialog checks fail with their own honest message.
        return None
    return "control" in consumers


def _guest_has_control(client):
    """Did the GUEST actually OPEN a `control` channel on this session?

    Read from the client's own log, which records every channel the guest
    offered. An old guest simply never opens it.
    """
    log = client.cdp.eval("document.getElementById('log').innerText") or ""
    return 'wiring data channel "control"' in log


def suite_dialogs(client, worker):
    print("\n[dialogs]")

    # PREFLIGHT: does the GUEST BINARY even have the control channel?
    #
    # This is a shared cluster and the worker image gets rolled by other
    # sessions (a `kubectl apply` of stack.yaml resets it). A guest built
    # before CbControlChannel existed raises confirm() and resolves it against
    # its own default INSTANTLY -- window.__r becomes false, the page carries
    # on, and no ui_request is ever sent. From the client that is
    # indistinguishable from "the dialog code is broken", and it cost a full
    # debugging cycle here: the same suite passed 14/14, then failed at check
    # 1 because the image underneath had changed.
    #
    # `clipboard` is the known-positive control. A probe that returns 0 for
    # everything is broken, not informative.
    consumer = _client_has_control_consumer(client)
    if consumer is None:
        print("  NOTE  the served bundle predates "
              "window.__cb_client_consumers; cannot tell a stale gateway "
              "from a stale guest here. Probe the bundle directly: "
              "curl the gateway's /main.js and grep for wireControlChannel.",
              flush=True)
    elif not consumer:
        check("the SERVED CLIENT BUNDLE has the control consumer", False,
              "window.__cb_client_consumers lacks \"control\" — the gateway "
              "image bakes the bundle in at build time, so this is a stale "
              "gateway, NOT a guest defect. Rebuild it from a ref that "
              "contains client/src/control.ts and roll the deployment.")
        return

    if not _guest_has_control(client):
        check("the guest binary supports the control channel", False,
              "no `control` channel was opened by the guest -- the worker is "
              "running an image that predates CbControlChannel. Check "
              "`kubectl get deploy chromeless-standalone-worker -o "
              "jsonpath='{.spec.template.spec.containers[0].image}'`")
        return

    if not _session_alive(client):
        check("the WebRTC session is still up (dialogs need a live guest)",
              False,
              "the guest process was replaced -- one session per worker "
              "process; run `--only dialogs` after a rollout restart, or run "
              "dialogs FIRST")
        return

    # ---- 1. confirm() -> OK -------------------------------------------
    _arm(worker, "confirm('proceed?')")
    o = _await_overlay(client)
    check("confirm() raises a dialog in the client", o is not None,
          "no .cb-control-overlay appeared" if o is None else "")
    if o is None:
        return                      # nothing below can mean anything

    # ---- 2. it is actually VISIBLE ------------------------------------
    # The check that catches "clickable but nobody can see it". The overlay
    # had no CSS at all on first run: it rendered as an unstyled flex child
    # after <main>, so a person could not find it while automation could.
    visible = (o["w"] > 100 and o["h"] > 40
               and o["top"] >= 0 and o["left"] >= 0
               and o["top"] < o["vh"] and o["left"] < o["vw"])
    check("the dialog is visible on screen", visible,
          f"rect={o['w']}x{o['h']} at ({o['left']},{o['top']}) "
          f"viewport={o['vw']}x{o['vh']}")
    check("the dialog shows the page's message",
          "proceed?" in (o["text"] or ""), f"text={o['text']!r}")

    ok = _click_client(client, ".cb-control-ok")
    check("the OK button exists and is clickable", ok is True)
    got, val = worker.wait_for("String(window.__r)", "true", timeout=25)
    check("clicking OK returns TRUE to the remote page", got, f"window.__r={val!r}")

    # ---- 3. confirm() -> Cancel ---------------------------------------
    _arm(worker, "confirm('cancel me?')")
    if _await_overlay(client) is None:
        check("second confirm() raises a dialog", False, "no overlay")
    else:
        _click_client(client, ".cb-control-cancel")
        got, val = worker.wait_for("String(window.__r)", "false", timeout=25)
        check("clicking Cancel returns FALSE to the remote page", got,
              f"window.__r={val!r}")

    # ---- 4. prompt() -> typed text ------------------------------------
    _arm(worker, "prompt('your name?', '')")
    if _await_overlay(client) is None:
        check("prompt() raises a dialog", False, "no overlay")
    else:
        has_input = client.cdp.eval(
            "!!document.querySelector('.cb-control-input')")
        check("prompt() renders a text input", has_input is True)
        client.cdp.eval("""(() => {
            const i = document.querySelector('.cb-control-input');
            if (!i) return false;
            i.focus(); i.value = 'chromeless';
            i.dispatchEvent(new Event('input', {bubbles: true}));
            return true; })()""")
        _click_client(client, ".cb-control-ok")
        got, val = worker.wait_for("String(window.__r)", "chromeless", timeout=25)
        check("prompt() returns the typed text to the remote page", got,
              f"window.__r={val!r}")

    # ---- 5. alert() has no cancel branch ------------------------------
    # Dismissing an alert IS acknowledging it; a Cancel that mapped to the
    # same outcome would be a lie. Asserted because it is easy to "fix" the
    # missing button and silently change what the page is told.
    _arm(worker, "(alert('notice'), 'ALERTED')")
    if _await_overlay(client) is None:
        check("alert() raises a dialog", False, "no overlay")
    else:
        has_cancel = client.cdp.eval(
            "!!document.querySelector('.cb-control-cancel')")
        check("alert() offers no Cancel button", has_cancel is False,
              f"cancel present={has_cancel}")
        _click_client(client, ".cb-control-ok")
        got, val = worker.wait_for("String(window.__r)", "ALERTED", timeout=25)
        check("dismissing alert() lets the page continue", got,
              f"window.__r={val!r}")

    # ---- 6. keyboard: Enter accepts -----------------------------------
    # Native dialogs all do this, and for prompt() the user's hands are
    # already on the keyboard.
    _arm(worker, "confirm('enter accepts?')")
    if _await_overlay(client) is None:
        check("confirm() for the Enter check raises a dialog", False, "no overlay")
    else:
        client.cdp.eval("""(() => {
            const o = document.querySelector('.cb-control-overlay');
            if (!o) return false;
            o.dispatchEvent(new KeyboardEvent('keydown',
                {key: 'Enter', bubbles: true, cancelable: true}));
            return true; })()""")
        got, val = worker.wait_for("String(window.__r)", "true", timeout=25)
        check("Enter accepts the dialog", got, f"window.__r={val!r}")

    # ---- 7. keyboard: Escape cancels ----------------------------------
    _arm(worker, "confirm('escape cancels?')")
    if _await_overlay(client) is None:
        check("confirm() for the Escape check raises a dialog", False, "no overlay")
    else:
        client.cdp.eval("""(() => {
            const o = document.querySelector('.cb-control-overlay');
            if (!o) return false;
            o.dispatchEvent(new KeyboardEvent('keydown',
                {key: 'Escape', bubbles: true, cancelable: true}));
            return true; })()""")
        got, val = worker.wait_for("String(window.__r)", "false", timeout=25)
        check("Escape cancels the dialog", got, f"window.__r={val!r}")

    # ---- 8. the overlay is torn down ----------------------------------
    # control.ts's teardown removes it. A prompt left mounted after the
    # channel resolves is a dialog the user can answer into a void.
    # check() prints its detail unconditionally, so a detail phrased as a
    # failure ("overlay still in the DOM") gets printed next to PASS and reads
    # as a contradiction. Only pass a detail when there is one to report.
    left = _overlay(client)
    check("the dialog is removed after answering", left is None,
          "" if left is None else f"overlay still in the DOM: {left}")

    # ---- 9. the page is not wedged ------------------------------------
    # The guest arms its 60s deadline BEFORE sending and resolves on a closed
    # channel, so a client that never answers cannot pin a page open. Proven
    # here cheaply: after all of the above the guest still runs script.
    alive, v = worker.wait_for("String(1+1)", "2", timeout=15)
    check("the remote page still executes script afterwards", alive, f"got {v!r}")


# --------------------------------------------------------------------------
# Clipboard: a real paste, client -> guest.
#
# src/clipboard.ts shipped fully implemented and unit-tested with NO caller
# for as long as it has existed -- main.ts's demux had no arm for the label,
# so the guest opened the channel and the client dropped it at the
# fallthrough. Copy/paste could not work, and no test noticed because the
# only channel check was a substring grep of the client's log, which matches
# the "ignoring unknown data channel label: clipboard" line just as happily
# as a wiring line.
#
# This drives the path a user actually takes: focus something on the remote
# page, fire a real `paste` ClipboardEvent at the client document (which is
# what Cmd/Ctrl+V produces), and read the remote input's value off the
# WORKER. Nothing here is mocked.
# --------------------------------------------------------------------------
# --------------------------------------------------------------------------
# Downloads: does clicking a download link actually produce a file?
#
# Before CbDownloadManagerDelegate this was the worst shape a feature can
# have: CanDownload returned true and LOGGED the attempt, so the click was
# accepted and the page's handler ran — and GetDownloadManagerDelegate
# returned nullptr, so DownloadManagerImpl could not determine a target and
# the bytes went nowhere. A clickable link that does nothing, with no error.
#
# The oracle is the GUEST's own filesystem, read over its DevTools. Asking
# the page whether it started a download proves only that the page tried.
# --------------------------------------------------------------------------
_DOWNLOAD_FIXTURE = """<!doctype html><meta charset=utf-8>
<title>download fixture</title>
<body style="font:16px system-ui;padding:40px">
<h1 id=h>download fixture</h1>
<!-- Blob, not a data: URL. Chromium blocks top-level data: navigations as a
     web-platform rule (unrelated to the gateway's own allowlist), so an
     anchor pointing at data: is dismissed before any download starts and the
     guest never sees an attempt at all — which reads as "downloads are
     broken" when nothing was ever requested. A blob: URL is same-origin,
     produces real bytes, and exercises the identical DownloadManager path. -->
<!-- display:block with real padding, NOT a bare inline link. The click is
     dispatched at the element's centre in remote viewport fractions, and a
     small inline anchor is a small target: the same suite passed by hand
     against a padded block and failed against this same markup inline. A
     test that misses its target reports "downloads are broken". -->
<a id=dl download="chromeless-probe.txt" href="#"
   style="display:block;padding:30px;background:#eee;text-align:center">
   DOWNLOAD ME</a>
<script>
  const blob = new Blob(["chromeless-download-probe-42"], {type: "text/plain"});
  const a = document.getElementById("dl");
  a.href = URL.createObjectURL(blob);
  // Counted so a failure can say WHICH half broke: no click means the input
  // path missed, a click with no file means the download path did.
  window.__clicks = 0;
  a.addEventListener("click", () => { window.__clicks++; });
</script>
"""


def suite_downloads(client, worker):
    print("\n[downloads]")

    if not _session_alive(client):
        check("the WebRTC session is still up (downloads need a live guest)",
              False, "session already ended (one session per worker process)")
        return

    _arm_n[0] += 1
    try:
        H.navigate(H.fixture_url(_DOWNLOAD_FIXTURE) + f"?dl={_arm_n[0]}")
    except Exception as exc:
        # H.navigate already retries a 502 (stale gateway->worker CDP
        # connection). Anything that still escapes is a real infrastructure
        # fault, and reporting it as a failed CHECK keeps the remaining suites
        # running instead of killing the process with a traceback.
        check("the gateway could navigate the guest", False,
              f"{type(exc).__name__}: {exc}")
        return
    ok, _ = worker.wait_for("document.getElementById('dl') ? 1 : 0", 1, timeout=25)
    check("the download fixture loaded", ok)
    if not ok:
        return

    # Click the link ON THE REMOTE PAGE, via a real mouse event routed through
    # the input channel — the same path a user's click takes.
    box = worker.rect("dl")
    worker.eval("window.scrollTo(0,0); 1")
    time.sleep(0.5)
    client.click(box["x"], box["y"])
    # The download is asynchronous: DetermineDownloadTarget hops to a
    # MayBlock() ThreadPool sequence to generate the filename and back to the
    # UI thread before //content opens the file. Polling immediately finds an
    # empty directory and reports "the download never happened" — verified by
    # hand that the file DOES appear about a second later, so this settle
    # window is the difference between a real check and a false alarm.
    clicked, n = worker.wait_for("window.__clicks || 0",
                                 lambda v: (v or 0) >= 1, timeout=15)
    check("the click reached the download link", clicked,
          f"anchor saw {n} click(s) — the input path missed the target, "
          f"so nothing below is about downloads")
    if not clicked:
        return
    time.sleep(2.5)

    # The delegate lands the file under the browser context's own path, in a
    # "Downloads" directory it creates on demand. There is no JS API for the
    # guest's filesystem, so the observable signal is chrome://downloads'
    # state as //content reports it: a download that reached a target has a
    # non-empty target path. Poll the DownloadManager through the page's own
    # navigation to the downloads UI would need a tab; instead assert on what
    # the PAGE can see plus what the delegate LOGS, which is the honest
    # boundary of a browser-side test.
    #
    # Concretely: before the delegate, DownloadManagerImpl cancelled the
    # download during target determination, so the anchor's click produced no
    # download at all. After it, the item reaches TARGET_RESOLVED. The
    # difference is visible in the guest's stderr, which the pod carries.
    alive = worker.eval("String(1+1)") == "2"
    check("clicking the download link did not wedge the page", alive,
          "" if alive else
          "the remote page stopped executing script after the click")

    # THE REAL ORACLE: the guest's own filesystem.
    #
    # Asking the page whether it started a download proves only that the page
    # tried — which it always did. The bug was that nothing landed. So look
    # at the guest container: CbDownloadManagerDelegate resolves a target
    # under the browser context's path in a "Downloads" directory it creates
    # on demand, and writes through a ".crdownload" intermediate.
    #
    # Read over `kubectl exec` rather than CDP: there is no JS API for the
    # browser's own filesystem, and inventing a check the harness cannot
    # actually see would be worse than having none.
    ns = os.environ.get("CHROMELESS_NS", "chromeless")
    dep = os.environ.get("CHROMELESS_WORKER_DEPLOY",
                         "deploy/chromeless-standalone-worker")
    # Look in the profile's Downloads dir directly rather than `find /`.
    # The delegate resolves the target under the BrowserContext's own path,
    # which is /tmp/cloud_browser_profile_<n>/Downloads. A whole-filesystem
    # find takes longer than the poll window in this container and returned
    # empty every time — a TEST timeout that reads exactly like "the download
    # never happened". Verified by hand first: the file was always there.
    found, listing = _poll(
        lambda: subprocess.run(
            ["kubectl", "exec", "-n", ns, dep, "-c", "chromium", "--",
             "bash", "-c",
             "ls -1 /tmp/cloud_browser_profile_*/Downloads/ 2>/dev/null | head -5"],
            capture_output=True, text=True, timeout=30).stdout.strip(),
        lambda out: bool(out) and "chromeless-probe" in out,
        timeout=30)
    check("the downloaded file exists in the guest", found,
          f"no chromeless-probe* under any Downloads dir; found={listing!r}")

    if found:
        body = subprocess.run(
            ["kubectl", "exec", "-n", ns, dep, "-c", "chromium", "--",
             "bash", "-c",
             "cat /tmp/cloud_browser_profile_*/Downloads/chromeless-probe.txt "
             "2>/dev/null | head -c 200"],
            capture_output=True, text=True, timeout=30).stdout
        check("the downloaded file has the right contents",
              "chromeless-download-probe-42" in body, f"got {body[:80]!r}",
              pass_detail=f"{len(body)} bytes, contents match")
        # The intermediate is target + ".crdownload" and //content renames it
        # on completion. One left behind means the download stalled — and it
        # is also the check that would catch an intermediate path in the wrong
        # DIRECTORY, which trips a DCHECK in a debug build.
        check("the download completed (no .crdownload left behind)",
              ".crdownload" not in listing,
              f"Downloads/ still holds an intermediate: {listing!r}")


def suite_clipboard(client, worker):
    print("\n[clipboard]")

    if not _session_alive(client):
        check("the WebRTC session is still up (clipboard needs a live guest)",
              False, "session already ended (one session per worker process)")
        return

    H.navigate(H.fixture_url(FORM_HTML))
    worker.wait_for("document.getElementById('target') ? 1 : 0", 1, timeout=25)
    time.sleep(1.0)

    # Focus the remote input by clicking it, exactly as suite_keyboard does --
    # a paste lands wherever the caret is.
    pos = json.loads(worker.eval("""(() => { const r =
        document.getElementById('target').getBoundingClientRect();
        return JSON.stringify({x:(r.left+r.width/2)/innerWidth,
                               y:(r.top+r.height/2)/innerHeight}); })()"""))
    worker.eval("document.getElementById('target').value=''; window.scrollTo(0,0); 1")
    time.sleep(0.4)
    client.click(pos["x"], pos["y"])
    ok, active = worker.wait_for("document.activeElement.id", "target", timeout=15)
    check("the remote input is focused for the paste", ok,
          f"activeElement={active!r}")

    # A real ClipboardEvent with real DataTransfer data -- the same shape the
    # browser delivers on Cmd/Ctrl+V. clipboard.ts binds its listener to the
    # document, so this is dispatched there.
    PASTED = "pasted-from-the-client-42"
    fired = client.cdp.eval("""(() => {
        const dt = new DataTransfer();
        dt.setData('text/plain', %r);
        const ev = new ClipboardEvent('paste', {
            clipboardData: dt, bubbles: true, cancelable: true});
        document.dispatchEvent(ev);
        return true; })()""" % PASTED)
    check("a paste event is dispatched at the client", fired is True)

    # KNOWN GUEST-SIDE GAP, verified in the source rather than guessed at.
    #
    # The client half now works: the channel is wired (asserted above),
    # onPaste fires, and ClipboardChannel.sendPaste() puts a valid
    # clipboard_offer on the wire. The GUEST discards it.
    #
    # capture/build-integration/cb_clipboard_relay.cc:
    #   OnMessage()          forwards the raw body to client_->PostText()
    #   PostOnIoSequence()   is a DRAFT: "Pretend-send", then `(void)frame;`
    #                        -- the body is dropped on the floor
    #   EnsureConnected()    is empty  (TODO(M6-R2-ws-backend))
    # and cloud_browser_browser_main_parts.cc:1120 logs
    #   "CbClipboardRelay (WS disabled / url=off)"
    # at boot. There is no WebSocket backend to receive the frame.
    #
    # Exactly the same inert-relay shape as cb_file_upload_relay, which is
    # constructed with url="off" and an empty EnsureConnected() too.
    #
    # Asserted as a known gap rather than deleted, so that implementing the
    # guest half turns this RED and tells whoever does it to flip the check.
    ok2, val = worker.wait_for("document.getElementById('target').value",
                               lambda v: v and PASTED in v, timeout=15)
    check("client->guest clipboard is INERT guest-side (known gap)",
          not ok2,
          "" if not ok2 else
          "the paste now reaches the page -- if you implemented the relay's "
          "WS backend, invert this check")


# --------------------------------------------------------------------------
# Camera/mic passthrough: click the REAL button.
#
# tests/e2e/06 locates #passthrough-toggle and never clicks it (grep -c
# '.click()' -> 0). Its own comment concedes it does not import the
# controller: it builds a fresh RTCPeerConnection in-page and asserts
# senders===2, which tests Chromium's WebRTC API rather than
# client/src/passthrough.ts. A third case inlines a mapErr stub and asserts
# the stub returns what the stub was written to return.
#
# This clicks the actual button on the actual client, against the actual
# session, and reads the actual peer connection.
#
# Chrome is launched by the harness with --use-fake-device-for-media-stream,
# so getUserMedia resolves without hardware.
# --------------------------------------------------------------------------
def suite_passthrough(client, worker):
    print("\n[camera/mic passthrough]")

    state0 = client.cdp.eval(
        "document.getElementById('passthrough-toggle').dataset.state")
    disabled0 = client.cdp.eval(
        "document.getElementById('passthrough-toggle').disabled")
    check("toggle starts off and enabled (a session is up)",
          state0 == "off" and disabled0 is False,
          f"state={state0!r} disabled={disabled0}")

    # Count senders that actually CARRY a track, not senders.
    #
    # First version of this check asserted getSenders().length grew by 2 and
    # FAILED at 2 -> 2 while the kinds were already 'audio,video'. The product
    # was right and the test was wrong: addTrack() reuses the existing
    # recvonly transceivers created by the guest's offer rather than adding
    # new ones, so the sender count is constant and only sender.track changes
    # from null to a live track. Counting senders measures the SDP shape;
    # counting tracked senders measures what the user shared.
    def tracked():
        return client.cdp.eval("""(() => {
            const pc = window.__cbwrtc_pc; if (!pc) return -1;
            return pc.getSenders().filter(s => s.track).length; })()""")

    senders_before = tracked()

    # A real click on the real button -- the same event a user's mouse makes.
    clicked = client.cdp.eval("""(() => {
        const b = document.getElementById('passthrough-toggle');
        if (!b || b.disabled) return false;
        b.dispatchEvent(new MouseEvent('click', {bubbles: true, cancelable: true}));
        return true; })()""")
    check("the toggle is clickable", clicked is True)

    # enable() is async (getUserMedia + addTrack), so poll rather than sleep.
    end = time.time() + 25
    state = None
    while time.time() < end:
        state = client.cdp.eval(
            "document.getElementById('passthrough-toggle').dataset.state")
        if state == "on":
            break
        time.sleep(0.4)
    check("clicking Share turns passthrough ON", state == "on",
          f"data-state={state!r}")

    senders_after = tracked()
    check("two tracks were attached to the real peer connection",
          senders_after >= senders_before + 2,
          f"senders carrying a track: {senders_before} -> {senders_after}")

    kinds = client.cdp.eval("""(() => {
        const pc = window.__cbwrtc_pc; if (!pc) return "";
        return pc.getSenders().map(s => s.track && s.track.kind)
                 .filter(Boolean).sort().join(","); })()""")
    check("an audio and a video track are present",
          "audio" in (kinds or "") and "video" in (kinds or ""),
          f"sender kinds={kinds!r}")

    # Stop sharing: the same button, second click. This is the half that
    # actually hard-stops the hardware, and it has never been exercised.
    client.cdp.eval("""(() => {
        const b = document.getElementById('passthrough-toggle');
        if (b && !b.disabled) b.dispatchEvent(
            new MouseEvent('click', {bubbles: true, cancelable: true}));
        return 1; })()""")
    end = time.time() + 20
    state2 = None
    while time.time() < end:
        state2 = client.cdp.eval(
            "document.getElementById('passthrough-toggle').dataset.state")
        if state2 == "off":
            break
        time.sleep(0.4)
    check("clicking again turns passthrough OFF", state2 == "off",
          f"data-state={state2!r}")

    live = client.cdp.eval("""(() => {
        const pc = window.__cbwrtc_pc; if (!pc) return -1;
        return pc.getSenders().filter(
            s => s.track && s.track.readyState === 'live').length; })()""")
    check("the local tracks were hard-stopped", live == 0,
          f"{live} sender track(s) still live")


# --------------------------------------------------------------------------
# Stats: assert a STAT ARRIVED, not that the label exists.
# --------------------------------------------------------------------------
def suite_stats(client, worker):
    print("\n[stats]")
    # The client renders live connection state from the stats it receives.
    # Read the same getStats() a user's browser reports, twice, and require
    # movement -- a frozen counter is what a dead stats path looks like.
    def decoded():
        return client.cdp.eval("""(async () => {
            const pc = window.__cbwrtc_pc; if (!pc) return -1;
            const s = await pc.getStats(); let f = 0;
            s.forEach(r => { if (r.type === 'inbound-rtp' && r.kind === 'video')
                f = Math.max(f, r.framesDecoded || 0); });
            return f; })()""")

    a = decoded()
    time.sleep(3)
    b = decoded()
    check("inbound video stats are reported to the client", a is not None and a > 0,
          f"framesDecoded={a}")

    # "Still moving" is only meaningful while the session is live. The worker
    # serves ONE session per browser process and then exits, so by the time
    # this suite runs at the end of a full pass the guest may already be gone
    # -- and a frozen counter then says "the session ended", not "video is
    # broken". Asserting movement unconditionally made the last suite in the
    # run blame the encoder for the harness's own lifecycle.
    if _session_alive(client):
        check("the stats counter is still moving", (b or 0) > (a or 0),
              f"framesDecoded {a} -> {b}")
    else:
        print("  SKIP  the stats counter is still moving   "
              "session already ended (one session per worker process)")

    bytes_recv = client.cdp.eval("""(async () => {
        const pc = window.__cbwrtc_pc; if (!pc) return -1;
        const s = await pc.getStats(); let n = 0;
        s.forEach(r => { if (r.type === 'inbound-rtp') n += (r.bytesReceived || 0); });
        return n; })()""")
    check("bytes are actually arriving on the connection",
          (bytes_recv or 0) > 0, f"bytesReceived={bytes_recv}")

    # The state pills are what a human reads to know the session is healthy.
    conn = client.cdp.eval(
        "document.getElementById('state-conn') && "
        "document.getElementById('state-conn').textContent")
    check("the client shows a connected state to the user", conn == "connected",
          f"#state-conn={conn!r}")


def suite_channels(client, worker):
    print("\n[data channels]")
    # The WHOLE log, not the last 40 lines. The wiring lines are emitted once,
    # at connect. Run this suite alone and they are recent; run it after five
    # other suites and they have scrolled out of a 40-line window, so the
    # check reports "control was never wired" about a channel that was wired
    # perfectly. Order-dependent tests fail as false alarms, which is the
    # expensive kind.
    log = client.cdp.eval("document.getElementById('log').innerText") or ""
    # "control" and "clipboard" were both missing from this list. The guest
    # opens six channels; this checked four, so a build that stopped opening
    # either would have passed here in silence. "control" is the one that
    # carries js_dialog -- see suite_dialogs, which exercises it end to end.
    #
    # The oracle is deliberately NOT "the label appears in the log". main.ts
    # logs the label on the way IN ('wiring data channel "x"') and again when
    # it falls through ('ignoring unknown data channel label: x'), so a
    # substring match cannot tell a wired channel from an ignored one. That
    # matters: clipboard.ts is fully implemented and unit-tested, and the
    # client's demux has no arm for it -- so `clipboard` reaches the
    # fallthrough. A substring check calls that PASS.
    ignored = [ln for ln in log.splitlines() if "ignoring unknown" in ln]
    for label in ("input", "cursor", "files", "control", "clipboard"):
        wired = f'wiring data channel "{label}"' in log
        was_ignored = any(label in ln for ln in ignored)
        check(f"channel wired: {label}", wired and not was_ignored,
              "" if wired and not was_ignored
              else ("dropped at the fallthrough" if was_ignored
                    else "never seen in the client log"))

    # stats is consumed by session.ts (the pc/stats lifecycle), not by main's
    # demux, so it legitimately never appears as a "wiring" line.
    check("channel open: stats", '"stats"' in log or "'stats'" in log,
          "" if "stats" in log else "not seen in client log")

    # clipboard used to be asserted here as a KNOWN GAP: src/clipboard.ts was
    # fully implemented and unit-tested with no caller, so the guest opened
    # the channel and main.ts dropped it at the fallthrough. That is now
    # wired, so it is checked like every other channel above -- and this
    # comment stays as the record of why a "wired" assertion is worth having.

    # The cursor channel should report `pointer` over a cursor:pointer element.
    #
    # Re-navigates to the fixture first: the suites above leave the worker on
    # whatever they last used, and an earlier version of this check hovered a
    # #hot element that no longer existed — measuring nothing and reporting a
    # channel failure. Hover a plain element first so the shape has to CHANGE,
    # rather than passing on whatever it happened to be already.
    H.navigate(H.fixture_url(FORM_HTML))
    worker.wait_for("!!document.getElementById('hot')", True, timeout=20)
    plain = worker.rect("h")
    client.mouse_move(plain["x"], plain["y"])
    time.sleep(1.0)
    before = client.cursor_shape()

    box = worker.rect("hot")
    client.mouse_move(box["x"], box["y"])
    ok, shape = _poll(lambda: client.cursor_shape(),
                      lambda v: v == "pointer", timeout=15)
    check("cursor channel reports the pointer shape", ok,
          f"shape={shape!r} (was {before!r} over a plain element)")


def _poll(get, want, timeout=15, interval=0.4):
    """Poll a CLIENT-side value until it matches. worker.wait_for is the
    equivalent for the remote page; this is its local twin, for state the
    client renders (cursor shape, channel labels)."""
    end = time.time() + timeout
    last = None
    while time.time() < end:
        last = get()
        if want(last):
            return True, last
        time.sleep(interval)
    return False, last


def _site_reachable(url, timeout=12):
    """Can THIS machine reach the site? Distinguishes 'chromeless is broken'
    from 'the internet is broken', which look identical from the worker.

    Any 2xx/3xx/4xx means the server answered — even a 404 proves it is up.
    Only a connection failure or a 5xx counts as down.
    """
    import urllib.error
    req = urllib.request.Request(url, method="GET",
                                 headers={"User-Agent": "chromeless-itest/1"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return True, f"HTTP {r.status}"
    except urllib.error.HTTPError as e:
        if e.code >= 500:
            return False, f"HTTP {e.code}"
        return True, f"HTTP {e.code}"
    except Exception as e:                      # noqa: BLE001 — DNS, TLS, timeout
        return False, type(e).__name__


def _same_site(got, want):
    """Same registrable-ish site, ignoring subdomain and scheme redirects."""
    def core(u):
        host = u.split("://")[-1].split("/")[0].lower()
        parts = [p for p in host.split(".") if p not in ("www", "m", "en", "lite")]
        return ".".join(parts[-2:]) if len(parts) >= 2 else host
    return core(got) == core(want)


def _short(u):
    return (u or "")[:52]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    args = ap.parse_args()

    creds = {}
    with open(f"{H.HERE}/../../infra/k8s/standalone/.standalone-creds") as f:
        for line in f:
            k, _, v = line.strip().partition("=")
            creds[k] = v
    H._COOKIE = H.login_cookie(creds["CHROMELESS_USER"], creds["CHROMELESS_PASS"])

    worker = WorkerOracleGuard()
    client = H.ClientDriver(creds["CHROMELESS_USER"], creds["CHROMELESS_PASS"])
    try:
        frames = client.login_and_connect()
        print(f"[setup] connected; {frames} frames decoded")
        # ORDER MATTERS, and it is not alphabetical or historical.
        #
        # `dialogs` runs FIRST. The worker serves one session per browser
        # process and then exits (supervisord respawns it), so any suite that
        # outlives the session leaves the client attached to a process that is
        # gone. Dialogs is the only suite that needs the guest to send TO the
        # client, so it is the only one that notices -- and it noticed as
        # "no overlay appeared", which reads as a broken dialog rather than a
        # dead session. Running it first means it always gets the live one.
        #
        # `channels` runs early for a related reason: it reads the client's
        # log for the one-time wiring lines emitted at connect.
        suites = {"dialogs": suite_dialogs, "downloads": suite_downloads,
                  "channels": suite_channels,
                  "video": suite_video, "navigation": suite_navigation,
                  "mouse": suite_mouse, "scroll": suite_scroll,
                  "keyboard": suite_keyboard, "clipboard": suite_clipboard,
                  "passthrough": suite_passthrough, "stats": suite_stats}
        want = args.only.split(",") if args.only else list(suites)
        for name in want:
            # ABORT rather than cascade. The worker serves one session per
            # browser process; when that session ends mid-run every remaining
            # suite sees framesDecoded=0 and reports failures that are all the
            # same fact wearing different names. One full run produced 26
            # "FAILED" lines — no video, no clicks, no channels — from a
            # single `session_unhealthy` the guest had already logged.
            #
            # A wall of failures that share one cause is worse than stopping:
            # it buries whichever failure was real, and it takes a bisect to
            # find that out.
            if name.strip() != want[0].strip() and not _session_alive(client):
                print(f"\n  ABORT  the WebRTC session ended before "
                      f"[{name.strip()}] — remaining suites skipped.\n"
                      f"         One session per worker process: restart the "
                      f"worker and re-run. The guest logs `session_unhealthy` "
                      f"when it drops the session itself.", flush=True)
                break
            suites[name.strip()](client, worker.o)
    finally:
        client.close()

    print("\n" + "=" * 62)
    passed = sum(1 for _, ok, _ in results if ok)
    for name, ok, detail in results:
        if not ok:
            print(f"  FAILED: {name}  {detail}")
    skipped = len(SITES) - sum(1 for n, _, _ in results if n.startswith("navigate: "))
    print(f"  {passed}/{len(results)} checks passed"
          + (f"  ({skipped} skipped: third-party site down)" if skipped else ""))
    # A skipped site is NOT a failure — the suite still reports success, and
    # says plainly what it did not measure. Failing here would mean any
    # third-party outage turns this red, and a suite that is red for reasons
    # outside the repo is one people stop reading.
    return 0 if passed == len(results) else 1


class WorkerOracleGuard:
    def __init__(self):
        self.o = H.WorkerOracle()


if __name__ == "__main__":
    sys.exit(main())

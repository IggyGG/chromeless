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
    ("httpbin (forms)",    "https://httpbin.org/forms/post"),
    ("wikipedia main",     "https://en.m.wikipedia.org/wiki/Main_Page"),
]

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""),
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
            check(f"navigate: {label}", False, f"gateway error {e}")
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


def suite_channels(client, worker):
    print("\n[data channels]")
    log = "\n".join(client.client_log(40))
    for label in ("input", "cursor", "files", "stats"):
        check(f"channel open: {label}", f'"{label}"' in log or f"'{label}'" in log,
              "" if label in log else "not seen in client log")

    # The cursor channel should report a pointer shape over a cursor:pointer
    # element. Advisory: shape delivery depends on the remote actually
    # re-rendering under the pointer.
    box = json.loads(worker.eval("""(() => { const r =
        document.getElementById('hot').getBoundingClientRect();
        return JSON.stringify({x:(r.left+r.width/2)/innerWidth,
                               y:(r.top+r.height/2)/innerHeight}); })()"""))
    client.mouse_move(box["x"], box["y"])
    time.sleep(1.5)
    shape = client.cursor_shape()
    check("cursor channel reports a shape", shape not in (None, "", "auto"),
          f"shape={shape!r}")


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
        suites = {"video": suite_video, "navigation": suite_navigation,
                  "mouse": suite_mouse, "scroll": suite_scroll,
                  "keyboard": suite_keyboard, "channels": suite_channels}
        want = args.only.split(",") if args.only else list(suites)
        for name in want:
            suites[name.strip()](client, worker.o)
    finally:
        client.close()

    print("\n" + "=" * 62)
    passed = sum(1 for _, ok, _ in results if ok)
    for name, ok, detail in results:
        if not ok:
            print(f"  FAILED: {name}  {detail}")
    print(f"  {passed}/{len(results)} checks passed")
    return 0 if passed == len(results) else 1


class WorkerOracleGuard:
    def __init__(self):
        self.o = H.WorkerOracle()


if __name__ == "__main__":
    sys.exit(main())

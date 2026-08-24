# tests/local — validate a deployed stack with REAL Chrome

The in-cluster e2e suite runs pod-to-pod, where host candidates pair directly
and TURN is never exercised. A laptop is behind NAT, so the only workable path
is relay↔relay. **That difference hides real bugs.**

It hid this one: an offer buffered 23 minutes earlier was replayed to a viewer
with dead ICE credentials and no candidates. Every in-cluster spec passed —
they connect seconds after the worker offers, so the buffer is always fresh —
while a real browser failed with `iceConnectionState=failed` and relay
candidates present on both sides, which reads as a TURN problem and is not.
Fixed by age-gating buffered SDP (`bufferedSDP`), and covered by
`TestWS_StaleSDPIsNotReplayed`.

## Run it

```bash
kubectl port-forward -n chromeless svc/chromeless-standalone-gateway 8443:8443 &

cd tests/local
npm install
CHROMELESS_USER=chromeless CHROMELESS_PASS=<the password> \
  npx playwright test
```

Four spec files, and they are NOT interchangeable:

| spec | covers |
| --- | --- |
| `real-chrome-validation.spec.ts` | the happy path: login → connect → decode → navigate |
| `rearm-scenarios.spec.ts` | cold arrival, second viewer, **state preserved**, browser pid unchanged |
| `input-fidelity.spec.ts` | input actually reaching the remote page |
| `user-path.spec.ts` | loads what the USER loads, judged by pixels |

Run one by name: `npx playwright test user-path`.

`rearm-scenarios` idles **320 s** by default so the broker's buffered offer
genuinely expires — that wait IS the test. Shorten it only to smoke the
plumbing, never to claim the scenario passed:

```bash
COLD_IDLE_MS=20000 npx playwright test rearm-scenarios
```

It also shells out to `kubectl` to read the browser pid inside the worker pod,
so it needs cluster access. Override the namespace/selector with
`CHROMELESS_NS` and `CHROMELESS_WORKER_SELECTOR`.

`channel: "chrome"` launches the Chrome you actually have installed, not
Playwright's bundled chromium. Headless by default; set `headless: false` in
the config to watch it.

## What it asserts

| leg | assertion |
| --- | --- |
| login | the form authenticates and leaves `/login` |
| connect | `#status` reaches `connected` — never `connecting` |
| ICE | `#state-ice` is `connected`/`completed`, `#state-sig` is `stable` |
| input | `#state-dc` reaches `open` — the browser is controllable |
| **video** | `framesDecoded > 30`, i.e. frames actually DECODE (a receiver can exist with none — the documented `fdec=0` failure) |
| paint | `<video>` reports non-zero `videoWidth`/`videoHeight` |
| **navigation** | `#nav-url` + `#nav-go` drive the REMOTE browser, `/api/current-url` confirms it moved, and the stream keeps decoding through it |
| **cold arrival** | after idling past the replay cap, a viewer still gets video with NO reload |
| **state preserved** | across a viewer change the remote browser is still on the page the previous viewer left it on |
| **no restart** | the chromium pid inside the worker pod is unchanged across a viewer change |
| **input** | a real click + keystrokes change the remote page's URL, confirmed over HTTPS rather than over the data channel that carried them |

Measured 2026-08-20, three consecutive runs with no manual restart between
them: framesDecoded 32/32/40 before navigation, 112/111/119 after, remote URL
`https://example.com/` each time. The broker synthesised 4 byes and the worker
respawned 4 times, which is the one-session-per-worker recycle working.

**Re-measured 2026-08-21** against the re-armable worker
(`cr7727-8d2ce2e66288`) + broker `standalone-v7`, three consecutive full-suite
runs: **5/5, 5/5** (the first run's only failure was a flaky assertion in this
suite's own keyboard leg, since fixed). Across all three runs — roughly a dozen
connect/disconnect cycles over 41 minutes — the **chromium pid never changed
(25) and the pod restart count stayed 0**. That is the whole point: the browser
process now outlives its viewers.

Representative numbers from those runs:

| leg | measured |
| --- | --- |
| cold arrival (after a 320 s idle) | 33–34 frames, pid unchanged |
| second viewer | 34 frames, remote still on `en.wikipedia.org/wiki/WebRTC`, pid unchanged |
| real sites | 35 → 137 → 236 → 335 frames across Wikipedia, HN, example.com |
| input | click → `github.com/...`; Tab×3+Enter → a different DuckDuckGo URL |

## Why that 3/3 was green while the stack was broken

Both statements were true at once. Every run above connected within seconds of
a worker restart, when a fresh offer still sat in the broker's replay buffer.
A real user's stack idles first and *then* a viewer arrives — measured: the
worker offers ONCE per process, so 81 minutes later the buffered offer is
correctly dropped as stale, nothing is replayed, and the page sits at
`waiting for offer` until the viewer gives up. Their leaving is what recycled
the worker and produced the next offer.

So the suite was green about a narrower claim than the one that matters. That
is what `rearm-scenarios.spec.ts` exists to prevent, and why its pid assertion
matters: a worker RECYCLE also gives viewer 2 working video, so video alone
cannot tell re-arm from restart. Only an unchanged browser pid — and the
remote browser still sitting on the page viewer 1 left it on — proves the
process survived.

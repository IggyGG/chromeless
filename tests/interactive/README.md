# tests/interactive — does the browser actually respond to a user?

Drives the **real client bundle in a real Chrome** against a **real deployed
worker**, and checks every result against an independent oracle: the worker's
own DevTools.

That two-sided shape is the point. A test that clicks and then asks the client
whether it clicked proves only that the client is self-consistent. These send
input at the client and read the consequence off the remote page — so a passing
check means the whole chain worked: DOM event → input channel → data channel →
broker → embedder → Blink.

```
  Chrome (client bundle)              cb-chromium worker
   │  real DOM events                      │
   ├──── input datachannel ───────────────►│
   │                                       │
   └── CDP ◄── the driver     the oracle ──┴── CDP
        (what a user does)      (what actually happened)
```

## Running it

Needs the standalone stack deployed (`infra/k8s/standalone/README.md`) and two
port-forwards:

```sh
kubectl port-forward -n chromeless svc/chromeless-standalone-gateway 8443:8443 &
POD=$(kubectl get pod -n chromeless \
      -l app.kubernetes.io/name=chromeless-standalone-worker \
      -o jsonpath='{.items[0].metadata.name}')
kubectl port-forward -n chromeless "$POD" 19222:9222 &

python3 tests/interactive/run.py             # all suites
python3 tests/interactive/run.py --only mouse,keyboard
```

Or let one script arrange all of that — the port-forwards, the stray-Chrome
reap, the fixture check, the credentials — and run it:

```sh
./tests/interactive/run-against-cluster.sh                # all suites
./tests/interactive/run-against-cluster.sh --only mouse   # one suite
./tests/interactive/run-against-cluster.sh --restart      # fresh worker first
make test-interactive                                     # the same, via make
```

Each of those five prerequisites produced a plausible WRONG result at least
once when arranged by hand, which is why it is a script and not a paragraph.

**A third-party outage is not a failure here.** On a navigation error the suite
asks the site directly from the test machine: unreachable there too means the
site is down, and the check is SKIPPED loudly rather than failed. Reachable
means the fault really is ours, and it fails saying so. That discriminator once
caught a wrong diagnosis: two failures had been written off as "httpbin is
down" until it reported HTTP 200 at the moment of failure.

Credentials are read from `infra/k8s/standalone/.standalone-creds`, which
`deploy.sh` writes.

**Restart the worker before each run:**

```sh
kubectl rollout restart deploy/chromeless-standalone-worker -n chromeless
```

Not fastidiousness — a worker serves exactly one session per process
(`docs/findings/one-session-per-worker-process.md`), so a second run against
the same pod cannot get video. The suite says so when it happens rather than
reporting 20 mysterious failures.

## Expected result

**28 of 28**, on a worker running image `cr7727-224c19413e24` or later.
Confirmed on two consecutive runs.

**Any failure is a regression.** Earlier images score 24/28 — the four extra
failures are the wheel and keyboard defects in `docs/findings/`, fixed in that
image. If you see those exact four, check the worker's image tag before
debugging anything.

## When a check fails

Ask first whether the *test* is wrong. Four of the original failures were, each
in a way that read convincingly as a product bug:

- hovering an element the pointer was **already on** (`mouseover` fires on
  entry, so nothing fired — a correct product, reported broken)
- reading `getComputedStyle(...).cursor` when the client deliberately renders
  an SVG glyph instead — a check that could only ever fail
- asserting on a fixture element after an earlier suite had navigated away
- a stray Chrome from an interrupted run holding the session's single client
  slot, so the next run got no video at all

The discriminating move is always the same, and it is cheap:

> **Inject the same interaction directly at the worker over CDP.** If it works
> there, Blink and the page are fine and the fault is in the chromeless input
> path. If it fails there too, look at the test.

That one step is what separated the real keyboard defect from the imaginary
hover one, and it is how the `dom_code` root cause was isolated: dispatch the
same `windowsVirtualKeyCode` with and without the `code`/`key` params and
compare.

## Layout

- `harness.py` — `CDP` (stdlib websocket), `WorkerOracle` (truth),
  `ClientDriver` (the user), fixture upload, login.
- `run.py` — the suites: video, navigation, mouse, scroll, keyboard, channels.

Input is dispatched as genuine DOM events on the client's `<video>` element,
never as synthetic CDP input at the client — that would bypass the very code
under test.

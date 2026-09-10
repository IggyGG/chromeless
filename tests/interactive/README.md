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

**A restart between runs is no longer required** on a current image: the
worker re-arms its peer connection when a viewer leaves
(`docs/findings/one-session-per-worker-process.md`, now resolved). It is still
the fastest way to get a known-clean guest, and the runner aborts rather than
cascading if the session ends mid-run:

```sh
kubectl rollout restart deploy/chromeless-standalone-worker -n chromeless
```

## Expected result

**65 of 65** was the score before 2026-09-08. The suite now carries **91
check() sites** — `uploads` is new (12) and `stats` grew from 6 to 13 (audio
reception, the unmute control, the HUD) — so the number a full run prints is
higher and depends on which optional preflights fire.

**Do not treat a bare total as the pass criterion.** The reliable statement is
*zero FAILED lines*; the run prints `N/N checks passed` and names every
failure. Suites skip whole blocks when a preflight says the guest or the
gateway bundle predates a feature, and that is reported, not silently counted.

On the pinned guest (`infra/k8s/standalone/stack.yaml`, `cr7727-c5f2eb91c6f0`
or later), with these checks reporting a preflight rather than a product
verdict:

- the clipboard suite expects paste AND copy to round-trip on a guest built
  after 2026-09-05 (boot log `CV2-CLIPBOARD: relay bound`). An older guest logs
  `CbClipboardRelay (WS disabled / url=off)` and fails the paste check. The
  copy check observes the WRITE the client bundle makes (it wraps
  `navigator.clipboard.writeText`) rather than reading the harness Chrome's
  clipboard back, which a headless Chrome refuses; its failure line carries
  `document.hasFocus()` because the bundle queues the write until the
  document is focused, and the harness enables focus emulation for exactly
  that reason.
- the passthrough and dialogs suites need the gateway bundle that carries the
  `control` consumer; the preflight names a stale gateway, a stale guest, or
  "cannot tell" explicitly.
- the download oracle lists the profile's `Downloads/` under
  `/home/cbuser/.config/chromium` (the guest honours `--user-data-dir` since
  the same image) and the older `/tmp/cloud_browser_profile_*` path, so it
  stays honest against an older guest.
- the permissions suite (added 2026-09-10 with batch C) asserts that a
  prompt REACHED the viewer, not that it was granted. The demo client uses
  `window.confirm()`, which headless Chrome auto-dismisses, so the outcome is
  always "denied" and says nothing. The load-bearing fact is that a prompt
  happened at all: before batch C, `GetPermissionControllerDelegate()`
  returned nullptr and //content denied everything WITHOUT asking, which the
  page saw as an ordinary `PERMISSION_DENIED`. Nothing looked broken, which
  is why it survived.

  Because `window.confirm()` blocks the client's JS thread, the prompt cannot
  be observed by CDP eval on that page; the oracle is the guest's own
  `CV2-PERMISSION` log line, read the same way the download oracle reads the
  filesystem.

- the uploads suite (added 2026-09-08 with `CbFileUploadReceiver`) needs a
  guest that opens BOTH the `files` and `control` channels. Its preflight
  reports the two separately, because the two halves of `<input type=file>`
  failed independently and for a while both were missing: no
  `RunFileChooser` override meant chromium auto-cancelled the chooser before
  anything was asked, and the `files` channel was bound to a relay whose
  WebSocket backend was never built. Either alone looks like "the file input
  does nothing".

  Its oracle is the REMOTE PAGE's own `<input>`, not the guest's disk: bytes
  landing in `Uploads/` prove only that the transport ran, while the defect
  was that the page's `change` event never fired. It also asserts `File.name`
  is the name the viewer picked — an empty `NativeFileInfo::display_name`
  makes blink fall back to our sanitised on-disk spelling, which every other
  check would pass right through.

**Any failure is a regression.** Earlier images score lower for reasons
recorded in `docs/findings/`; if the failures cluster in one suite, check the
worker's image tag before debugging anything.

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
- `run.py` — the suites: dialogs, uploads, downloads, permissions, clipboard,
  passthrough, stats, video, navigation, mouse, scroll, keyboard, channels.

Input is dispatched as genuine DOM events on the client's `<video>` element,
never as synthetic CDP input at the client — that would bypass the very code
under test.

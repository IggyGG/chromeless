# End-to-end tests (Playwright)

Functional E2E for the chromeless stack. Drives a real Chromium against the
gateway-served client, exercising signaling, ICE, the SDP round-trip and
media reception. Latency is **not** measured here — that lives in `harness/`
and `tests/harness/`. This suite is purely about correctness of behaviour.

> **Status:** specs 01–03 and 05–06 are active. The `test.skip` on 02 and 03
> pending **T34** ("flip T14 client to answerer role") is gone — T34 landed;
> the client is the answerer.
>
> **These specs did not execute in CI until 2026-08-19.** Every e2e job in
> this repo's history — 120 of them — self-skipped before reaching
> Playwright, and reported success. If you are relying on a green E2E check,
> read the "In CI" section below and make sure the run you are looking at
> actually ran.

## Layout

```
tests/e2e/
├── package.json                       # Playwright + TS deps
├── playwright.config.ts               # webServer + projects + base URL
├── fixtures/
│   └── audio-tone.html                # 440 Hz Web Audio fixture (T64)
├── auth.setup.ts                      # setup project — logs in, saves the cookie
├── 01-signaling-handshake.spec.ts     # peer connection reaches connected
├── 02-data-channel-open.spec.ts       # input data channel opens
├── 03-receives-video-track.spec.ts    # video track received AND decoding frames
├── 05-audio-receives.spec.ts          # audio bytesReceived > 0 (needs DevTools, see below)
├── 06-camera-passthrough.spec.ts      # client-side camera/mic passthrough (T81)
└── README.md
```

## How to run

```sh
cd tests/e2e
npm install                                  # one-time
npx playwright install --with-deps chromium  # one-time, downloads browser

# run the full suite
npm run test:e2e

# list specs without running them — fast structural sanity check
npm run test:e2e:list

# Playwright UI mode for local debugging
npm run test:e2e:ui

# tear down on demand
docker compose -f ../../infra/compose.yaml down
```

The default flow drives `docker compose -f infra/compose.yaml up --build`
under Playwright's `webServer` and waits for the **gateway** at
`https://localhost:8443`. To skip the compose management — for example when
you have already brought the stack up by hand, or are on a CI host that
pre-provisions it — set:

```sh
CHROMELESS_E2E_USE_RUNNING_STACK=1 npm run test:e2e

# or against a different URL entirely:
CHROMELESS_E2E_BASE_URL=https://my-host:8443 \
CHROMELESS_E2E_USE_RUNNING_STACK=1 \
  npm run test:e2e
```

Three things about the topology are worth knowing before debugging a failure
here:

- **One published port.** The stack exposes only the gateway (8443). The
  signaling broker (8080) and Chromium's DevTools (9222) used to be published
  and no longer are — 9222 is unauthenticated remote code execution against
  the browser. Reach either through `docker compose exec` instead.
- **HTTPS with a self-signed certificate.** The config sets
  `ignoreHTTPSErrors: true`; a suite that talks to the gateway without it
  fails every navigation on the TLS check.
- **Everything is behind a login.** `auth.setup.ts` runs first as a setup
  project, signs in, and saves the cookie as `storageState` for every spec —
  so the specs themselves never mention auth. The credentials default to
  `e2e` / `e2e-password` and are passed to compose by the same config, so the
  pair the gateway boots with cannot drift from the pair we sign in with.

Readiness is probed at `/healthz`, not `/`: every other route redirects to
`/login` without a session, and a 303 would let Playwright call the stack ready
before it is.

You can still point the suite at a hand-rolled stack, but note it must now
serve the page and the broker on ONE origin — the client derives its signaling
endpoint from `location` (`client/src/config.ts`), so a page on :5173 will dial
:5173 for the WebSocket. Running the gateway directly is the simplest way:

```sh
( cd signaling && go run . ) &
( cd client && npm install && npm run build ) &&
( cd infra/gateway && CHROMELESS_USER=e2e CHROMELESS_PASS=e2e-password \
    CHROMELESS_SIGNALING_URL=ws://127.0.0.1:8080 \
    CHROMELESS_STATIC_DIR=../../client/dist \
    CHROMELESS_TLS_DIR=/tmp/chromeless-certs go run . ) &

CHROMELESS_E2E_USE_RUNNING_STACK=1 npm run test:e2e
```

## Playwright cannot launch on an arm64 Mac with the pinned version

`@playwright/test` is pinned to 1.48.2, whose chromium-1140 build does not
launch here — `spawn Unknown system error -88` (EBADARCH), or a download that
lands with an invalid code signature. `npx playwright install` makes it worse
by resolving a NEWER playwright than the pinned one and fetching a build the
runner will not use.

That is a local tooling limitation, not a product one. The stack itself was
verified end to end by driving real Google Chrome over CDP instead — login,
connect, `framesDecoded` climbing at 1280x720, and a frame read off the live
`<video>` element showing the remote page. The specs here are correct and will
run wherever the pinned browser does (CI, Linux, an x86 Mac); they were checked
with `--list` to confirm collection and un-skipping.

If you need to run them locally on arm64, bump `@playwright/test` and re-pin
the browser, or run them from a machine where 1.48.2 works.

## Known dependencies

| Spec  | Runs when | Notes |
| ----- | --------- | ----- |
| 01    | always    | Peer connection reaches `connected`, signaling reaches `stable`, ICE reaches `connected`/`completed`. The client is the **answerer**, so `have-local-offer` and a client-driven ICE gathering `complete` are unreachable — an earlier version asserted both and could never have passed. |
| 02    | always    | Input data channel opens. Needs a real peer answering, which is why it was skipped pending T34; T34 landed. |
| 03    | always    | The load-bearing one: a video receiver **and** `framesDecoded > 0`. A receiver with zero frames is a documented failure mode in this system, so presence alone is not asserted. |
| 05    | `CHROMELESS_E2E_DEVTOOLS_URL` set | Injects a 440 Hz tone into the worker via CDP, then asserts inbound-rtp audio `bytesReceived > 0`. The stack publishes only 8443, so DevTools must be forwarded deliberately — `docker compose exec` or a port-forward. Absent that, the spec skips: a harness prerequisite, not a product failure. |
| 06    | always    | Client-side camera/mic passthrough (T81) with a fake device. Asserts senders and `request_renegotiate`; the cloud-side v4l2 sink is a manual smoke, documented in `docs/protocols/webcam-mic-passthrough.md`. |

Spec `04` served `capture/streamer-page/` and asserted `window.pc` on it. That
directory was deleted in the M7 migration and has no successor — the peer now
lives in the browser process, where no page-scoped `window.pc` exists. The spec
was removed rather than rewritten; there is nothing left for it to test.

(A note here used to describe the `client` nginx service serving a 404 on
`main.js` because it bind-mounted the source rather than the build. That
service is gone — the gateway serves `client/dist` directly, and the
`client-build` service still populates it.)

## How to add a test

1. Number the file: `NN-short-name.spec.ts`. The numeric prefix is
   advisory — Playwright runs alphabetically by default and we keep
   the suite serial (`fullyParallel: false`, `workers: 1`) since all
   specs share one cloud-browser session at a time.
2. Use `expect(...)` with explicit timeouts. Default is 5 s; bump to
   10–15 s for assertions that wait on signaling / ICE state.
3. If the test depends on an unfinished task, write the body anyway
   and `test.skip()` it with a comment block matching the format in
   `02-...spec.ts` (`why skipped`, `what to do when X lands`). For
   tests that we expect to start passing once a *committed* task
   lands without further intervention (T69-style), prefer ACTIVE
   tests with **failure messages naming the gating task** — see
   spec 05 — so a future test run that starts failing immediately
   tells the on-call where to look.
4. **Do not** write tests that require manual setup. Anything that
   needs a webcam, an external TURN server, or a pinned DNS record
   belongs in the harness layer or in a separate suite.

### Audio presence (T64) — companion fixture and integration test

Spec 05 (`05-audio-receives.spec.ts`) drives a real audio round-trip
end-to-end. Two related artifacts:

- **`fixtures/audio-tone.html`** — self-contained 440 Hz Web Audio
  fixture. Documents the canonical "minimum viable audio source"
  that the streamer can pick up via `getDisplayMedia({audio:true})`
  + PulseAudio null-sink (T24). Spec 05 doesn't load this file
  directly (loading it would replace the streamer page); instead
  it injects an equivalent inline AudioContext into the running
  streamer page via Playwright's `connectOverCDP`. The fixture
  exists for: (a) operator manual inspection, (b) future tests that
  may load it into a sibling tab once we add tab-based testing.

- **`../integration/audio_loopback_test.go`** — Go integration
  partner (also T64). Asserts the streamer's SDP **offer** advertises
  audio (m=audio + opus rtpmap) at the contract layer, without
  needing a browser. Opt-in via `CHROMELESS_INTEGRATION_LIVE=1` because
  it stands up `docker compose`. Together with spec 05 they cover
  the full vertical: *audio is offered* (Go test) → *audio is
  received* (Playwright spec).

Spec 05 depends on **T26** (cursor metadata sidecar) being well-
behaved alongside audio: cursor-watcher polls Chromium DevTools but
must NOT throttle, suspend, or replace the AudioContext. If audio
suddenly stops mid-spec, that's the first regression to chase.

## In CI

`.github/workflows/e2e.yml` runs this suite on every pull request and every
push to main. It is not part of `make test` — it needs Docker and a worker
image, neither of which a unit-test lane has.

**A green E2E check does not by itself mean the specs ran.** The job skips
itself, green, in two cases:

- the `CHROMELESS_IMAGE` repository variable is unset — there is no browser
  to test against, and this repo publishes no image;
- the docker daemon cannot resolve the job's paths, so compose bind-mounts
  would silently mount empty directories.

Both now emit `::warning::E2E DID NOT RUN`, because for four months they
emitted only a `::notice::` and 120 consecutive skips read as 120 passes.
The run also fails outright if Playwright exits without collecting a spec.

Two host-specific things the workflow handles, worth knowing if you port it:

- **Path alignment.** On this CI host the checkout lives in a docker volume;
  the job container sees `/workspace/<owner>/<repo>` and the daemon sees
  `/var/lib/docker/volumes/<id>/_data`. Same bytes, different paths — so
  compose is pointed at the daemon's path via a symlink rather than given
  one that means nothing on the other side.
- **Playwright runs in `mcr.microsoft.com/playwright`, not the job
  container.** The job image is Alpine; Playwright's browser is a glibc
  binary and dies with a confusing `ENOENT` on the loader. The image tag is
  pinned to match `package.json` — a browser newer than the client library
  refuses to start.

## See also

- Overall testing strategy: [`../README.md`](../README.md)
- Integration tests (signaling, no Docker): [`../integration/`](../integration/)
- Smoke tests (container boots): [`../smoke/`](../smoke/)
- Latency harness (orthogonal to E2E): [`../../harness/`](../../harness/)

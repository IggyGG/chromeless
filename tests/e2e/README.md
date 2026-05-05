# End-to-end tests (Playwright)

Functional E2E for the v0 chromeless stack. Drives a real
Chromium against the served client, exercising signaling, ICE, and
(once T34 lands) the full SDP round-trip + media reception. Latency is
**not** measured here — that lives in `harness/` and `tests/harness/`.
This suite is purely about correctness of behaviour.

> **Status:** Phase 0. Spec 01 is implemented and active; specs 02–03
> are scaffolded but `test.skip`'d pending **T34** ("flip T14 client to
> answerer role"). Comments in each `*.spec.ts` explain the un-skip
> conditions in detail.

## Layout

```
tests/e2e/
├── package.json                       # Playwright + TS deps
├── playwright.config.ts               # webServer + projects + base URL
├── fixtures/
│   └── audio-tone.html                # 440 Hz Web Audio fixture (T64)
├── 01-signaling-handshake.spec.ts     # ACTIVE — stub-state handshake
├── 02-data-channel-open.spec.ts       # SKIP (T34) — input DC opens
├── 03-receives-video-track.spec.ts    # SKIP (T34/T23) — video receiver
├── 05-audio-receives.spec.ts          # ACTIVE pending T78 — audio bytes_received > 0
└── README.md
```

## How to run

```sh
cd tests/e2e
npm install                                  # one-time
npx playwright install --with-deps chromium  # one-time, downloads browser

# run the full suite (currently: spec 01 active; 02 and 03 reported as skipped)
npm run test:e2e

# list specs without running them — fast structural sanity check
npm run test:e2e:list

# Playwright UI mode for local debugging
npm run test:e2e:ui

# tear down on demand
docker compose -f ../../infra/compose.yaml down
```

The default flow drives `docker compose -f infra/compose.yaml up --build`
under Playwright's `webServer` and waits for the client to be reachable
at `http://localhost:3000`. To skip the compose management — for
example, when you've already brought the stack up by hand or are
running on a CI host that pre-provisions the stack — set:

```sh
CHROMELESS_E2E_USE_RUNNING_STACK=1 npm run test:e2e

# or against a different URL entirely:
CHROMELESS_E2E_BASE_URL=http://my-host:3000 \
CHROMELESS_E2E_USE_RUNNING_STACK=1 \
  npm run test:e2e
```

You can also point the suite at a hand-rolled local stack
(`go run ./signaling`, `python3 -m http.server -d client/dist 5173`):

```sh
( cd signaling && go run . ) &
( cd client && npm install && npm run build && python3 -m http.server -d dist 5173 ) &

CHROMELESS_E2E_USE_RUNNING_STACK=1 \
CHROMELESS_E2E_BASE_URL=http://localhost:5173 \
  npm run test:e2e
```

## Known dependencies

| Spec  | Blocked by | Notes |
| ----- | ---------- | ----- |
| 01    | none       | Active. Asserts the documented stub state — passes today and continues to pass after T34 (regex covers both stub and connected states). |
| 02    | T34        | Data channel cannot open without a real peer answering. Un-skip when T34 lands. |
| 03    | T34, possibly more T23/T28 follow-up | Needs a connected PC + real media + a client-side `window.__cbwrtc_pc` test hook. |
| 04    | T78 (T69 + T52 already in) | Audio presence E2E (T64). Active code path; T69 (window.pc) and T52 (host DevTools) are already on main. As of authoring, gated by **T78** — getDisplayMedia inside the cloud Chromium fails with NotReadableError, so the streamer's start() throws before reaching `window.pc = pc`. Once T78 lands, this spec passes without further changes. Failure messages name T78 explicitly. |

Other follow-ups visible from this suite that are **not** mine:

- The `client` service in `infra/compose.yaml` bind-mounts `client/`
  but the page imports `./main.js`, which is built into `client/dist/`.
  As of this writing, opening `http://localhost:3000/` returns a 404
  on `main.js`. Spec 01 therefore fails against the canonical compose
  flow today; it does pass via the hand-rolled local stack described
  above. **Follow-up needed on T28/T31:** either bind-mount
  `client/dist/` after a build step, or update the index to import
  `./dist/main.js`. Filed as a follow-up via TaskCreate.

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

## CI hookup

Playwright tests are explicitly **not** in the default `make test` PR
gate (see [`tests/README.md`](../README.md#3-ci-plan)). The expected
shape:

- **`e2e.yml`** — runs on `merge to main` and nightly on a
  self-hosted Linux runner with Docker. Uses the docker-compose
  webServer flow.
- **PR opt-in** — a `ci:e2e` label on a PR triggers the same workflow
  for that PR, for changes in `client/`, `signaling/`, `capture/`, or
  `infra/`.

The current `T21` workflow (`.github/workflows/ci.yml`) covers
Dockerfile build + smoke test only. Adding `e2e.yml` is a follow-up
T21-style task — flag this when wiring CI for the next phase.

## See also

- Overall testing strategy: [`../README.md`](../README.md)
- Integration tests (signaling, no Docker): [`../integration/`](../integration/)
- Smoke tests (container boots): [`../smoke/`](../smoke/)
- Latency harness (orthogonal to E2E): [`../../harness/`](../../harness/)

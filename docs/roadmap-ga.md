# Roadmap to GA — the standalone browser host

**Status:** the plan of record as of `9d057cc` (2026-09-05). Written after a
deep dive over `origin/main`; every claim cites the file it was read from.
Revise this document when a milestone closes, not when a task does.

**GA, defined:** a person with Docker on a Linux amd64 machine runs one
documented command, opens one URL, and gets a browser that looks and behaves
like a browser — sound, resize, tabs, clipboard, uploads, downloads, dialogs —
that stays up for days, and that maintainers verify from CI on every change.

## Decisions already taken

| Decision | Choice | Consequence |
| --- | --- | --- |
| Platforms | **Linux amd64 at GA.** arm64 tracked after | one build lane, not two; Apple Silicon hosts run the worker on a Linux box or cluster |
| Public worker image | **Publish the full image, x264 included**, with a license review and third-party notices | the live guest links `libx264.so.164` (GPLv2+); the published image carries GPL terms and a notices file |
| Browser shape | **Tabs yes, multi-viewer no** | popups become visible and switchable; a second simultaneous viewer stays out of scope |

## What is already right

Keep these as they are.

- **The security shape.** One published port; TLS at the gateway; login → opaque
  server-side cookie (`Secure`/`HttpOnly`/`SameSite=Lax`, lockout); gateway-minted
  Ed25519 tokens the broker verifies; DevTools and the broker internal-only;
  URL-shaped navigation verbs behind an `http`/`https` allowlist
  (`infra/gateway/cdp.go`, `validateNavigationURL`); relay denies private ranges;
  required credentials with no default. `peer_absent` and the offer watchdog make a
  misconfiguration loud in seconds.
- **The browser outlives its viewers.** `RearmSession` in
  `capture/build-integration/cloud_browser_browser_main_parts.cc` rebuilds the peer
  connection in place; the chromium pid stayed constant across a dozen
  connect/disconnect cycles (`tests/local/README.md`).
- **Real fidelity, in one wave.** `CbWebContentsDelegate`: popups adopted as real
  tabs so OAuth `postMessage` works, JS dialogs routed to a human over the `control`
  channel with safe defaults, downloads that land on disk, drag-and-drop dispatched,
  renderer-crash recovery with a bounded budget, fail-fast when no video track can
  exist, a viewport controller, the resize crash fixed in all five encoders.
- **Input verified by an independent oracle.** `tests/interactive/` reads
  consequences off the worker's own DevTools (58 checks on a live stack);
  `tests/local/` drives the real Chrome a user has.
- **Provenance.** `build/guest-release.json` and `infra/k8s/standalone/stack.yaml`
  pin by digest; `make lint` guards the recurring failure classes (targets nothing
  builds, tests nothing runs, includes nothing declares, pods nothing can schedule).
- **Deliberate video defaults** (VP9 first, `MAINTAIN_RESOLUTION`, 6 Mbps ceiling,
  1.5 Mbps floor, 30 fps) and **honest docs** (`docs/README.md` says which documents
  are history; `docs/operations/standalone.md` says what the stack is not).

## Where it falls short

Ranked by what a first-time user hits first.

### The page is a debugger

- `client/index.html` titles itself "v0 client"; a permanent 380 px panel of
  `signaling/ice/ice-gather/connection/data-channel` and a raw event log is the
  whole post-login UI. No responsive rules, no light theme, no favicon, no empty
  state, no help.
- **Audio is muted forever.** The `<video>` is `autoplay playsinline muted` and
  nothing unmutes it. The guest sends an audio track that is discarded.
- **Resize does nothing.** `Cb.setViewport` exists (`docs/protocols/cb-cdp-methods.md`)
  and nothing in `client/` or `infra/gateway/` calls it; the remote stays 1280×720.
- **No fullscreen, page title, loading state, stop button, tab strip, zoom.**
- **Keys are captured globally.** `client/src/input.ts` listens on `window`, so
  Cmd/Ctrl+L, T, W, R fire locally *and* remotely, and typing in the URL bar streams
  to the guest.
- **Paste is sent twice and consumed zero times.** `input.ts` sends `clipboard_paste`
  on the input channel and `main.ts` sends `clipboard_offer` on the clipboard channel;
  the guest declines the first (`cb_input_dispatch_clipboard.h`) and stubs the second.
- File upload has no UI; stats and codec outcome are emitted by `ChromelessSession`
  and rendered nowhere; `TokenRefresher` (`client/src/auth.ts`) has no caller, so a
  15-minute signaling token under a 12-hour cookie is a latent reconnect failure.

### Ordinary sites break

- `<input type=file>` does nothing: no `RunFileChooser` override (the API is pinned
  in `docs/build/chromium-7727-api-pins.md`, unused).
- **Clipboard and file-upload guest halves are draft stubs** built around a
  WebSocket bridge that never existed: `cb_clipboard_relay.cc` "pretend-sends",
  `cb_file_upload_relay.cc` is constructed with `url=off`, and the boot log says
  "file transfer is INERT". `tests/interactive/run.py` records the paste gap
  explicitly.
- **Downloads never reach the viewer.** `CbDownloadManagerDelegate` writes under the
  profile's `Downloads/` inside the guest, and that is where they stay.
- **Popups are adopted but invisible, and cannot close.** Capture deliberately does
  not retarget (`cb_web_contents_delegate.cc`), there is no tab list or switch
  surface, and there is no `CloseContents` override, so `window.close()` is a no-op.
  Closing the captured tab would strand the capturer and trip the 30 s
  self-termination.
- **Re-arm strands in-flight dialogs.** `RearmSession` resets `dc_host_` without
  `CancelAllPending()`, then `RebuildNativeDataChannels` replaces `control_channel_`;
  the old channel's destructor only logs. A viewer leaving mid-`confirm()` leaves the
  page blocked for the next viewer.
- Context menu suppressed; permission prompts (geolocation, notifications) have no
  path to the user; HTTP auth has no `LoginDelegate`; certificate errors show
  nothing; HiDPI is clamped to a scale of 1.0; a keyframe on navigation is a recorded
  TODO; WebGL is SwiftShader; PDFs have no viewer.

### Session and state

- The profile lives under `base::DIR_TEMP` (`cloud_browser_browser_context.cc`), so
  the `--user-data-dir` the launcher passes is ignored and **a volume alone persists
  nothing**. Gateway sessions are in memory. One tab is streamed to one viewer.

### Distribution

- **No publicly pullable worker image.** `build/guest-release.json` names a private
  registry; `release.yml` publishes only the Go images, only on `v*` tags, and no tag
  exists. An outsider needs a 4–8 h Chromium build on a ≥64 GiB amd64 box.
- **Lane throughput is the critical path.** Every `capture/` change waits on one
  hostPath Job that is routinely unable to schedule
  (`docs/findings/build-node-cpu-reservations.md`).
- `keygen` needs a Go toolchain on the host; the gateway and broker images build from
  source at `compose up`; the `turn` profile is a no-op on Docker Desktop.

### Verification

- `make verify` (~15 s) is all a laptop runs. `tests/local` and `tests/interactive`
  are wired to no CI lane. The e2e lane ran zero times for four months before
  2026-08-19 and now runs on a borrowed runner pool with a destructive garbage
  collector. No visual regression, no latency budget, no soak, no Firefox/Safari/
  mobile matrix. The audio spec skips unless DevTools is forwarded.

### Operability, hardening, docs

- No gateway `/metrics`, no aggregated status endpoint; worker logs are not in
  `docker logs`. Compose applies no seccomp profile or capability drops to the
  `chromium` service; `--no-sandbox`, `--remote-allow-origins=*` and
  `--use-fake-ui-for-media-stream` are always on.
- No `SECURITY.md`, `CHANGELOG`, issue/PR templates or `CODEOWNERS`.

## The plan

Six tracks. C++ items are lane-bound and travel in four batches (Track 2).
Everything else is laptop-verifiable and runs in parallel.

### Track 1 — Product client (`client/`)

1. **Shell.** Toolbar (back/forward/reload/stop, URL with loading state, page title
   from an extended `/api/current-url`), debug panel behind `?debug=1`, responsive
   layout, light/dark, `<title>`, favicon, empty state, reconnect banner. New
   `client/src/ui/*`; reuse `ChromelessSession` events, `navigate.ts`, the
   `control.ts` presenter seam.
2. **Audio.** Unmute/volume; autoplay-policy handling; level indicator from stats.
3. **Viewport follows the window.** `ResizeObserver` → debounced `POST /api/viewport`
   → gateway `Cb.setViewport` → apply the echoed size. Scale stays 1.0 until Batch D.
4. **Fullscreen** button and shortcut; honour the guest's `fullscreen_changed`.
5. **Keyboard model.** Capture only while the stage has focus; prevent browser
   shortcuts while captured; an explicit release key with a visible state; never
   forward keys typed into the URL bar. Drop the duplicate paste on the input channel.
6. **HUD** from `stats`/`codecOutcome`: fps, bitrate, RTT, codec, quality dot.
7. **Upload and clipboard affordances**; `control.ts` becomes kind-aware
   (`file_chooser`, `download`, `permission`, `login`, `cert_error`, `context_menu`).
8. **Token refresh**: wire `TokenRefresher` into `ChromelessSession`.

### Track 2 — Browser fidelity (`capture/`, lane-bound)

Per item: grep in-tree precedent, pin new symbols in
`docs/build/chromium-7727-api-pins.md`, `make lint-cxx`, one `--keep-going` lane run
per batch, re-verify with `tests/interactive` on the new image, bump
`guest-release.json`. Say "unverified" until the lane has run.

**Batch A — small, high value.**
- Cancel in-flight control requests on re-arm (`CancelAllPending()` and
  `SetSessionContext(aura, nullptr)` before `dc_host_.reset()`).
- `CloseContents`; when the captured WebContents dies, re-arm capture onto the
  initial one (precedent `CV2-CAPTURE-REARM`), so `window.close()` works.
- `tab_opened`/`tab_closed` control events (hints; `/json` is truth).
- Keyframe on retarget and navigation (`RtpSenderInterface::GenerateKeyFrame`).
- Honour `--user-data-dir` in `cloud_browser_browser_context.cc`.
- **Clipboard both ways, directly on the data channel; delete the WebSocket
  classes.** Inbound `clipboard_offer` → `ui::ScopedClipboardWriter` on the UI runner
  → synthesise Ctrl+V against the active WebContents (clone
  `CbInputDispatchClipboard::DispatchCopy` with VKEY_V). Outbound via a
  `ui::ClipboardObserver`, gated to a short window after `clipboard_copy_request` or a
  guest Ctrl+C, echo-suppressed. Flip the known-gap check in `tests/interactive/run.py`.

**Batch B — file chooser and upload.** `RunFileChooser` →
`control_channel_->SendRequest("file_chooser", …)` → client picker → the existing
`FileUploadChannel` chunking with a `chooser_id` → a real receiver replaces
`cb_file_upload_relay.cc` (base64 precedent `capture/cursor/cb_custom_cursor_image.cc`,
blocking IO as in `cb_download_manager_delegate.cc`) → `<profile>/Uploads/<id>__<name>`
→ `FileSelectListener::FileSelected` exactly once. Paths never come from the wire;
`Uploads/` is wiped on re-arm; per-session byte cap.

**Batch C — control kinds.** `download` (a `DownloadItem::Observer`), `permission`
(`PermissionControllerDelegate`), `login` (`CreateLoginDelegate`), `cert_error`
(`AllowCertificateError`), `context_menu` (forward `ContextMenuParams`; client actions
map to open, open in new tab, copy link, copy, paste, save image).

**Batch D — HiDPI.** Normalise input to DIP in
`CbInputDispatchCompositeDelegate::OnInputEvent`, unclamp `deviceScaleFactor`, have
the client send `devicePixelRatio`; run the input suites at scale 1 and 2.

Also: the audio `PrepareForTeardown` hook on an inbound bye; IME/dead-key and touch
checks added to `tests/interactive`; **wire the R7 reconnect supervisor**
(`cb_signaling_reconnect`, built and never constructed) into `StartNativeSession`
so a broker restart does not cost the worker its process
(`docs/findings/worker-signaling-no-redial.md`) — then the liveness probe in
`stack.yaml` that stands in for it can go.

### Track 3 — Downloads, tabs, session state

1. **Downloads to the viewer via a sidecar.** A `downloads-server` program under
   supervisord (python3 is in the image) serving the profile's `Downloads/` read-only
   by basename on the container IP; gateway `GET /api/downloads` and
   `GET /api/downloads/{name}` behind the session cookie, delete-after-serve; a
   downloads tray fed by the Batch C `download` event. A data-channel relay with
   backpressure comes after GA.
2. **Tabs, gateway-side, no new `Cb.*` method.** `cdp.go` gains an active target id
   that `pageTarget()` and `currentURL` honour; `GET /api/tabs` from `/json`;
   activate = dial that target's socket and call `Cb.startFrameSinkCapture`, which is
   page-scoped and retargets the capturer (`capturer.cc`, `ChangeTarget`); new tab via
   `/json/new` (`CreateNewTarget` exists); close via `/json/close/{id}`. Client tab
   strip; auto-switch to a new popup; restore on close. Depends on Batch A.
3. **Profile persistence**: an opt-in named volume once `--user-data-dir` is honoured;
   document cookies at rest.
4. Gateway sessions file-backed under `/data` (or documented as ephemeral); an idle
   timeout that blanks and locks; an explicit "end session" that resets the profile.

### Track 4 — Distribution and installation

1. **Publish the pinned digest publicly (M0).** Copy `guest-release.json`'s digest to
   a public registry, sign it, attach an SBOM; compose uses `image:` plus digest with
   `build:` as the developer override. Same PR: third-party notices in the image and
   the repo (x264, libvpx, Chromium), an explicit statement of the GPL terms the
   published worker image carries, and a review of `proprietary_codecs` and
   `ffmpeg_branding` in `capture/build-integration/args.gn`.
2. **Reproducible outsider build**: `build/Dockerfile.build` with a documented
   `docker run` recipe and sccache. A contributor concern, not a GA blocker.
3. **One-command install.** `chromeless up`: checks Docker, writes `.env` (keys,
   worker token, password), pulls pinned images, waits for health, prints the URL and
   credentials. Ship `keygen` and `worker-token` in the gateway image so no Go
   toolchain is needed on the host.
4. Prebuilt gateway and signaling images from `release.yml`, extended to the worker
   on `v*`; TURN on Docker Desktop via a narrow mapped port range or a documented
   external relay; NAT detection in `chromeless up`.
5. arm64 worker after GA. Real semver and a `CHANGELOG.md`.

### Track 5 — Verification: green means green

1. A dedicated runner pool, or runners large enough that the garbage collector never
   arms mid-job.
2. e2e on every PR against the pinned guest; `tests/local` and `tests/interactive`
   nightly against a compose stack on a Linux runner (port the download oracle from
   `kubectl exec` to `docker exec`); the audio spec with DevTools forwarded in-job.
3. Merge gate for `capture/`: lane green, `guest-release.json` bumped, and the
   printed `N/N` from `tests/interactive --only <suites>` in the PR body.
   `Cb.getCaptureStats` is liveness only, never a feature check.
4. Visual regression through the stream, a glass-to-glass latency budget via
   `harness/`, a 24 h soak, a client matrix (Chrome, Firefox, Safari, iOS Safari,
   Android Chrome) for connect, decode and input.

### Track 6 — Operability, hardening, docs

1. Gateway `/metrics` and `/api/status` (gateway, broker, worker DevTools,
   `Cb.getCaptureStats`); worker logs to stdout.
2. Compose hardening: `infra/seccomp/chromeless.json`, `cap_drop: [ALL]`,
   `no-new-privileges`, a narrow `--remote-allow-origins`, rate limits on `/login`
   and `/api/*`, a CSP on the client page, a "public internet" threat model; the
   Chromium sandbox via user namespaces where the host allows.
3. `SECURITY.md`, `CHANGELOG.md`, issue and PR templates, `CODEOWNERS`; one "start
   here" per audience (operator, integrator, contributor).

## Milestones

| Milestone | Contents | Exit criterion |
| --- | --- | --- |
| **M0 — Trustworthy now** | this document; doc reconciliation; public digest + notices; `chromeless up` skeleton with keygen in the image; runner-pool decision; **Batch A fired** | a fresh Linux box: one command → a browser; CI green on a dedicated pool; the Batch A image pinned |
| **M1 — Looks like a browser** | Track 1; downloads sidecar | `tests/local` user path passes with a visual snapshot; audio audible; resize follows; keys do not leak; a download reaches the viewer |
| **M2 — Behaves like a browser** | Batches B and C; tabs; profile persistence; sessions | `tests/interactive` covers clipboard, upload, downloads, tabs, dialogs, context menu, green on the pinned image |
| **M3 — Anyone can run it** | Track 4.2–4.4; Track 6.1–6.2; Track 5.2–5.3 | published images pulled by digest; a 7-day soak; hardening on by default |
| **M4 — GA** | Batch D; Track 5.4; Track 6.3; `v1.0.0` | `release.yml` publishes every image; SECURITY and CHANGELOG present; browser matrix and latency budget green |

**Critical path:** lane throughput first (Track 5.1 and Batch A on day one), then
Batch B. Copying the public image is an afternoon, not the long pole.

## How this document is verified

Each milestone's exit criterion names the suite that proves it: `make verify` for
everything laptop-side, `tests/local` for the user path, `tests/interactive` for
input and channels, the lane plus a `guest-release.json` bump for anything in
`capture/`. A milestone is closed by the printed result of that suite in the closing
PR, never by the checklist above.

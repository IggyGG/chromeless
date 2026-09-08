# chromeless

[![GitHub](https://img.shields.io/badge/source-IggyGG%2Fchromeless-181717?logo=github)](https://github.com/IggyGG/chromeless)
[![CI](https://forgejo.triform.dev/triform/chromeless/actions/workflows/ci.yml/badge.svg?branch=main)](https://forgejo.triform.dev/triform/chromeless/actions)
[![E2E](https://forgejo.triform.dev/triform/chromeless/actions/workflows/e2e.yml/badge.svg?branch=main)](https://forgejo.triform.dev/triform/chromeless/actions)
[![Harness](https://forgejo.triform.dev/triform/chromeless/actions/workflows/harness-loopback.yml/badge.svg?branch=main)](https://forgejo.triform.dev/triform/chromeless/actions)
[![CodeQL](https://forgejo.triform.dev/triform/chromeless/actions/workflows/codeql.yml/badge.svg?branch=main)](https://forgejo.triform.dev/triform/chromeless/actions)
[![Release](https://forgejo.triform.dev/triform/chromeless/actions/workflows/release.yml/badge.svg?branch=main)](https://forgejo.triform.dev/triform/chromeless/actions)

**A Chromium embedder that streams a real browser over WebRTC, with a native
libwebrtc peer running inside the browser process.**

A real Chromium runs on a Linux server, its tab is captured straight out of the
Viz compositor via `FrameSinkVideoCapturer`, encoded (x264 / VP9 / NVENC /
VAAPI / SVT-AV1), and streamed to a client. Mouse, keyboard, clipboard, file
uploads, cursor updates, and audio round-trip through the same peer connection.
Useful for browser isolation, agent tooling, embedded co-browsing, and anywhere
you need a real Chromium you don't want to run locally.

**Status: Apache-2.0 open source, in production.** This is not a packaged product.
There are no published container images, no npm package, and no release tags —
you build it yourself. See [Building](#building) for what that costs.
[`docs/roadmap-ga.md`](./docs/roadmap-ga.md) is the plan of record for closing
that gap: what already works, what a first-time user hits, and the order of work.

The canonical public source is
[github.com/IggyGG/chromeless](https://github.com/IggyGG/chromeless). Triform's
Forgejo repository remains the protected CI and integration surface used by
maintainers; reviewed `main` history is published to GitHub for users and
external contributors.

---

## What you actually get

| Component | Language | What it is |
| --- | --- | --- |
| `capture/` | C++ | The product. A Chromium content-embedder (`cloud_browser_worker`) built out-of-tree against a pinned Chromium release branch. Native browser-process libwebrtc peer, FrameSink capture, encoder factory, input dispatch, five data channels, a custom `Cb.*` CDP domain. |
| `signaling/` | Go | WebSocket signaling broker. One session = one `client` + one `browser`. JWT auth, TURN credential minting, offer/ICE replay buffers. |
| `client/` | TypeScript | Browser-side client: answerer state machine, input encoder, cursor/clipboard/file-upload/stats channels, codec negotiation, reconnect. Tested with vitest. |
| `infra/` | Go/YAML | The standalone gateway (TLS, login, session tokens, navigation), compose stack, Helm chart, `BrowserSession` CRDs + controller, TURN issuer. |
| `harness/` | mixed | Glass-to-glass latency measurement (flashing block + webcam reconciliation). |

The browser is **always the offerer**; the client is the answerer.

---

## Quickstart

You need a `cloud_browser_worker` image. Nothing here publishes one, so either
build it (below) or use one your organization already built.

```bash
git clone https://github.com/IggyGG/chromeless.git && cd chromeless

CHROMELESS_IMAGE=my-registry/chromeless:cr7727-abc1234 make standalone-up
```

That needs Docker and nothing else. It builds the gateway image, mints the
keypair the gateway signs with and the broker verifies against plus the token
the browser presents to the broker, generates a login, writes all of it to
`infra/.env` once, starts the stack, and prints the URL and credentials. Open
<https://localhost:8443>, accept the certificate once, sign in, and type a URL:
you are driving a real Chromium. `make standalone-down` stops it and keeps the
credentials; delete `infra/.env` to rotate them.

The same thing by hand, if you would rather see the parts:

```bash
eval "$(docker run --rm --entrypoint /keygen chromeless-gateway:dev)"   # or: cd infra/gateway && go run ./cmd/keygen
CHROMELESS_IMAGE=my-registry/chromeless:cr7727-abc1234 \
CHROMELESS_USER=me CHROMELESS_PASS=hunter2 \
  docker compose -f infra/compose.yaml up
```

All three of keygen's exports are needed: with the broker's key set and the
worker's token missing, the broker refuses the browser and nothing but a
WebSocket close code says so. `CHROMELESS_IMAGE` has no default because this
repo publishes no images and there is nothing honest to point at;
`CHROMELESS_USER` / `CHROMELESS_PASS` have none because a built-in password is
worse than no login at all — it looks like protection. Compose fails fast on
each with a message.

**About that certificate warning.** The gateway generates a self-signed
certificate on first boot and reuses it afterwards. Accepting it once also
covers the `wss://` signaling connection, because the page and the WebSocket
share one origin — which is the reason everything runs on a single port. Bring
your own with `CHROMELESS_TLS_CERT` / `CHROMELESS_TLS_KEY`, and add
`CHROMELESS_TLS_HOSTS=hostname,192.168.1.10` if you reach it by anything other
than `localhost`.

Exactly one port is published: the gateway. The signaling broker and Chromium's
DevTools stay on the internal network — DevTools in particular is
unauthenticated remote code execution against the browser, and it used to be
published by default.

Without `CHROMELESS_ICE_SERVERS` the peer falls back to public STUN. Fine for a
same-host run and **will not traverse most NATs**; for anything else bring up
the bundled relay:

```bash
CHROMELESS_TURN_SECRET=$(openssl rand -hex 32) \
  docker compose -f infra/compose.yaml --profile turn up
```

**Running the browser on another machine** — a box with more cores, or a server
you already have — is `infra/compose.host.yaml` plus `infra/compose.worker.yaml`.
See [`docs/operations/standalone.md`](./docs/operations/standalone.md).

---

## Building

The browser is a from-source Chromium build. There is no smaller path:

- **WebGL is software-rendered.** v1's launch flag set pins Chromium
  to ANGLE + SwiftShader (T78 / T91 — Vulkan is disabled because
  Xvfb has no Vulkan driver). Simple WebGL pages render correctly;
  heavy WebGL (Three.js stress scenes, full-frame post-processing)
  will drop below ~5 fps and feel choppy. Phase 4's GPU passthrough
  unblocks hardware WebGL. See [`docs/research/rendering-matrix.md`](./docs/research/rendering-matrix.md).
- **WebGPU is not supported.** Dawn requires Vulkan on Linux; v1
  disables Vulkan. `navigator.gpu` is present, but `requestAdapter()`
  returns null. Same Phase 4 GPU-passthrough fix unblocks it.
- **Dev compose ships synthetic media.** `infra/compose.yaml`
  defaults `CHROMELESS_USE_FAKE_MEDIA=1`, which routes `getDisplayMedia`
  through Chromium 147's synthetic test pattern + tone instead of
  real screen capture. Real `getDisplayMedia` fails on Chromium 147
  + Xvfb regardless of launch-flag tuning (see
  [`docs/capture/path-of-least-resistance.md`](./docs/capture/path-of-least-resistance.md)
  §3a). Phase 2's
  `FrameSinkVideoCapturer` (T47, T55) replaces the
  `getDisplayMedia` path entirely and is the durable fix.
- **Single tab per session, single session per container.** v1
  intentionally ships one Chromium per user; multi-tenant
  orchestration is Phase 3.

- **4–8 hours cold**, ~1 hour warm with a populated sccache.
- Needs a full `gclient sync` of the Chromium tree plus a many-core builder.
- Produces `cloud_browser_worker` + a runtime image via
  [`build/Dockerfile.runtime`](./build/Dockerfile.runtime).

```bash
build/chromeless-build.sh          # see build/README.md for the 10 steps and knobs
```

Chromium is pinned to a release branch (currently `refs/branch-heads/7727`,
≈M147) and rolled roughly every four weeks. Images are tagged
`cr<chromium-branch>-<repo-sha>`, so the tag answers "which Chromium is in
here". Pinning policy and the roll procedure:
[`docs/build/chromium-from-source.md`](./docs/build/chromium-from-source.md) §6.

The Go services and the TypeScript client build normally and need none of the
above:

```bash
( cd signaling && go build ./... && go test ./... )
( cd client && npm ci && npm test )
```

---

## Architecture

```
        ┌──────────── User's machine ────────────┐
        │  Browser client — client/              │
        │  RTCPeerConnection (answerer)          │
        │  recv video+audio · send input over DC │
        │  address bar ─── POST /api/navigate ─┐ │
        └───────┬───────────────────────▲──────┼─┘
                │ WebSocket (SDP/ICE)   │ SRTP/DTLS media + DataChannels
                ▼                       │      │
   ╔════════════════════════════════════╪══════╪═══╗
   ║  infra/gateway/  (Go)   TLS ends here      │  ║  ← the only published
   ║  login · session tokens · static · proxy   │  ║     port, standalone
   ╚════════┬═══════════════════════════╪══════╪═══╝
        ┌───▼──────────────────┐        │      │ CDP Page.navigate
        │  signaling/  (Go)    │        │      │
        │  offer + ICE replay  │        │      │
        └───────┬──────────────┘        │      │
                │                       │      │
┌───────────────▼───────────────────────┼──────────────────────────────┐
│  cloud_browser_worker  (capture/)     │                              │
│                                       │                              │
│   Chromium browser process            │                              │
│     ├─ Viz compositor                 │                              │
│     │    └─ FrameSinkVideoCapturer ──▶ video track source ──┐        │
│     ├─ native libwebrtc PeerConnection ◀────────────────────┘        │
│     │    └─ encoder factory: x264 / VP9 / NVENC / VAAPI / SVT-AV1 ───┤
│     ├─ DataChannels: input · stats · cursor · clipboard · files      │
│     └─ CDP: Page.navigate ◀──────────────────────────────────────────┘
│            Cb.startNativeSession · Cb.startFrameSinkCapture · …      │
└──────────────────────────────────────────────────────────────────────┘
```

The gateway is the standalone deployment's single front door; Kubernetes
deployments terminate TLS at an Ingress and mint sessions in the controller
instead, and skip it. **Navigation does not ride the peer connection**: the
connection carries pixels and input, and the URL goes over HTTP to a control
plane that drives CDP. The triform portal works the same way, with physics in
the gateway's place.

The peer lives in the **browser process**, not in a page. There is no streamer
web page and no `getDisplayMedia` — both were removed in the M7 native-peer
migration.

**Wire protocol.** `{type, from, data}` with six tags: `offer`, `answer`,
`ice`, `bye`, `request_renegotiate`, `probe_result`. `ice` data is an
`RTCIceCandidateInit` or `null` (end-of-candidates); `bye` omits `data`
entirely. The authoritative statement is
[`capture/signaling/cb_wire_envelope.h`](./capture/signaling/cb_wire_envelope.h).

**Channel protocols** — one spec per channel, each independently versioned —
live in [`docs/protocols/`](./docs/protocols/).

---

## Configuration

The worker is configured by environment. `infra/launch-chromeless.sh`
translates the friendly names into what the browser process reads:

| Set this | Becomes | Meaning |
| --- | --- | --- |
| `SIGNALING_URL` | `WEBRTC_SIGNALING_HOST` + `_TLS` | `ws[s]://host[:port]`. Path is ignored — the peer builds its own from the session id. |
| `SESSION_ID` | `WEBRTC_SIGNALING_SESSION_ID` | Opaque string. Must survive percent-encoding as one path segment. |
| `SIGNALING_TOKEN` | `WEBRTC_SIGNALING_TOKEN` | The browser's own session token, forwarded verbatim as a query param. Optional **only** while the broker has no `CHROMELESS_AUTH_PUBKEY`; the moment it has one this is required, and omitting it gets the worker closed with `1008 missing token`. `cmd/keygen` prints one. |
| `CHROMELESS_ICE_SERVERS` | `WEBRTC_ICE_SERVERS` | JSON array of `{urls, username, credential}`, or an object with an `iceServers` array. |
| `CHROMELESS_ICE_TRANSPORT_POLICY` | `WEBRTC_ICE_TRANSPORT_POLICY` | `all` (default) or `relay`. |

Setting a `WEBRTC_*` variable directly always wins, which is how the Kubernetes
controller drives it.

Sessions can also be started at runtime over CDP — `Cb.startNativeSession`
takes the same settings as parameters, which is what a warm-pool orchestrator
uses after restoring a snapshot. (This previously documented a
`Cb.startSession` with `Cb.startNativeSession` as its "deprecated alias". It
is the other way round: `startNativeSession` is the only spelling the embedder
implements — see `kStartNativeSessionMethod` in
`capture/build-integration/cb_devtools_agent.cc`. Since `Cb.*` is
hand-dispatched and absent from `/json/protocol`, nothing would have caught
the wrong name but a failing call.)

---

## Deploying

- **Kubernetes** — the chart at [`infra/helm/chromeless/`](./infra/helm/chromeless)
  ships `BrowserSession` / `BrowserSessionPool` CRDs and a controller that
  translates annotations into pod env. Point `images.*` at your own registry;
  nothing is published to a public one. Required values: the Ed25519 auth
  pubkey, the TURN shared secret, and TURN URLs.
- **Firecracker / warm snapshots** — boot the worker as a CDP-only target and
  drive `Cb.startNativeSession` over CDP once the microVM is restored. Note
  that "no signaling env" is **not** how you get there: `cold-start.sh`
  defaults `SIGNALING_URL` to `ws://signaling:8080/ws`, so an unconfigured
  worker dials a host that usually does not resolve, fails the handshake, and
  the process exits — supervisord then restart-loops it every ~30 s while
  DevTools still answers, which looks like a healthy browser. Set
  `SIGNALING_URL=""` explicitly.
- **Standalone / bare docker** — `infra/compose.yaml` plus
  [`infra/gateway/`](./infra/gateway/): one TLS port, a login, and everything
  else on an internal network. Split across two machines with
  `infra/compose.host.yaml` + `infra/compose.worker.yaml`. Full guide:
  [`docs/operations/standalone.md`](./docs/operations/standalone.md).

Day-2 material is in [`docs/operations/`](./docs/operations/);
`triform-deploy.md` there documents one real production cluster and is useful
as a worked example, not as a generic guide.

---

## Known limitations

- **WebGL is software-rendered** (ANGLE + SwiftShader; Vulkan is off because
  Xvfb has no Vulkan driver). Simple scenes are fine; heavy WebGL drops below
  ~5 fps. See [`docs/research/rendering-matrix.md`](./docs/research/rendering-matrix.md).
- **WebGPU is unsupported.** `navigator.gpu` exists but `requestAdapter()`
  returns null — Dawn needs Vulkan.
- **One tab streamed, one viewer at a time.** Popups open as real tabs but the
  stream stays on the opener and nothing switches it yet; a second simultaneous
  viewer is not supported. Between viewers the worker re-arms its peer
  connection in place, so the browser keeps its state across a reload or a
  reconnect (`RearmSession`; falls back to a process exit only if re-arm fails).
- **`Cb.*` is not in `/json/protocol`.** The domain is hand-dispatched, so
  protocol-introspecting CDP clients won't discover it.
- **Hardware encoders are compile-time.** NVENC/VAAPI/SVT-AV1 availability is
  fixed by the build variant; runtime config selects among what was compiled in.

---

## Latency

Latency is the design constraint: **<100 ms glass-to-glass on LAN, <200 ms
regional**. It is measured, not asserted — [`harness/`](./harness/) points a
webcam at the client screen, emits timestamped color transitions, and recovers
end-to-end latency from the captured video. Method:
[`harness/latency/README.md`](./harness/latency/README.md). Full acceptance
criteria: [`docs/v1-success-criteria.md`](./docs/v1-success-criteria.md).

---

## Prior art

We owe [Selkies](./docs/prior-art/selkies.md),
[Neko](./docs/prior-art/neko.md), and [Kasm](./docs/prior-art/kasm.md)
significant prior art; each solves an adjacent problem. Selkies is
GStreamer-based desktop streaming, not browser-native. Neko is a multi-user
"watch together" room around a shared browser. Kasm is a polished commercial
product built on VNC/KasmVNC.

chromeless is WebRTC-first, one-Chromium-per-user, capture-from-compositor, and
latency-obsessed. If that overlaps with what you need, it may be useful to you.

---

## Repo layout

| Path | Contents |
| --- | --- |
| [`capture/`](./capture/) | The Chromium embedder: browser-process WebRTC peer, FrameSink capture, encoders, input dispatch, `Cb.*` CDP domain. |
| [`signaling/`](./signaling/) | Go WebSocket signaling broker. |
| [`client/`](./client/) | TypeScript browser client + demo page. |
| [`infra/`](./infra/) | [gateway](./infra/gateway/), compose stack, Helm chart, CRDs + controller, TURN issuer, lifecycle scripts. |
| [`build/`](./build/) | Chromium build orchestration and the runtime image. |
| [`patches/`](./patches/) | The (small) Chromium patch series. |
| [`docs/protocols/`](./docs/protocols/) | Per-channel wire specs — the most useful docs here. |
| [`harness/`](./harness/) | Glass-to-glass latency measurement. |
| [`tests/`](./tests/) | Smoke, integration, e2e, and WebRTC drivers. |
| [`verification/`](./verification/) | Assertion-gate tooling. |

[`docs/README.md`](./docs/README.md) is the map: it groups the documentation by
what you're trying to do (integrate / operate / change the browser) and lists,
explicitly, which documents are historical.

Historical planning documents (`PROJECT_BRIEF.md`, the phase-exit reports, the
Phase 1 stack audit) describe an earlier architecture — stock Chromium plus
`getDisplayMedia` — that no longer exists. They each carry a status banner
saying so. Read them as history.

---

## Contributing

See [`CONTRIBUTING.md`](./CONTRIBUTING.md). Two things worth knowing up front:

- Anything in the hot path wants a harness measurement attached.
- A change to a documented protocol needs the spec in `docs/protocols/` updated
  in the same change.

---

## License

Apache-2.0 — see [`LICENSE`](./LICENSE).

This project builds and distributes a derivative of Chromium, which carries its
own licenses (BSD-3-Clause plus the terms of its many third-party
dependencies). If you ship binaries produced by `build/chromeless-build.sh`,
those obligations are yours to meet.

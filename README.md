# chromeless

**A Chromium embedder that streams a real browser over WebRTC, with a native
libwebrtc peer running inside the browser process.**

A real Chromium runs on a Linux server, its tab is captured straight out of the
Viz compositor via `FrameSinkVideoCapturer`, encoded (x264 / VP9 / NVENC /
VAAPI / SVT-AV1), and streamed to a client. Mouse, keyboard, clipboard, file
uploads, cursor updates, and audio round-trip through the same peer connection.
Useful for browser isolation, agent tooling, embedded co-browsing, and anywhere
you need a real Chromium you don't want to run locally.

**Status: source-available, in production.** This is not a packaged product.
There are no published container images, no npm package, and no release tags —
you build it yourself. See [Building](#building) for what that costs.

---

## What you actually get

| Component | Language | What it is |
| --- | --- | --- |
| `capture/` | C++ | The product. A Chromium content-embedder (`cloud_browser_worker`) built out-of-tree against a pinned Chromium release branch. Native browser-process libwebrtc peer, FrameSink capture, encoder factory, input dispatch, five data channels, a custom `Cb.*` CDP domain. |
| `signaling/` | Go | WebSocket signaling broker. One session = one `client` + one `browser`. JWT auth, TURN credential minting, offer/ICE replay buffers. |
| `client/` | TypeScript | Browser-side client: answerer state machine, input encoder, cursor/clipboard/file-upload/stats channels, codec negotiation, reconnect. Tested with vitest. |
| `infra/` | YAML/shell | compose stack, Helm chart, `BrowserSession` CRDs + controller, TURN issuer. |
| `harness/` | mixed | Glass-to-glass latency measurement (flashing block + webcam reconciliation). |

The browser is **always the offerer**; the client is the answerer.

---

## Quickstart

You need a `cloud_browser_worker` image. Nothing here publishes one, so either
build it (below) or use one your organization already built.

```bash
git clone <this-repo> && cd chromeless

# Build the client bundle once.
( cd client && npm ci && npm run build )

CHROMELESS_IMAGE=my-registry/chromeless:cr7727-abc1234 \
  docker compose -f infra/compose.yaml up
```

Then open <http://localhost:3000>.

Driving it from a browser on the host means the in-network DNS name won't
resolve, so point the client at the published port:

```bash
CHROMELESS_IMAGE=… SIGNALING_URL=ws://localhost:8080 \
  docker compose -f infra/compose.yaml up
```

`CHROMELESS_IMAGE` is required and has no default — compose fails fast with a
message rather than pulling a tag that doesn't exist.

Without `CHROMELESS_ICE_SERVERS` the peer falls back to public STUN. That is
fine for a same-host compose run and **will not traverse most NATs**; bring
your own TURN for anything else.

---

## Building

The browser is a from-source Chromium build. There is no smaller path:

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
        └───────┬───────────────────────▲────────┘
                │ WebSocket (SDP/ICE)   │ SRTP/DTLS media + DataChannels
                ▼                       │
        ┌──────────────────────┐        │
        │  signaling/  (Go)    │        │
        │  offer + ICE replay  │        │
        └───────┬──────────────┘        │
                │                       │
┌───────────────▼───────────────────────┼──────────────────────────────┐
│  cloud_browser_worker  (capture/)     │                              │
│                                       │                              │
│   Chromium browser process            │                              │
│     ├─ Viz compositor                 │                              │
│     │    └─ FrameSinkVideoCapturer ──▶ video track source ──┐        │
│     ├─ native libwebrtc PeerConnection ◀────────────────────┘        │
│     │    └─ encoder factory: x264 / VP9 / NVENC / VAAPI / SVT-AV1 ───┤
│     ├─ DataChannels: input · stats · cursor · clipboard · files      │
│     └─ CDP: Cb.startSession · Cb.startFrameSinkCapture · …           │
└──────────────────────────────────────────────────────────────────────┘
```

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
| `SIGNALING_TOKEN` | `WEBRTC_SIGNALING_TOKEN` | Optional JWT, forwarded verbatim as a query param. |
| `CHROMELESS_ICE_SERVERS` | `WEBRTC_ICE_SERVERS` | JSON array of `{urls, username, credential}`, or an object with an `iceServers` array. |
| `CHROMELESS_ICE_TRANSPORT_POLICY` | `WEBRTC_ICE_TRANSPORT_POLICY` | `all` (default) or `relay`. |

Setting a `WEBRTC_*` variable directly always wins, which is how the Kubernetes
controller drives it.

Sessions can also be started at runtime over CDP — `Cb.startSession` (and its
deprecated alias `Cb.startNativeSession`) takes the same settings as
parameters, which is what a warm-pool orchestrator uses after restoring a
snapshot.

---

## Deploying

- **Kubernetes** — the chart at [`infra/helm/chromeless/`](./infra/helm/chromeless)
  ships `BrowserSession` / `BrowserSessionPool` CRDs and a controller that
  translates annotations into pod env. Point `images.*` at your own registry;
  nothing is published to a public one. Required values: the Ed25519 auth
  pubkey, the TURN shared secret, and TURN URLs.
- **Firecracker / warm snapshots** — boot the worker with no signaling env and
  drive `Cb.startSession` over CDP once the microVM is restored.
- **Bare docker** — `infra/compose.yaml` is the reference.

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
- **One tab per session, one session per worker process.** Starting a second
  session in a live process is not yet supported; the process is single-use.
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
| [`infra/`](./infra/) | compose stack, Helm chart, CRDs + controller, TURN issuer, lifecycle scripts. |
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

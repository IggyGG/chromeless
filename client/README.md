# Browser client

The page a user sees after logging in to a standalone deployment, and the
TypeScript library underneath it that any app can embed. Both live here.

The client is the **WebRTC answerer**: the browser process (`capture/`) holds the
captured media tracks and is therefore the offerer, so it constructs the offer
that already knows its codecs, RTP extensions and data channels. The client waits
for that offer, answers it, and attaches what arrives to the page.

## Where it runs

In a standalone deployment the gateway (`infra/gateway/`) serves `dist/` on the
**same origin** as the signaling WebSocket it proxies. That is why nothing here
needs configuring: `src/config.ts` derives the signaling endpoint from
`location`, and one certificate decision covers the page and the `wss://` dial.
See `docs/operations/standalone.md`.

Three escape hatches exist for other setups, in this order of precedence:
`?signaling=<ws-url>` on the page URL, `window.__CHROMELESS_CONFIG__` from an
optional `config.js`, then the compile-time fallback.

## Integrating: `ChromelessSession`

`src/session.ts` is the DOM-free answerer state machine — token fetch, ICE
config, signaling dial with reconnect, offer/answer/ICE pump, codec
classification, the `stats` channel, teardown. `main.ts` is only a thin demo shell
over it, and the recipe a third party copies is:

```ts
import { ChromelessSession } from "./src/session.js";

const session = new ChromelessSession({ signalingBase });
session.on("track", (_track, stream) => { video.srcObject = stream; });
session.on("dataChannel", (dc) => { /* wire by dc.label, see below */ });
session.on("status", (status, hint) => { /* idle|connecting|connected|failed|closed */ });
session.on("stats", (sample) => { /* per-second RTCStats digest */ });
await session.connect(sessionId);
```

`pcCreated` fires for the initial peer connection **and every rebuild** after a
signaling reconnect; anything holding a `RTCPeerConnection` reference must re-arm
on each emission. `closed` fires once, on `bye`, on a fatal negotiation failure,
or on `disconnect()`.

### Data channels the guest opens

Everything except `stats` is forwarded on `dataChannel` for the caller to wire.
`main.ts` wires all five; the published list is
`window.__cb_client_consumers`, which exists so a stale gateway bundle can be told
apart from a guest defect.

| label | module | what it carries | spec |
| --- | --- | --- | --- |
| `input` | `src/input.ts` `InputChannel` | mouse, wheel, keyboard, IME, drag, touch → guest | `docs/protocols/input-channel.md` |
| `cursor` | `src/cursor.ts` `attachCursorChannel` | the pointer shape the page wants (the OS draws the position) | `docs/protocols/cursor-channel.md` |
| `files` | `src/file-upload.ts` `FileUploadChannel` | chunked uploads → guest | `docs/protocols/file-upload.md` |
| `control` | `src/control.ts` `attachControlChannel` | the guest asking a human: `alert`/`confirm`/`prompt`/`beforeunload` | `capture/build-integration/cb_control_channel.h` |
| `clipboard` | `src/clipboard.ts` `ClipboardChannel` | text both ways | `docs/protocols/clipboard-channel.md` |
| `stats` | inside `ChromelessSession` | per-second client stats → guest | `docs/protocols/stats-channel.md` |

Two guest-side halves are not finished yet, and the client can do nothing about
them: paste and file upload reach the wire and are dropped in the guest
(`cb_clipboard_relay.cc`, `cb_file_upload_relay.cc`). `docs/roadmap-ga.md` has
the plan.

### Navigation is not on the peer connection

The address bar posts to the gateway (`src/navigate.ts`: `navigate`, `goBack`,
`goForward`, `reload`, `stopLoading`, `currentUrl`), which drives CDP on the
worker. The peer connection carries pixels and input only. `normalizeUrl` turns
"example.com" into `https://example.com/` and a bare phrase into a search.

### Other modules

- `src/auth.ts` — fetches the session token the gateway mints; `TokenRefresher`
  rolls it over (not yet wired into the session).
- `src/turn.ts` — ICE/TURN config from `/turn-credentials`.
- `src/reconnect.ts` — `ReconnectingWebSocket` with capped backoff, and the
  `request_renegotiate` ICE-restart helper.
- `src/probe.ts` — the pre-call quality probe sent as `probe_result`.
- `src/sdp.ts` — `prioritizeCodec`, applied to the **answer** only.
- `src/codec-negotiate.ts` — classifies what was actually negotiated.
- `src/passthrough.ts` — camera/microphone sharing into the guest.
- `src/simulcast.ts` — pure SDP helpers; not used by the demo page.

## Build, test, run

```bash
cd client
npm install
npm run build       # esbuild → dist/main.js, then copies index.html
npm run typecheck   # strict tsc, no emit
npm test            # vitest (node environment — there is no DOM here)
npm run watch       # rebuild on change; the gateway serves dist/ live
```

The gateway image builds this bundle itself from the repo root, so a client
change reaches a deployment only when that image is rebuilt and rolled. The
compose stack also bind-mounts `dist/` over the baked copy for live reload.

Because vitest runs without a DOM, anything that renders (the control-channel
overlay, the cursor) is verified by `tests/local/` and `tests/interactive/`
against a real browser, not here.

## Debug hooks

- `?e2e=1` pins the live `RTCPeerConnection` to `window.__cbwrtc_pc` for the e2e
  specs. Off by default because this page is user-reachable.
- `?debug=1` is reserved for the debug panel toggle (see `docs/roadmap-ga.md`).
- The right-hand panel and log pane show every signaling and ICE transition; the
  status pill's hint text names the wait it is in (`waiting for offer`,
  `no browser on session "<id>"`, `no offer from browser`).

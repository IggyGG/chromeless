# Browser client (v0)

The user-facing page. Per `docs/capture/path-of-least-resistance.md`,
this client is the **WebRTC answerer** — the streamer
(`capture/streamer-page`) holds the captured media tracks and is
therefore the offerer. The flow is:

1. Click **Connect**. The page fetches `/turn-credentials` (T25) from
   the signaling server, then opens a WebSocket at
   `ws://localhost:8080/ws/{session}`.
2. The client sends a hello frame so the signaling server learns its
   role (`from: client`), then waits.
3. When the streamer sends an `offer`, the client calls
   `setRemoteDescription`, `createAnswer`, applies T30 SDP munging
   (VP9 prioritized), `setLocalDescription`, and returns the answer.
4. ICE candidates trickle in both directions.
5. Incoming media tracks (`pc.ontrack`) are attached to the
   `<video>` element.
6. Incoming data channels (`pc.ondatachannel`) labelled `"input"`
   are wrapped in an `InputChannel` (T20) that forwards mouse,
   keyboard, scroll, IME, and clipboard events to the streamer.

The signaling/ICE/connection/data-channel states are surfaced in the
right-hand debug panel.

> **Why this role split?** The streamer holds the media tracks already
> wired into its `RTCPeerConnection`, so it has the information needed
> to construct a valid offer (SDP includes the negotiated codecs, RTP
> extensions, simulcast layout, etc.). Asking the client to offer
> would mean the client has to add `recvonly` transceivers blind, then
> the streamer has to coerce them — extra round trips and brittle
> against codec changes. Letting the streamer offer is what
> Selkies-GStreamer, Pion-based projects, and Hyperbeam all do.

## Build

Requires Node 20+ and a network connection on first run for `npm install`.

```bash
cd client
npm install
npm run build
```

Outputs `dist/main.js` (bundled, sourcemapped) and `dist/index.html`.

## Run

The client expects a signaling server on `localhost:8080`. Start it
from a separate terminal:

```bash
cd ../signaling
go run .
```

Then either open `dist/index.html` directly in your browser, or serve
`dist/` from any static HTTP server, e.g.:

```bash
cd dist
python3 -m http.server 5173
# then visit http://localhost:5173
```

Click **Connect**. Expected behavior in a full stack (with the
streamer page from `capture/streamer-page` running):

1. Status pill goes `idle` → `connecting (waiting for offer)` →
   `connected`.
2. Log shows `ws open`, `← offer`, `→ answer`, a stream of
   `↔ ice` lines, then `connectionState=connected`.
3. Incoming video and audio attach to the `<video>` element.
4. If the streamer offers a data channel labelled `input`, the
   `InputChannel` from T20 wires up mouse/keyboard/IME forwarding
   automatically.

If the streamer is **not** running, the client will sit at
`waiting for offer` indefinitely. ICE candidates will not be gathered
until `setRemoteDescription` is called, so the debug panel stays
quiet — that's normal.

## SDP munging

T30's `prioritizeCodec(sdp, "VP9")` is applied to **the answer**, not
the offer. The streamer is free to apply its own munging on the offer
if it has a reason to; this side intentionally only touches outbound
SDP to avoid confusing the negotiation.

If the streamer eventually publishes its preferred codec ordering
(e.g., AV1 first), tweak the call site in `main.ts` accordingly. The
SDP transforms in `client/src/sdp.ts` are pure functions — full
documentation in `docs/protocols/sdp-munging.md`.

## Other commands

```bash
npm run typecheck   # strict tsc, no emit
npm run test        # vitest run (input + turn + sdp tests)
npm run watch       # esbuild watch mode for development
```

## Files

- `index.html` — page shell, status pill, video stage, debug panel.
- `main.ts` — answerer-role lifecycle, signaling, peer connection,
  and data channel binding.
- `src/input.ts` (+ `.test.ts`) — input data-channel encoder (T20).
- `src/turn.ts`  (+ `.test.ts`) — ICE/TURN config fetcher (T25).
- `src/sdp.ts`   (+ `.test.ts`, `__fixtures__/`) — SDP munging (T30).
- `copy-html.mjs` — post-build step that copies `index.html` into `dist/`.
- `tsconfig.json` — strict TS config (typecheck only; esbuild does the bundling).
- `package.json` — minimal deps: `typescript`, `esbuild`, `vitest`.

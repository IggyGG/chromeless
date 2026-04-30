# Browser client (v0)

Single-page client that:

- opens a WebSocket to the signaling server at `ws://localhost:8080/ws/{session}`,
- creates an `RTCPeerConnection` with a default Google STUN server,
- opens a data channel `input` (for future input forwarding),
- sends a stub SDP offer and applies any answer it receives,
- attaches an incoming remote track (when one shows up) to the page's
  `<video>` element,
- surfaces signaling / ICE / connection / data-channel state to a debug panel.

This is the **stub** for T14. There is no real remote media source yet —
capture lives in Phase 1 — so the connection will sit in
`have-local-offer` / `new` ICE state until a real peer answers. The goal
of this task is to validate that signaling round-trips and that the
peer-connection lifecycle wiring is correct.

## Build

Requires Node 20+ and a network connection on first run for `npm install`.

```bash
cd client
npm install
npm run build
```

Outputs `dist/main.js` (bundled, sourcemapped) and `dist/index.html`.

## Run

The client expects a signaling server on `localhost:8080`. Start it from a
separate terminal:

```bash
cd ../signaling
go run .
```

Then either open `dist/index.html` directly in your browser, or serve `dist/`
from any static HTTP server, e.g.:

```bash
cd dist
python3 -m http.server 5173
# then visit http://localhost:5173
```

Click **Connect**. Expected behavior:

1. Status pill goes `idle` → `connecting`.
2. Log shows `ws open`, `→ offer`, then a stream of `→ ice` lines as
   ICE candidates are gathered.
3. Without a real peer answering, `signalingState` will sit at
   `have-local-offer` and `iceConnectionState` at `new` / `checking`.
4. When the cloud-browser side eventually responds with an SDP answer,
   the client will log `← answer`, transition to `connected`, and any
   incoming `ontrack` will attach to `<video>`.

## Other commands

```bash
npm run typecheck   # strict tsc, no emit
npm run watch       # esbuild watch mode for development
```

## Files

- `index.html` — page shell, status pill, video stage, debug panel.
- `main.ts` — all client logic (WebSocket, RTCPeerConnection, UI bindings).
- `copy-html.mjs` — post-build step that copies `index.html` into `dist/`.
- `tsconfig.json` — strict TS config (typecheck only; esbuild does the bundling).
- `package.json` — minimal deps: `typescript`, `esbuild`.

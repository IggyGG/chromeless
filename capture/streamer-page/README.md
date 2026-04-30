# `capture/streamer-page/` — the privileged streamer page

The streamer page is the bit of HTML+JS that runs **inside** the headless
Chromium container and turns the captured display into a
`RTCPeerConnection` whose remote peer is the user's browser. It is the
concrete implementation of the design in
`docs/capture/path-of-least-resistance.md` (T15).

There is nothing user-facing here. The user's browser talks to
`client/` (T14). This page lives behind the curtain.

## Files

| File              | Purpose                                                                 |
|-------------------|-------------------------------------------------------------------------|
| `index.html`      | The DOM shell. No real UI — exists only so debug screenshots are legible. Loads `streamer.js` as a module. |
| `streamer.js`     | The streamer logic: `getDisplayMedia()` → `RTCPeerConnection` → signaling, with heartbeat and clean exit. |
| `launch.md`       | The Chromium command-line flags + URL the headless instance is launched with, intended for `infra-dev` to wire into supervisord. |

## How it gets launched

The container's supervisord (set up in T7's `infra/Dockerfile` /
`infra/compose.yaml`) launches a Chromium instance that opens this page
as its sole tab, with the auto-grant flags from `launch.md`. The page
serves from the local HTTP daemon also running in the container (any
trivial static-file server — `python -m http.server 9000`, `caddy
file_server`, or a tiny Go binary).

The exact wiring (which static-file server, which port, which
supervisord program block) is `infra-dev`'s domain — see `launch.md`
for what they need to know, and the follow-up task created in
`tests/notes/` (next paragraph).

## Coordination

- **T13 (signaling):** uses the protocol from `signaling/server.go`
  (envelope `{type, from, data}`, role `browser`). Already complete.
- **T14 (client):** is the **answering** side of the offer this page
  produces. The current T14 stub also creates an offer; that needs
  to flip to handle an incoming offer from `from:"browser"` and reply
  with an answer carrying recvonly transceivers' answers. Coordinate
  with `webrtc-dev`.
- **T7/T8 (infra):** must launch Chromium with the flags in
  `launch.md` and serve this page over HTTP. A follow-up task will
  be filed targeting `infra-dev` if they aren't already wired.

## Acceptance

Per T23's DoD: the page loads in a regular Chrome with the auto-grant
flags from `launch.md`, calls `getDisplayMedia` without a picker,
connects to a running signaling server, sends an offer, and stays
alive sending heartbeat log lines every 10 s. Real end-to-end
connection requires the T14 client flip mentioned above.

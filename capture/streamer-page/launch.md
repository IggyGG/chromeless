# Launching the streamer page in headless Chromium

This is the command line `infra-dev` needs to wire into supervisord
inside the Phase 1 container, plus the small static-file server that
hosts this page.

## URL the streamer page expects

```
http://localhost:9000/streamer/index.html?signal=ws://signaling:8080/ws&session=dev&fps=30
```

Query parameters (all optional, but `session` should match what the
user's client connects to):

| Param      | Default                          | Notes                                |
|------------|----------------------------------|--------------------------------------|
| `signal`   | `ws://signaling:8080/ws`         | Signaling base URL. Session ID is appended. |
| `session`  | `dev`                             | Static session ID for v1.            |
| `fps`      | `30`                              | Display capture target framerate.    |
| `input`    | `ws://localhost:9100/input`      | Input-bridge endpoint (T22 / T41). Streamer relays each `RTCDataChannel("input")` message to this WS. Override for tests that run the bridge elsewhere. |

The host name `signaling` resolves to the signaling-server container in
`infra/compose.yaml` (T8). For local-laptop runs outside compose,
override with `?signal=ws://localhost:8080/ws`.

## Static-file server

Any static server will do — choose one that fits the container budget.
Suggestions in increasing order of complexity:

```bash
# Simplest — bundled with Python, already in the image.
python3 -m http.server -d /opt/cloud-browser/streamer 9000
```

```bash
# If we want a tiny Go binary as a single supervisord program:
go run signaling/cmd/staticserver -dir /opt/cloud-browser/streamer -addr :9000
```

`/opt/cloud-browser/streamer/` is the in-container path where the
contents of `capture/streamer-page/` get copied during image build.

## Chromium command line

These flags were verified against the Chromium command-line switch
reference at https://peter.sh/experiments/chromium-command-line-switches/
during T15. Verify against the pinned Chromium release branch
(`docs/build/chromium-from-source.md` §6) before shipping — flag
spelling drifts.

```bash
chromium \
  --no-first-run \
  --no-default-browser-check \
  --disable-features=TranslateUI,MediaRouter \
  \
  # Display + audio
  --window-size=1920,1080 \
  --window-position=0,0 \
  --autoplay-policy=no-user-gesture-required \
  \
  # Auto-grant getDisplayMedia / getUserMedia for the streamer origin.
  # --use-fake-ui-for-media-stream is the suspenders to the belt of
  # --auto-select-desktop-capture-source: it bypasses the picker for
  # tab/window paths as well, so any future use of getDisplayMedia
  # variants on this page works without UI.
  --use-fake-ui-for-media-stream \
  --auto-select-desktop-capture-source="Entire screen" \
  --auto-accept-this-tab-capture \
  --enable-usermedia-screen-capturing \
  \
  # Treat http://localhost:9000 as a secure context so getDisplayMedia
  # is permitted without HTTPS inside the container.
  --unsafely-treat-insecure-origin-as-secure=http://localhost:9000 \
  \
  # Open the streamer as an "app" (no chrome chrome).
  --app="http://localhost:9000/streamer/index.html?signal=ws://signaling:8080/ws&session=dev&fps=30"
```

## Supervisord program block (sketch)

```ini
[program:streamer-static]
command=python3 -m http.server -d /opt/cloud-browser/streamer 9000
autorestart=true
stdout_logfile=/var/log/streamer-static.log
redirect_stderr=true

[program:chromium]
command=/usr/bin/chromium %(ENV_CHROMIUM_FLAGS)s
autorestart=true
stopasgroup=true
killasgroup=true
stdout_logfile=/var/log/chromium.log
redirect_stderr=true
```

Where `CHROMIUM_FLAGS` is the entire flag list above as a single env
var so it can be tuned without editing the supervisord config.

## Follow-up task to file

`infra-dev` should own a follow-up to wire these flags into
`infra/Dockerfile` and the supervisord config. Create it as part of
the T23 hand-off (or roll it into T7/T8 follow-ups if those are still
in flight).

## Input-bridge co-location (T22 / T41)

The streamer dials the input-bridge at `ws://localhost:9100/input` by
default. That endpoint is expected to live **inside the same Chromium
container**, supervised by `supervisord`, listening on loopback only.
This keeps the dependency local — no extra service in compose, no
DNS to resolve — and means the bridge talks to Chromium's CDP at
`http://127.0.0.1:9222` without crossing a network boundary.

Required supervisord program block (sketch):

```ini
[program:input-bridge]
command=/usr/local/bin/input-bridge --source ws --ws-addr 127.0.0.1:9100 --cdp-url http://127.0.0.1:9222 --metrics-addr 127.0.0.1:9101
autorestart=true
stdout_logfile=/var/log/input-bridge.log
redirect_stderr=true
```

If this conflicts with an existing port assignment (the current
`infra/compose.yaml` has the comment "9100: cb-metrics-sidecar (T38)"
on the chromium container's published ports — the host-side
publication is for a different sidecar; input-bridge stays on
loopback inside the container), reconcile during the supervisord
wire-up. Track via the T23-followup or a dedicated infra task.

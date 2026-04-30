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
  \
  # Disable the features that fight the headless-Xvfb capture path on
  # Chromium 147+. T78-followup: without `Vulkan` in the disable
  # list, vkCreateInstance fails inside the GPU process (no Vulkan
  # driver inside Xvfb) and the failure cascades into the screen
  # capturer, surfacing as `NotReadableError: Could not start video
  # source` from getDisplayMedia. VaapiVideoDecodeLinuxGL is disabled
  # for the same reason — VAAPI isn't available under Xvfb.
  --disable-features=TranslateUI,MediaRouter,Vulkan,VaapiVideoDecodeLinuxGL \
  \
  # Force the X11 ozone backend explicitly. Without this, Chromium
  # 147 sometimes picks the headless ozone backend even when
  # --display=:99 is set, and the headless backend cannot drive the
  # screen capturer for getDisplayMedia. T78-followup.
  --ozone-platform=x11 \
  \
  # Software GL via ANGLE/SwiftShader. The Xvfb display has no real
  # GPU; ANGLE+SwiftShader is the supported software path that lets
  # the compositor (and therefore the screen capturer) keep working
  # without a Vulkan driver. T78-followup.
  --use-gl=angle \
  --use-angle=swiftshader-webgl \
  \
  # Xvfb has no real vsync; let Chromium drive the compositor at the
  # configured framerate without waiting on a non-existent vsync
  # signal. T78-followup.
  --disable-gpu-vsync \
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

### Why the GPU / Vulkan flags are non-negotiable on Chromium 147

This was T78-followup. On a stock `chromium 147` package running
under Xvfb, the diagnostics on the running container were:

```
$ tail /var/log/supervisor/chromium.err.log
...
vkCreateInstance failed with VK_ERROR_INCOMPATIBLE_DRIVER
...
$ # streamer page log
10:15:57.404 ERR  getDisplayMedia failed — check Chromium auto-grant flags
                 NotReadableError: Could not start video source
```

The cause: Chromium 147 tries the Vulkan path first inside the GPU
process. Vulkan init fails (Xvfb has no Vulkan driver), and the
failure leaves the screen capturer in a state where the desktop
source cannot be opened — even though the auto-grant flags
correctly bypass the permission picker. The error propagates to
the page as `NotReadableError`.

The fix is the four flags above (`--disable-features=Vulkan,...`,
`--ozone-platform=x11`, `--use-gl=angle --use-angle=swiftshader-webgl`,
`--disable-gpu-vsync`) — together they pin Chromium to the
software-rendered X11 path that has no Vulkan dependency.

The Phase-2 from-source build (T17 / T49) will likely revisit this:
once we own the Chromium binary we can pre-disable Vulkan at
build-time (`use_vulkan = false` in `args.gn`) and skip three of
these flags.

### T86: the X11+SwiftShader pin alone wasn't enough

Post-T78, qa-tester confirmed via DevTools that the X11 ozone
backend was actually in use (UA reports `X11; Linux x86_64`) but
`getDisplayMedia` STILL fails with `NotReadableError`. The fault
is not in the GPU layer — Vulkan is correctly disabled — but in
the screen-capturer's `start` step itself. We've ruled out the
obvious causes:

| Hypothesis                        | Verified                                      |
|-----------------------------------|-----------------------------------------------|
| Vulkan still trying               | `chromium.err.log` is clean post-T78.         |
| Xvfb has no screen                | `supervisord.conf` line 42 sets `-screen 0 1920x1080x24`. |
| Wrong ozone backend selected      | UA shows `X11; Linux x86_64`. ✓               |
| Permission picker blocking        | `--use-fake-ui-for-media-stream` set; no picker logs. |
| Auto-select-source flag wrong     | T86 added `--auto-select-tab-capture-source-by-title` sibling. Same error. |
| MIT-SHM / RandR extension absent  | (re-validate) Xvfb defaults include both, but worth a `xdpyinfo` audit. |

The remaining hypothesis is that Chromium 147's `DesktopCapturer`
implementation has a regression specific to capturing from
software-rendered X11 displays under SwiftShader. This is a
durable Chromium-side problem; we are not in a position to fix it
in the launch flag set alone.

### Phase 1 decision: route around with `--use-fake-device-for-media-stream`

`infra/compose.yaml` defaults `CBWRTC_USE_FAKE_MEDIA=1` for the
dev compose stack. The launch script appends
`--use-fake-device-for-media-stream` when set, which routes
`getDisplayMedia` to Chromium's synthetic test pattern + tone
generator instead of the real X11 screen capturer. The streamer
page sees a normal `MediaStream`; the rest of the WebRTC pipeline
(encoder, signaling, RTP, client receive) runs against real
encoded video and audio.

What this is:

- **A deliberate stop-gap** that unblocks T64 + T65 + every Phase 1
  end-to-end demo today.
- **Architecturally aligned with Phase 2.**
  `docs/capture/path-of-least-resistance.md` (T15) and
  `docs/capture/framesink-design.md` (T47) commit Phase 2 to a
  Chromium-internal `FrameSinkVideoCapturer` hook that does NOT go
  through `getDisplayMedia`. Spending more cycles fixing v1's
  `getDisplayMedia` path for Chromium 147+Xvfb is fixing a thing
  Phase 2 replaces.

What this is not:

- **NOT a fix.** Real screen capture is broken; we routed around
  it. If the prod stack ever flips this to `0`, we hit the same
  `NotReadableError`.
- **NOT a substitute for real-content demos.** Stakeholder demos
  that show "real cloud browsing" require either flipping back to
  real `getDisplayMedia` (and accepting the bug) OR landing the
  Phase 2 capture path.

To flip back for diagnostic runs:

```bash
CBWRTC_USE_FAKE_MEDIA= docker compose -f infra/compose.yaml up --build
```

(empty string ≠ unset for the `[ "${VAR}" = "1" ]` shell test;
either works to disable.)

### Diagnostic procedure when getDisplayMedia is the suspect

For the next time something in this area breaks. Run inside the
container:

```bash
# 1. Confirm Xvfb is presenting a renderable screen.
docker compose -f infra/compose.yaml exec chromium \
    sh -c 'DISPLAY=:99 xrandr 2>&1 || true'
#   Expect: "Screen 0: minimum 1920 x 1080, current 1920 x 1080,
#            maximum 1920 x 1080" or similar.
#   If "no screens" / "0x0": Xvfb didn't start. Check
#   /var/log/supervisor/xvfb.err.log.

# 2. Confirm the ozone backend that took effect.
docker compose -f infra/compose.yaml exec chromium \
    sh -c 'curl -s http://127.0.0.1:9222/json/version | grep -i user-agent'
#   Expect a UA containing "X11; Linux".

# 3. Confirm getDisplayMedia was the failure point and capture the
#    error name (NotReadableError, NotAllowedError, etc.).
#    streamer.js post-T78 surfaces err.name + err.message + UA in
#    one log line — open the streamer page log via DevTools or
#    /var/log/supervisor/chromium.log.

# 4. Probe whether window.pc was reached (i.e., did getDisplayMedia
#    succeed and the failure is downstream).
#    The T69 hook puts pc on window unconditionally. If
#    typeof window.pc === 'undefined', the failure is at or before
#    the getDisplayMedia call.
docker compose -f infra/compose.yaml exec chromium \
    sh -c 'curl -s "http://127.0.0.1:9222/json/list" | head'
#   Then connect a DevTools/Runtime.evaluate via the listed
#   webSocketDebuggerUrl and probe `typeof window.pc`.
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

# Container session lifecycle

Scripts and supervisord configuration that govern what happens when a
cloud-browser-webrtc container boots, runs, idles, and dies.

## Files

| Script | Where it runs | When |
|--------|--------------|------|
| `entrypoint.sh` | Dockerfile `ENTRYPOINT` chain (root, before supervisord) | Once per container start. |
| `cold-start.sh` | Invoked by `entrypoint.sh` (root) | Once per container start. |
| `idle-watchdog.sh` | `[program:idle-watchdog]` in supervisord (root) | Continuously, polling DevTools every `WATCHDOG_POLL_S` seconds. |
| `restart.sh` | Manual: `docker exec <c> /usr/local/bin/restart.sh` | On demand. |

## Boot sequence

```
docker run
  └─ /usr/bin/dumb-init                        (PID 1, signal handling)
       └─ /usr/local/bin/entrypoint.sh
            ├─ cold-start.sh                   (clears user-data-dir,
            │                                   resolves SESSION_ID,
            │                                   writes /run/cb-session/env)
            ├─ source /run/cb-session/env      (env into entrypoint shell)
            └─ exec /usr/bin/supervisord       (becomes PID 2; inherits env)
                 ├─ xvfb           priority 10
                 ├─ pulseaudio     priority 20
                 ├─ streamer-static priority 25
                 ├─ chromium       priority 30  (runs launch-chromium.sh)
                 └─ idle-watchdog  priority 40
```

## Cold-start (`cold-start.sh`)

- **Clears** `~cbuser/.config/chromium` so the new session has a fresh
  profile. (We always rebuild it; recovering a previous profile is
  explicitly NOT a v1 feature — see Phase 3 snapshot/restore in
  `PROJECT_BRIEF.md`.)
- **Resolves session env**: SESSION_ID falls back to `auto-<6 random
  hex bytes>` if not provided. SIGNALING_URL, STREAMER_FPS,
  STREAMER_PORT take their compose-side defaults.
- **Persists** the resolved values to `/run/cb-session/env` and the id
  alone to `/run/cb-session/id`. Other lifecycle scripts read these
  rather than re-implementing the resolution logic.

## Idle watchdog (`idle-watchdog.sh`)

Polls Chromium DevTools for the streamer page (T23) and evaluates
`window.pc.connectionState`. States `connected`, `connecting`, `new`
count as active; everything else (including a missing `window.pc` and
unreachable DevTools) counts as idle.

After `WATCHDOG_GRACE_S` seconds (default 60s) of grace, if the page
has been idle for `IDLE_TIMEOUT_S` continuous seconds (default 600s),
the watchdog calls `supervisorctl shutdown` to terminate all programs
and let dumb-init exit cleanly. Container exits with status 0.

### Knobs

| Env | Default | Notes |
|-----|---------|-------|
| `DEVTOOLS_URL` | `http://127.0.0.1:9222` | Where to dial DevTools. |
| `IDLE_TIMEOUT_S` | `600` | Continuous idle before shutdown. |
| `WATCHDOG_GRACE_S` | `60` | Don't account idle for this long after boot. |
| `WATCHDOG_POLL_S` | `30` | Poll interval. |

### Limitations of the heuristic

This is the right Phase 1 signal but the wrong Phase 3 signal:

- **It depends on the T23 streamer-page contract.** A streamer page
  that doesn't expose `window.pc` looks like an idle session even when
  audio/video are flowing. Mitigation: T23 publishes `window.pc`; if
  that contract changes, update the `expr` in `idle-watchdog.sh`.
- **It can't see signaling-side disconnects from the user's browser.**
  If the user closes their tab but the streamer-side `pc` happens to
  still be in `connected` (waiting for ICE timeout), we'll wait the
  full `IDLE_TIMEOUT_S` instead of shutting down promptly.
- **Phase 3 fix:** the signaling server should expose a per-session
  "last activity" timestamp, and the watchdog should poll that
  instead. Filed as a Phase 3 follow-up; see `PROJECT_BRIEF.md` Phase 3
  observability bullet.

## Manual restart (`restart.sh`)

`supervisorctl restart chromium`. Useful for debugging without
nuking the whole container. Drops the active streamer page, so the
remote user sees a reconnect.

## Why supervisord still runs cold-start *outside* itself

Supervisord has no native one-shot-program concept; configuring
`autorestart=false, exitcodes=0` makes a one-shot program land in the
EXITED state which is fine, but means the program runs *after*
supervisord has already started other programs (priority orderings are
honored, but supervisord doesn't block program startup on a
predecessor's *successful exit*). Running cold-start in the entrypoint
chain instead guarantees user-data-dir is wiped *before* Chromium ever
starts.

## Coordination with the rest of the stack

- **T28**: `launch-chromium.sh` reads `SESSION_ID` / `SIGNALING_URL` /
  `STREAMER_FPS` / `STREAMER_PORT`. Cold-start sets these via
  `/run/cb-session/env`, sourced by `entrypoint.sh` before
  supervisord starts.
- **T13/T31 sub-A**: the watchdog assumes a healthy signaling server
  is reachable so the streamer page can ever transition into
  `connected`. If signaling is down, the watchdog will declare the
  container idle on schedule — that's the desired behaviour.
- **T9 (qa-tester)**: container smoke test should run with
  `IDLE_TIMEOUT_S=999999` (effectively disabled) so the smoke doesn't
  race the watchdog.

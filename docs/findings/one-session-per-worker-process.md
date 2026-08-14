# A worker serves exactly one session, then goes quiet forever

**Status:** confirmed against a live deployment, 2026-08-14. The broker half is
fixed (`signaling/server.go`, `discardReplay`). The worker half is in `capture/`
(C++), which cannot be compiled in this repo — see "Fixing it".

**Impact for a standalone deployment:** the first viewer after the worker starts
gets video. Every viewer after that gets nothing, until the worker is restarted.
Reloading the page counts as a new viewer, so "it worked, then it stopped
working and a restart fixed it" is the expected shape.

## What happens

`cb_offerer_driver` treats a closed session as terminal, and the embedder only
ever brings one up:

- `CbOffererDriver::kClosed` is a terminal state. `BeginRenegotiation` refuses
  to run from it (`cb_offerer_driver.cc:1082-1085`), and a clean `bye` from
  either side routes through `CloseInternal` into exactly that state
  (`:1028-1031`).
- `CloudBrowserBrowserMainParts::StartNativeSession` is guarded by a one-way
  latch: `if (native_session_started_) return INVALID_STATE` with the message
  `"native session already started"`
  (`cloud_browser_browser_main_parts.cc:865-867`). So `Cb.startNativeSession`
  cannot re-arm it either.
- `OnClosed` only logs (`:1480-1484`). Nothing re-offers.

So after the first viewer leaves, the process holds a live WebSocket to the
broker, a healthy Chromium, a running capturer — and no peer connection, with
no path back to having one.

Read straight off the worker's log, one viewer connecting and leaving:

```
14:29:27.714  ICE connection state -> 2      (connected)
14:29:42.738  ICE connection state -> 3      (completed)
14:29:55.082  ICE connection state -> 5
14:29:55.109  session closed, reason=remote bye
14:29:55.110  ICE connection state -> 6      (closed)
   ...nothing further. No offer, no candidate, for the rest of the process.
```

Meanwhile the capturer keeps logging `retargeted producer to latest FrameSink
target` every 15 s, which makes the process look healthy in every log anyone
would think to check.

## Why it presented as a NAT bug

The broker's replay buffer (T96/T104) held the dead session's offer and its
nine candidates and handed them to the next viewer to join — `"replayed": 10`
in the broker log. That viewer received a complete, well-formed offer, answered
it, and paired its own fresh candidates against sockets that no longer existed.

The result is `iceConnectionState=checking` forever with a single unresponsive
pair (`req=0`), on a deployment where TURN allocations demonstrably succeed and
both sides gather host + srflx + relay. That is indistinguishable by inspection
from a relay or NAT problem, and it consumed most of a day — including chasing
a red herring where this laptop's public IP changed twice mid-session.

**The broker fix stops the disguise, not the limitation.** A second viewer now
receives no offer at all. Still broken, but honestly broken: the client sits at
"waiting for offer" instead of manufacturing a doomed connection, and the
broker log shows `peer joined` with no replay.

## Fixing it

Two options, in preference order.

1. **Make the driver re-armable.** Allow `kClosed → kCreatingOffer` on a fresh
   client join, and drop the `native_session_started_` latch (or add a
   `Cb.restartNativeSession` that clears it). The header already documents the
   renegotiation machinery this would reuse — `BeginRenegotiation` exists and
   is BEGIN-FROM-`kIceInFlight` only; this adds one more legal entry edge. The
   PC is dropped on close, so it would need re-creating via the same path
   `Start()` uses.

2. **Exit the process on session close, and let the supervisor restart it.**
   `[program:chromium]` in the runtime image already has `autorestart=true`, so
   `OnClosed` calling the same quit path `Cb.shutdown` uses would produce a
   fresh process with a fresh peer connection in a few seconds. Cruder, loses
   all browser state (open tabs, history, cookies-in-memory), but it is a small
   and obviously-correct change. This is effectively what triform does at a
   higher level, where physics recycles the guest.

Option 2 is a reasonable stopgap for standalone use; option 1 is the right fix
if the browser is meant to be a persistent workspace that several people attach
to over its lifetime.

Not attempted here: `capture/` is an out-of-tree Chromium embedder needing a
4–8 h build (see CLAUDE.md), so any change would ship unverified.

## Testing it

`signaling/replay_test.go:TestWS_ByeDiscardsReplayBuffer` covers the broker
half, driving real WebSockets through the real handler. It asserts both
directions: no dead envelopes to the second viewer, and live forwarding still
works afterwards (so "discard everything" cannot pass by breaking the broker).

The worker half has no test here and cannot have one without a build.

## Related

- `docs/findings/wheel-phase-start-delta-dropped.md` — the other confirmed
  embedder defect found in the same session.
- `capture/signaling/cb_signaling_reconnect.h` is explicit that token refresh
  is out of scope and the process expects an orchestrator to restart it. This
  finding is the same shape: the embedder assumes something outside it owns the
  lifecycle. For a standalone deployment, nothing does.

# A worker serves exactly one session, then goes quiet forever

> **RESOLVED — and superseded.** The exit-on-close fix described below shipped
> and was verified; it was then replaced by the re-armable driver
> (`RearmSession`, `cloud_browser_browser_main_parts.cc`), which keeps the
> browser process and its state across viewers. Exit is now only the fallback
> when a re-arm fails. Kept because code and tests cite this file; the
> audio-teardown note at the bottom is still relevant to the re-arm path.

**Status:** BOTH HALVES FIXED AND VERIFIED (2026-08-14, image
`cr7727-e654ed644219`). The broker half has two mutation-checked tests
(`signaling/server.go`, `discardReplay`). The worker half uses option 2 below —
exit on session close, let supervisord restart — and was confirmed live:

```
21:56:48  session closed, reason=remote bye
21:56:48  Cb.shutdown: quitting the main message loop
21:56:48  exited: chromium (exit status 0; expected)
21:56:49  spawned: 'chromium' with pid 286      ← ~1s later, ready for the next viewer
```

One consequence worth knowing before it surprises someone: **the worker now
restarts as part of normal operation**, every time a viewer disconnects. Any
tool holding a CDP session to it must expect its page target to disappear —
`tests/interactive/harness.py`'s `WorkerOracle` reconnects for exactly this
reason.

It also exposed a pre-existing audio-teardown gap; see "Audio teardown" below.

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

### The same bug in the other direction

Deploying that fix and watching the broker turned up a second instance of it,
which the `bye`-only rule did not cover. A viewer whose socket simply drops —
tab closed, network blip, WS 1006 — sends no `bye`, so its `answer` stayed in
the buffer. The next **worker** to join was replayed it:

```
14:54:55  peer left      role=browser                    ← worker restarting
14:54:58  peer joined    role=browser   replayed=1       ← fresh worker gets
                                                            the OLD viewer's answer
14:54:59  ICE connection state -> 2                      ← "connected", to nobody
```

A freshly-booted worker consumed a dead answer, believed it had a viewer, and
burned its one and only session before any real viewer arrived. Restarting the
worker — the documented workaround for the defect above — therefore did not
help, which is what made this worth chasing rather than accepting.

The rule that covers both is simpler than the one it replaced: **a peer's
buffered envelopes describe its peer connection, and are only valid while that
peer is connected.** `hub.dropIfEmpty` does not do this — it only fires when
BOTH peers are gone, and both these failures happen while one side is still up.

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

Two tests in `signaling/replay_test.go` cover the broker half, both driving
real WebSockets through the real handler:

- `TestWS_ByeDiscardsReplayBuffer` — the `bye` path. Also asserts that live
  forwarding still works afterwards, so "discard everything, always" cannot
  pass by breaking the broker.
- `TestWS_DisconnectDiscardsReplay` — the socket-drop path, with a browser
  deliberately kept connected throughout.

Both were checked by reverting each fix independently and confirming the
matching test goes red. That check earned its keep three times here: the first
version of each test passed with the fix reverted, for a different reason each
time (calling the function directly instead of through the handler; closing
both peers so `dropIfEmpty` reaped the session; asserting against a peer that
had not finished registering). **A test written against a bug you have already
fixed proves nothing until you have watched it fail.**

The worker half has no test here and cannot have one without a build.

## Audio teardown: a pre-existing gap this made visible

Exiting on close surfaced a warning that was always reachable but rarely seen:

```
[m55-r5] OnClosed(reason=remote bye) in state=active; embedder did not call
PrepareForTeardown before driver.Close — running best-effort cleanup,
PulseAudio orphan-stream window is open until libwebrtc worker teardown
completes
```

`cb_audio_lifecycle.h` is explicit that `PrepareForTeardown()` MUST run BEFORE
`driver.Close()`, because `OnClosed` fires after `pc_` has already been
dropped. Both existing call sites are embedder-INITIATED closes
(`PostMainMessageLoopRun`, `OnGpuPermanentDeath`). A remote `bye` closes the
driver without asking the embedder, so that path never had one — and now it
runs on every viewer disconnect.

Benign for exit-on-close: the orphan window shuts when the process does,
milliseconds later. It is NOT benign for option 1 (re-armable driver), where
the process keeps running and the window would stay open across every viewer
change. **Whoever implements option 1 must give `CbOffererDriver` a pre-close
hook so the lifecycle can stop cleanly on an inbound bye.** This is recorded at
the call site in `cloud_browser_browser_main_parts.cc` as well.

## Related

- `docs/findings/wheel-phase-start-delta-dropped.md` — the other confirmed
  embedder defect found in the same session.
- `capture/signaling/cb_signaling_reconnect.h` is explicit that token refresh
  is out of scope and the process expects an orchestrator to restart it. This
  finding is the same shape: the embedder assumes something outside it owns the
  lifecycle. For a standalone deployment, nothing does.

## 2026-08-20: measured from the worker's own log — the driver never CLOSES

Task 349025, a standalone `docker compose` stack, worker image
`cr7727-224c19413e24` (which DOES contain the exit-on-close fix, `0c5e6a1`, by
git ancestry). Worker log and broker log, same run:

```
17:43:14    worker joins broker, offers                    (buffered)
17:47:40.2  spec 01 client joins, replayed:7
17:47:40.3  ICE -> 1 checking
17:47:40.3  ICE -> 2 CONNECTED        <- spec 01 passes here
17:47:40.3  ICE -> 3 COMPLETED
17:47:40.7  spec 01 ends, client vanishes (NO bye)
17:47:41.1  spec 02 client joins      -- no replay (broker fix working)
17:47:46.5  ICE -> 5 disconnected
17:47:56.5  ICE -> 4 FAILED
17:48:14    spec 03 client joins      -- no replay
```

**`session closed` never appears. `Cb.shutdown` never appears. Zero
renegotiations, zero further offers.** So the exit-and-respawn cure written up
above never fires *in this path*: the viewer disappears without a `bye`, so
`CbOffererDriver` is never told the session ended. It just watches ICE rot
`connected -> disconnected -> failed` and sits there. `OnClosed` — and therefore
`Shutdown()` — is only reached on an explicit close.

That reframes the defect. It is not only "a closed session is terminal"; it is
that **a viewer leaving without a bye never closes the session at all**, so the
process is left holding a FAILED peer connection with no path back and nothing
to trigger the restart. A real user closing a laptop lid produces exactly this.

Two consequences for whoever fixes it:

- The ICE-failure path needs to reach the same teardown as `bye`. Watching for
  `kIceConnectionFailed` and routing it into `OnClosed`/`Shutdown` would make
  the existing autorestart cure cover the common case.
- Option 1 (make the driver re-armable) still needs this, because it needs to
  notice the viewer is gone before it can re-offer.

⚠ **Reading the log at all is a trap.** `infra/supervisord.phase2.conf` sends
chromium's stdout to `/var/log/supervisor/chromium.log` and stderr to
`/dev/console`; the embedder's `LOG(INFO)` goes to **stderr**, so
`chromium.log` is EMPTY and `docker logs` shows only supervisord's process
transitions. Grepping the wrong file prints nothing and reads as "the worker
says nothing" — which is how this was misdiagnosed twice before the log was
finally captured.

---

## Resolution, and the three defects the fix uncovered (2026-08-21)

Option 1 was chosen and implemented: `CbOffererDriver::Rearm()` plus
`CloudBrowserBrowserMainParts::RearmSession()`. **The re-armable driver alone
did not fix the user-visible problem.** Three further defects surfaced only
against a live browser, and each one was invisible in the logs in a different
way. They are recorded here because each was expensive to find and none would
have been caught by a build, a unit test, or the in-cluster suite.

### 1. A successful re-arm immediately undid itself

`RearmSession` → `Rearm()` → `CloseInternal()` → our own `OnClosed` → posts
*another* `RearmOrShutdown`. Two entries ~450 µs apart; the second was
correctly refused by the driver and fell through to `Shutdown()`.

```
083029.951776  CV2-REARM: driver returned to kIdle
083029.956753  CV2-REARM: rebuilt
083029.957207  CV2-REARM: rebuilding the peer connection      <- second entry
083029.957320  re-arm failed -> falling back to process exit
```

The log said "rebuilt" and the process died anyway. Fixed with a `rearming_`
latch mirroring `tearing_down_`.

### 2. A clean client close delivers TWO byes

The client sends its own `bye` and *then* drops the socket; `unregister` then
synthesised a second one because it checked only whether the peer had
negotiated, not whether it had already said goodbye. **~49 ms apart.**

Harmless for this code's entire life — a `bye` meant "exit", and the process
was already going down. Fatal the moment the worker re-arms: bye #1 rebuilt the
session in ~7 ms, bye #2 closed the rebuild, and the still-pending
`OnRenegotiationNeeded` landed in `kClosed` → `FailWithReason` → `kFailed` →
`Rearm()` refuses → exit.

Fixed in both halves: `peer.saidBye` in the broker, and `HandleByeEnvelope`
dropping a `bye` that arrives in `kIdle`/`kCreatingPc` — the exact window a
re-armed driver occupies.

### 3. Cold arrival: nothing makes a live worker offer to a NEW viewer

**This one took three more attempts after the first fix, and each attempt was
only reachable because the previous one worked.** The sequence is worth reading
as a whole, because every step looked like "the fix didn't work" and was
actually a different defect underneath:

| attempt | what happened | the real defect |
| --- | --- | --- |
| guard `kIceInFlight` | nothing fired at all | a re-armed driver sits in **`kAwaitingAnswer`**, not `kIceInFlight` — the request fell into the mid-dance coalesce and waited for an answer that could never come |
| guard `kAwaitingAnswer` | fired, rebuilt in 9 ms, viewer went `connecting` → **`closed`** | `Rearm()` emits a `bye`, the broker forwards it to the waiting viewer, and its client calls `teardown("peer said bye")` — killing the peer being rebuilt for, ~1 ms before the offer landed |
| `announce_bye=false` | **passes** — 34 frames, browser pid unchanged | — |

The state-machine mistake is the instructive one: the enum's comment describes
`kIceInFlight` as "steady state", which is true for a *connected* session and
false for a re-armed one. Read which state the code actually reaches, not which
state the comment calls normal.



This is the one the user actually hit, and re-arm does **not** address it,
because the trigger for re-arm was always *the previous viewer leaving*.

```
09:37:58  worker re-arms; its offer is buffered by the broker
...       >5 minutes idle
09:43:19  a NEW viewer joins. The broker CORRECTLY drops the buffered offer
          (age 322s > the 300s cap). Nothing is replayed.
+45s      the client's offer watchdog sends request_renegotiate
09:44:49  the only thing that ever produced an offer was the viewer GIVING UP
```

The worker sat in `kIceInFlight` **from the previous viewer's session**, so
`request_renegotiate` took the `BeginRenegotiation` branch — which re-offers on
the *existing* PeerConnection, whose ICE ufrag/pwd and DTLS fingerprint belong
to a peer that is gone. That is exactly the mismatch that presents as
`iceConnectionState=failed` with relay candidates on **both** sides.

Fixed by tracking whether the *current* offer was ever answered
(`current_offer_answered_`, per-offer — distinct from the
`was_in_ice_flight_once_` latch, which deliberately survives re-arm). An
unanswered offer means the asker is not the peer we are negotiating with, so
the driver raises `OnNewViewerNeedsOffer` and the embedder builds a fresh PC.

⚠ `CbAudioLifecycle` sits BETWEEN the driver and the embedder and had to
forward the new callback explicitly. An un-overridden observer method is
swallowed by the base class's empty default — **no error, no log line**, and
the fix would simply have done nothing.

## What the pid proves that video does not

A worker RECYCLE also gives viewer 2 working video. Video alone cannot
distinguish re-arm from restart, and for weeks the logs said "rebuilt" while
the browser was dying on every viewer change. The discriminator is the chromium
**pid inside the worker pod**, and the remote browser still being on the page
the previous viewer left it on. Both are asserted in
`tests/local/rearm-scenarios.spec.ts`.

Matching that pid is fussier than it looks: `pgrep -o -f chrome` returns the
supervisord *wrapper* (`launch-chromeless.sh`), which respawns on every restart
and would make the assertion vacuously true.

# A broker restart restarts the worker: the signaling client never redials

**Status:** FIXED and VERIFIED LIVE (2026-09-08, guest `cr7727-0f336fbd1c7d`
plus broker `batchr-ff7e944`). Kept for the diagnosis; see the bottom.

**Was:** OPEN. Measured 2026-09-07 on the k8s standalone stack, worker
`cr7727-c5f2eb91c6f0` (image digest `5813600c…`), three broker rollouts in one
afternoon. Lives in `capture/`, so it cannot be fixed from this repo without a
lane build.

**Impact for a standalone deployment:** every broker redeploy costs the
browser its process about 70 seconds later — open tabs, history, in-memory
logins — and any viewer attached at the time watches a live stream turn into a
fresh browser. A viewer who connects during those 70 seconds waits at
"waiting for offer" with nothing on the other end. The stack whose point is
"the browser outlives its viewers" does not outlive its own broker.

## What happens

The worker's WebSocket to the broker (`capture/signaling/cb_signaling_ws_client`)
is dialled exactly once per process. When the broker pod goes away the client
reports the drop and stops:

```
16:13:04  ReplicaSet chromeless-standalone-signaling-ffffc86c7 created   (the rollout)
16:13:08  CV2-69 ws_client: transport/handshake/codec error: channel dropped
          unexpectedly: code=1006 reason= — client is half-broken; offerer
          driver should ...
          ...nothing further from cb_signaling for the life of the process.
16:14:20  Liveness probe failed → Killing: Container chromium failed liveness
          probe, will be restarted
16:14:23  new process: cb_signaling: dialing ws://chromeless-standalone-signaling:8080/...
```

`grep -c 'cb_signaling: dialing' chromium.err.log` is **1 per process**. The
liveness probe in `infra/k8s/standalone/stack.yaml` exists precisely because of
this — its own comment says the WS client has no reconnect and that a restart is
"~15s, and invisible to a viewer". It is not invisible: WebRTC media does not
need the signaling channel, so an attached viewer keeps streaming for the whole
72 s and then loses the browser when the container is killed.

The mechanism has been known since the reconnect layer was written. R7
(`capture/signaling/cb_signaling_reconnect.{h,cc}`, `source_set(":cb_signaling_reconnect")`)
implements exactly this — exponential backoff redial, bounded attempts, idle
timeout, `OnGaveUp` — and **nothing constructs it**:

```sh
grep -rn "cb_signaling_reconnect" capture/ --include=*.cc --include=*.h --include=*.gn
# BUILD.gn (the target), cb_signaling_reconnect.cc/.h themselves — no caller.
```

`capture/signaling/BUILD.gn:421` records that its test "will be added in a
follow-up commit". It never was: a source set with no test and no consumer,
compiled by the lane and exercised by nothing. CLAUDE.md's rule applies — a TODO
that names its own verification step is a defect nobody has run yet.

## Why it looks like something else

- **In Kubernetes it reads as a crashing worker.** The pod stays `Running`,
  `restartCount` climbs, and the event is `Liveness probe failed` — the natural
  reading is a resource or stability problem in Chromium. The tell is the
  timing: every kill trails a broker ReplicaSet creation by ~72 s (3 × 30 s
  probe period, first failure at the drop).

  ```sh
  kubectl get rs -n chromeless -l app.kubernetes.io/name=chromeless-standalone-signaling \
    --sort-by=.metadata.creationTimestamp -o custom-columns=T:.metadata.creationTimestamp,N:.metadata.name
  kubectl get events -n chromeless --field-selector reason=Killing \
    -o custom-columns=FIRST:.firstTimestamp,LAST:.lastTimestamp,N:.count,MSG:.message
  ```

  Measured: rollouts at 15:21:58, 15:39:16, 16:13:04 UTC; kills at 15:23:20,
  15:40:52, 16:14:20.

- **In the worker log it reads as a broker fault.** The last signaling line is
  the 1006 with "client is half-broken", which points at the other end. The
  broker was fine; it had simply been replaced.

- **The next viewer's failed first attempt reads as an ICE problem.** After the
  container restart the fresh worker joined a session where a viewer had been
  waiting; the broker replayed that viewer's buffered `request_renegotiate`
  (`replayed: 1`), the driver treated it as "a new viewer never received our
  offer" and re-armed, and the viewer's `answer` to the *first* offer then
  landed in `kCreatingPc`:

  ```
  FAIL state=CreatingPc reason=`answer` envelope in unexpected state
  CV2-GPU-DEATH: ... signalling session-unhealthy
  exited: chromium (exit status 0; not expected)      ← supervisord respawn, 2 s
  ```

  The second boot (`replayed: 9`) negotiated normally. So a broker restart
  costs two worker processes, not one, and the `session_unhealthy` envelope it
  sends on the way out is dropped by the standalone broker as an unknown type
  (`signaling/server.go` `validTypes`; in triform, physics consumes it).

## The fix

1. **Wire R7.** In `CloudBrowserBrowserMainParts::StartNativeSession`
   (`capture/build-integration/cloud_browser_browser_main_parts.cc`, the
   `ws_client_ = std::make_unique<SignalingWsClient>(…)` at ~line 996), construct
   the reconnect supervisor around the client instead of calling
   `ws_client_->Connect()` directly, with the embedder as its consumer. The
   header already specifies the contract: one `OnConnected` per recovered
   channel, backoff reset on success, `OnGaveUp` after `max_attempts`.
2. **Re-drive on reconnect.** In the embedder's `OnConnected`, if
   `offerer_driver_` is in a connected state, do nothing — media is alive and
   the broker only needs the registration frame. Otherwise call
   `RearmOrShutdown(/*announce_bye=*/false)` so the new broker is offered to
   immediately; `announce_bye=false` because there is no old session on this
   broker to end (`cb_offerer_driver.cc` `Rearm` documents both choices).
3. **Write the missing test** (`cb_signaling_reconnect_test.cc`), declare it in
   `BUILD.gn`, and add it to the lane manifests so `make lint-build-targets`
   stops being the only thing that would notice.
4. **Then relax the liveness probe** in `stack.yaml` (or drop it for the
   readiness probe alone). Until step 1 lands it must stay: without it a
   dropped socket is permanent for the life of the process.

Separately, in the driver's `request_renegotiate` handling ("while AWAITING AN
ANSWER" branch of `cb_offerer_driver.cc`): a replayed request older than the
current offer should not trigger a re-arm. The broker can tell (the request was
buffered before the browser joined); the envelope cannot.

## Verifying it

`tests/local/` already has the re-arm scenarios. Add one: with a viewer
streaming, `kubectl rollout restart deploy/chromeless-standalone-signaling`
(or `docker compose restart signaling`) and assert, over the next 120 s, that
the worker pid is unchanged, `cb_signaling: dialing` appears a second time in
its log, and the viewer's `framesDecoded` keeps increasing. Today that scenario
fails at the pid check every time.

---

## Fixed, 2026-09-08

Two halves, because the first alone was not enough.

**Guest (`CV2-REDIAL`).** `main_parts` now owns `CbSignalingReconnect` — the
R7 wrapper that had been compiled by every lane and constructed by nothing —
instead of the bare ws client. It IS-A `SignalingTransport`, so the driver's
raw pointer survives a redial. Two bugs in the wrapper that six months of
disuse had hidden: a clean 1000 close from a broker shutting down was treated
as consumer-driven and went terminal (exactly the rollout case), and nothing
distinguished our own `Disconnect`. Both fixed, and the tests the BUILD.gn
TODO had promised since M3 now exist (`cb_signaling_reconnect_test.cc`, in the
t7 lane).

**Broker.** With the guest redialing correctly the attached viewer STILL lost
video, because `unregister` synthesised a `bye` to every counterpart as the
process shut down — every socket closing at once is indistinguishable from
every peer leaving. `hub.shuttingDown` is set before `http.Server.Shutdown`
and suppresses that (`TestWS_ShutdownDoesNotSynthesiseByes`, watched fail).

Measured with a viewer attached across a broker rollout:

```
10:42:09  viewer attached, 14 frames
10:42:09  kubectl rollout restart deploy/chromeless-standalone-signaling
   +20s   framesDecoded=77        pid unchanged
   +41s   framesDecoded=281
  +103s   framesDecoded=893
          dials 1 -> 3, two CV2-REDIAL lines, no liveness kill
  +203s   pid unchanged, same pod; next viewer connected and got video
```

Before: the socket died at 1006, nothing redialed, and the liveness probe
killed the container ~70 s later. The probe in `stack.yaml` can now be relaxed
or dropped — it was standing in for this.

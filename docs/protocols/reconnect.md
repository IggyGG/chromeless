# Reconnect protocol (v1)

How `chromeless` recovers from signaling websocket drops and
ICE failures without making the user reload the page.

> **Status:** v1, lands with T37. The signaling-server protocol gains
> one new envelope type (`request_renegotiate`); everything else is
> client-side machinery.

---

## Goals

- A user whose laptop wakes from sleep, suspends Wi-Fi, or whose
  signaling server pod restarts gets back to a working session
  automatically within ~30s, without page reload.
- A user behind a flaky NAT whose ICE pair stops working triggers an
  ICE restart on the offerer (the streamer) and reconnects.
- The server-side signaling state machine stays unchanged — it remains
  a stateless two-peer relay; sessions are still identified solely by
  `session_id` in the URL.

## Non-goals

- Buffering input events while disconnected. Inputs queued on a closed
  data channel are dropped on the floor (see the existing T20
  closed-channel drain). UX-wise, the cursor is frozen between drop
  and recovery.
- Migrating media across reconnect transparently. Each rebuild yields
  a fresh `RTCPeerConnection`; the browser DOM `<video>` element
  switches `srcObject` when the new track arrives.

---

## Wire format additions

A single new envelope type joins the four from T13:

```jsonc
{ "type": "request_renegotiate", "from": "client" | "browser", "data": null }
```

| field  | value                       |
|--------|-----------------------------|
| `type` | `"request_renegotiate"`     |
| `from` | sender role                 |
| `data` | `null`; reserved for future |

Semantics (from the receiving peer's perspective):

- **Receiver is the offerer (the streamer):** call
  `pc.createOffer({ iceRestart: true })`, set local description, send
  the resulting `offer` envelope back through signaling.
- **Receiver is the answerer (the client):** no-op. The client cannot
  initiate; it just logs and waits for the streamer's fresh offer.

The signaling server forwards the envelope verbatim like any other
message (added to `validTypes` in `signaling/server.go` per T37).

---

## Client state machine

The signaling websocket has its own state, separate from
`RTCPeerConnection.connectionState`:

```
        connect()
   ┌───────────────────────────►  connecting  ───────► open ─────► connected
   │                                  │                              │  │
   │                                  ▼ open fails                   │  ▼ underlying close
   │                              reconnecting ◄───── timer ───── reconnecting
   │                                  │                                  │
   │                                  ▼ attempts > maxAttempts            │
   │                                failed                                │
   │ close()                                                              │
   ▼                                                                     ▼
 closed ◄──────────────── close() ────────────────────────────────── closed
```

States:

- **idle** — wrapper constructed, `connect()` not yet called.
- **connecting** — first attempt's WebSocket constructor returned;
  awaiting `open`.
- **connected** — underlying socket emitted `open`. `send()` works.
- **reconnecting** — underlying socket emitted `close` (or constructor
  threw) and we are waiting out the backoff before retrying.
- **failed** — `maxAttempts` exhausted. No further automatic retries;
  the user must manually `connect()` again or reload.
- **closed** — `close()` was invoked (terminal).

### Backoff schedule

`initialBackoffMs * (backoffFactor ** (attempt - 1))`, capped at
`maxBackoffMs`. Defaults: 1000ms, ×2, capped at 30000ms. So:

| attempt | wait (ms) |
|---------|-----------|
| 1       | 1000      |
| 2       | 2000      |
| 3       | 4000      |
| 4       | 8000      |
| 5       | 16000     |
| 6+      | 30000 (capped) |

`attempt` resets to 0 on every successful connect.

### Successful reconnect

When the websocket reaches `connected` for the second (or later) time
in the same `ReconnectingWebSocket` lifecycle, the **client**:

1. Tears down the previous `RTCPeerConnection` (close, drop tracks).
2. Builds a fresh `RTCPeerConnection` with the same ICE config.
3. Sends the hello frame (`{type: "ice", data: null}`) so signaling
   re-learns the role.
4. Awaits a fresh `offer` from the streamer.

The streamer is expected to do the symmetric thing on its end (its own
reconnect logic, T37 follow-up on the streamer side): on websocket
re-open, create a new `RTCPeerConnection`, re-attach the existing
`MediaStream`, `createOffer()`, send.

### ICE failure path

When `pc.iceConnectionState === "failed"` fires on the client:

1. The client emits `request_renegotiate` over the signaling
   websocket. If the websocket isn't `connected`, the send is dropped
   (caller logs); the next reconnect cycle handles it.
2. The streamer (offerer) receives `request_renegotiate`, calls
   `createOffer({ iceRestart: true })`, sends the fresh offer.
3. The client's existing offer handler picks it up, re-answers, and
   ICE candidates trickle on the new SDP credentials.

The `RTCPeerConnection` itself is **not** torn down on ICE failure —
ICE restart is supposed to recover the same pc. The pc is only torn
down when the signaling websocket itself reconnects (because at that
point we cannot trust state alignment with the streamer).

---

## Server changes

`signaling/server.go::validTypes` gains `"request_renegotiate"`. The
server forwards the envelope verbatim to the other peer in the
session, like every other type. No state, no special-casing.

A test in `signaling/turn_test.go`-style table format is not added for
this single map entry; it's covered by the existing forwarding test
in `tests/integration/signaling-roundtrip` (T27).

---

## Files

- `client/src/reconnect.ts` — `ReconnectingWebSocket` class +
  `requestIceRecovery` helper.
- `client/src/reconnect.test.ts` — vitest unit tests with manual
  clock and fake socket.
- `client/main.ts` — uses `ReconnectingWebSocket`; wires
  `iceConnectionState=failed` to `requestIceRecovery`; rebuilds the
  peer connection on every signaling reconnect.
- `signaling/server.go` — `validTypes` includes
  `request_renegotiate`.

## Testing notes

- Unit tests cover backoff timing (1s/2s/4s, capped), state machine
  transitions, send return-values, `close()` cancelling future
  retries, and the `request_renegotiate` envelope shape.
- Manual integration test (run during T37 verification): start the
  signaling server + client, kill -9 the signaling server, restart
  it within ~10s, observe the client transitions
  `connected → reconnecting → connected` and a fresh offer/answer
  cycle without page reload.
- ICE failure simulation is harder and lives under T37 follow-up
  (would need to drop UDP at the netfilter layer mid-session).

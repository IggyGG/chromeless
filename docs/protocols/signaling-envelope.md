# Signaling envelope (v1, two dialects)

The JSON frames exchanged over the signaling WebSocket between the browser
peer (cb-chromium) and the client peer.

> **Status:** v1. `capture/signaling/cb_wire_envelope.h` remains
> authoritative — this document describes it, and if the two disagree the
> header wins. Written 2026-07-30 when the decoder learned a second dialect,
> because "read the C++" stopped being reasonable advice for someone
> implementing a peer.

---

## Canonical dialect

```json
{ "type": "offer", "from": "browser", "data": { "type": "offer", "sdp": "v=0..." } }
```

Three fields. `from` is a **sibling** of `type`, never nested under `data`.

**Tags** — the complete list:

| tag | `data` |
| --- | --- |
| `offer` / `answer` | `{type, sdp}` — `data.type` MUST equal the envelope tag |
| `ice` | `RTCIceCandidateInit`, **or JSON `null`** for end-of-candidates |
| `bye` | field **omitted entirely** — not present, not null, not `{}` |
| `request_renegotiate` | `null` (an absent field is also accepted) |
| `probe_result` | opaque object; receive-only, the browser peer never emits it |
| `session_unhealthy` | field omitted, like `bye`. Sent by the guest on permanent renderer/GPU death so the orchestrator recycles it |
| `peer_absent` | field omitted. **Broker → peer, advisory.** Sent once, at join, when no peer holds the counterpart role in this session |

**`from`** is `"browser"` or `"client"`. Any other value rejects the frame.

Emitters in this dialect: this repo's C++ peer, its TypeScript client, and
physics's `SignalingEnvelope` (whose `#[serde(rename_all="snake_case")]`
produces byte-identical tags).

## Portal dialect (decode-only)

The triform portal speaks a flat form, documented at
`portal/src/canvas/browser_screencast_webrtc.rs:48-53`:

```json
{ "type": "sdp_offer",     "sdp": "v=0..." }
{ "type": "sdp_answer",    "sdp": "v=0..." }
{ "type": "ice_candidate", "candidate": "...", "sdpMid": "0", "sdpMLineIndex": 0 }
```

Three differences, each of which the canonical decoder rejected on its own:
a different tag, **no `from`**, and the payload inlined as siblings of `type`
rather than nested under `data`.

A conforming peer **accepts** these on decode and normalizes them to the
canonical structure:

- `from` is inferred as `client`. That is a statement about who speaks this
  dialect — the portal is the browser-facing UI and the only producer — not
  a default. A future flat-but-not-client producer must send canonical.
- `data.type` is synthesized from the tag, so downstream code sees one shape
  regardless of which dialect arrived.
- An `ice_candidate` frame with **no `candidate` field** is the
  end-of-candidates marker. Canonical expresses that as `data: null`, which
  has no flat equivalent because the payload *is* the frame.

**Encoding is always canonical.** There is no portal-dialect emitter: a peer
that chose its dialect per-recipient would have to know which peer it is
addressing, which the codec deliberately does not.

## The accept-list is closed

Widening to the portal dialect added exactly three tags. Everything else —
`hello`, `restart_ice`, `candidate`, anything else that looks plausible —
**must still be rejected**, including when wrapped in an otherwise
well-formed envelope.

This is the load-bearing half of the contract, and the one a new
implementation is most likely to get wrong: accepting an unknown tag is
invisible until it meets a peer that assumes the documented behaviour.
`conformance/run.mjs --role=signaling` checks it.

## `peer_absent`: the broker is the only one who knows

Sent to a joining peer when the counterpart role is unoccupied. It is
**advisory** — it carries no payload, is not in `validTypes`, is never
forwarded, and a peer that does not recognise it drops it as an unknown type
(which is the documented forward-compatibility behaviour, so it is safe to
send to any client).

**The browser peer must accept it too.** The broker sends it to whichever
role joins alone — and on every cold boot that is the browser, whose offer is
then buffered for the first viewer. From 2026-08-25 to 2026-09-07 the C++ peer
rejected the tag per the closed accept-list, and `SignalingWsClient` treats a
decode rejection as a transport failure: the guest closed its own signaling
socket on the first frame it received, on every boot where no viewer was
already waiting. E2E was red for two weeks and the standalone stack worked
only if the page was open before the worker started. The guest now decodes
`peer_absent` (monostate, `data` omitted, `from` = the recipient's own role)
and logs it. An advisory that is "safe to send to any peer" has to be one the
peer survives; a codec that must reject unknown tags needs the ws client to
treat rejection as *drop the frame*, not *drop the socket* — that half is
still open.

It exists because being alone in a session is invisible from the inside. A
client on the wrong session id has an open socket, accepted auth, a delivered
ICE config and a green probe; it simply never receives an offer. Before this
envelope the only signal was the client's own offer watchdog, **65 seconds**
later, in a log. The observed case was a stray keystroke turning `dev` into
`devs`.

Sent at most once per join, and **only** when the counterpart is genuinely
absent. A notice that fired unconditionally would be noise, and noise trains
people to ignore the one time it matters — `TestWS_LonePeerIsToldTheCounterpartIsAbsent`
asserts both halves for that reason.

Receiving it does not close the session: the peer may legitimately be early,
and the counterpart can still join afterwards. It is a diagnosis, not a
verdict.

## Replay buffer lifetime: a `bye` is not guaranteed

The broker buffers the most recent `offer`/`answer`/`request_renegotiate` per
role (T96) plus a bounded ICE queue (T104), and replays them when the
counterpart joins. That fixes the race where the browser peer offers before any
viewer is connected.

Those buffers are dropped:

- when a peer **disconnects**, for its own buffer — its SDP and candidates
  describe a peer connection that went away with it;
- on **`bye`**, for both roles — a bye ends the negotiated session, not just
  the sender's half;
- when a peer that had **negotiated** disconnects, for both roles — even with
  no bye.

That last rule exists because **a `bye` frequently never arrives.** The web
client sends one from `beforeunload`, which does not fire reliably when a
browser context is closed programmatically, and a lid-close, process kill or
network drop never sends one at all. Measured 2026-08-19 across a full
Playwright run against a real worker: **zero** byes reached the broker. The
worker's offer therefore survived every viewer disconnect and was replayed to
the next one, which answered SDP whose peer connection had already been torn
down — `iceConnectionState` stuck at `checking`/`connecting` forever, with one
unresponsive candidate pair. It looks exactly like a NAT or TURN failure.

**"Negotiated" is required, and means SDP was exchanged** — this peer was
replayed the counterpart's SDP on join, or sent an `offer`/`answer` itself. ICE
alone does not count. A viewer that connects and drops again without exchanging
SDP (a refresh, a probe, a health check) must NOT invalidate the buffer, or the
T96 race it exists for is broken and *every* viewer fails instead of every
viewer after the first.

Implementations that keep their own replay buffer must apply all three rules.
Dropping only on `bye` is the shape that shipped and was wrong.

## A `bye` ends the session, not the socket

A peer that sends `bye` stays connected. The broker keeps reading from it, and
the same socket may carry a new `offer`/`answer` for the next session; a peer
that is actually leaving closes its socket itself (the web client does, and so
does the worker's exit path).

This matters for the re-armable worker. On a viewer that vanished without a
`bye` — ICE `failed` for the worker's grace period — the worker announces a
`bye` for the dead session and immediately offers again **on the same
socket**. The broker used to close the sender's socket after its `bye`, which
was right when a `bye` meant "I am leaving" and wrong for a peer that is
staying: the worker's next `offer` failed to send, its driver went to
`failed`, and the process recycled itself. Observed live 2026-09-07:

```
CV2-BYELESS: ICE still failed after 20s — re-arming
ws_client: channel dropped unexpectedly: code=1006
FAIL state=CreatingOffer reason=ws Send(offer) failed
unrecoverable failure — recycling this guest
```

So every byeless disconnect cost a process restart, on a stack whose point
is that the browser outlives its viewers. (The remote-`bye` re-arm never hit
this because the worker skips its own `bye` when the close was remote.)

Three bookkeeping rules a broker must get right:

- a `bye` still drops both replay buffers and still suppresses the synthesised
  `bye` at unregister time (`peer.saidBye`) — that flag is **cleared by the
  peer's next `offer`/`answer`**, so if the *new* session drops without a
  `bye` the counterpart is still told;
- the `bye` is forwarded to the counterpart exactly as before. A viewer whose
  ICE died but whose socket lives receives it and tears down; the worker's
  fresh `offer` then reaches whoever joins next;
- **a `bye` from a peer that was itself sent a `bye` for this session is an
  echo, and is dropped** — and so is the synthesised one if that peer's socket
  then closes, and its closing discards only its *own* replay buffer. The web
  client's teardown answers a `bye` with a `bye`; the re-armed worker has
  re-offered within ~40 ms of its own, so the echo would close the session it
  just rebuilt (measured live 2026-09-07 on the first broker that kept the
  socket open: two rebuilds per byeless loss), and discarding both buffers on
  it erased the fresh offer the next viewer needed. The receiver-side flag is
  cleared by that peer's next `offer`/`answer`, so a `bye` for the *new*
  session is never suppressed.

Peers written against the old behaviour that *waited* for the broker to close
them after a `bye` would now wait forever: close your own socket when leaving.

**A broker that is itself shutting down must synthesise nothing.** Every
socket closes at once during a rollout, which is indistinguishable from every
peer leaving — but the sessions are not over: media rides the peer
connections, which do not touch this socket, and the peers need signaling back
only to renegotiate later. Measured 2026-09-08 with a worker that redialed
correctly (back in 1 s, same process): the attached viewer still lost its
video on every broker restart, because the broker told it the session had
ended on the way out. `signaling/server.go` sets `hub.shuttingDown` before
`http.Server.Shutdown`, and `unregister` skips the synthesised `bye` while it
is set (`TestWS_ShutdownDoesNotSynthesiseByes`).

`signaling/replay_test.go` `TestWS_ByeKeepsSenderConnected` and
`TestWS_ByeEchoIsNotForwarded` pin this; each was watched fail against the
behaviour it replaced.

## Known gap: the physics translator is lossy

Today the portal and the browser peer are bridged by
`pattern_c_envelope_to_portal_msg` (`physics/.../screencast_ws.rs:6909`),
which maps offer/answer/ice between the two forms and returns `None` for
**`bye`, `request_renegotiate`, `probe_result` and `session_unhealthy`** —
so those four never reach the portal at all.

Teaching this decoder the flat dialect does **not** by itself let that
translator be deleted; the portal would first have to learn the four missing
tags. They are two separate changes, and conflating them would silently drop
frames.

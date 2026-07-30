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

# ICE / STUN / TURN

How `cloud-browser-webrtc` handles NAT traversal — what we ship today,
what we deploy when we go off-LAN, and where the wiring will plug in.

## Background, briefly

WebRTC negotiates a peer-to-peer media path through the **ICE**
algorithm (RFC 8445). Each side gathers candidate addresses, exchanges
them via signaling, and probes pairs until one works. Three flavours
matter:

- **host** candidates — the local interface IPs.
- **server-reflexive** candidates — discovered by asking a STUN server
  what your public IP/port look like from the outside (RFC 8489).
- **relayed** candidates — allocated on a TURN server which then forwards
  media (RFC 8656). TURN is an actual relay, not an introduction; it
  costs CPU and bandwidth.

For most home/office networks, host + server-reflexive candidates are
enough. For the meaningful fraction of users behind symmetric NAT,
double-NAT, hostile corporate firewalls, or strict CGNAT, only TURN
gets you through. TURN-over-TLS on 443 (`turns:`) is the universal
fallback when UDP is blocked outright.

## What we ship in v1

- **STUN-only by default.** `/turn-credentials` returns Cloudflare and
  Google public STUN endpoints. No allocation, no auth, no cost.
- **TURN is opt-in via env vars.** Setting `TURN_URLS`, `TURN_USER`,
  `TURN_PASS` on the signaling container enables a TURN entry in the
  response. v1 uses **static long-term credentials** — fine for a single
  internal deployment, not safe for shipping browser-side to the world.
- **Browser fallback.** If `/turn-credentials` is unreachable or
  malformed, the client falls back to public STUN and logs a console
  warning. The connection then works on LAN and breaks on hostile NATs.
- **No `iceTransportPolicy` enforcement.** We don't force-relay; we let
  ICE pick. Phase 3 may flip this on a per-tenant basis when we want
  guaranteed media-path control.

### Wire format

The endpoint mirrors the W3C `RTCConfiguration.iceServers` shape one to
one, so the browser pastes the response straight into
`new RTCPeerConnection(cfg)`:

```jsonc
{
  "iceServers": [
    { "urls": ["stun:stun.cloudflare.com:3478"] },
    { "urls": ["turn:turn.example.com:3478"], "username": "...", "credential": "..." }
  ]
}
```

`urls` is always an array (the spec also accepts a bare string; we always
emit the array form for consistency).

## Operational story

### "Should we run our own TURN?"

Yes, eventually. The build-vs-buy question is:

| option | when it makes sense |
|--------|---------------------|
| **Public STUN only** (today) | Internal LAN, dev, watch-party-style demos. Cheap, zero ops. |
| **Cloudflare Calls** / metered.ca / Twilio NTS | Phase 1–2 hosted demo with public users. Fastest path to TURN-on-443. Per-minute pricing. |
| **Self-hosted coturn** | Phase 3+, scale economics flip and per-tenant credentialing matters. Run alongside the orchestrator; same region as the cloud-browser pool. |
| **Self-hosted Pion-based TURN** | Niche; only if we want TURN policy embedded in the Go signaling binary. Probably not — coturn is battle-tested. |

The plan: ship STUN-only for v1 / Phase 0, layer Cloudflare Calls or
metered when we open a public demo, migrate to coturn when traffic and
costs justify it.

### Credential refresh (the part v1 punts on)

Long-term static credentials in the browser are fundamentally insecure
because every visiting client sees them. For Phase 3 we move to
**TURN-REST** (RFC 7635 / draft-uberti-rtcweb-turn-rest):

- Server holds a single shared secret with coturn (`use-auth-secret` mode).
- Each request to `/turn-credentials` (now authenticated against the
  signaling tenant auth) returns:
  ```
  username = <expiry-unix-ts>:<tenant-id>
  credential = base64(HMAC-SHA1(shared_secret, username))
  ```
  with `<expiry-unix-ts>` typically `now + 24h`.
- The browser keeps using these credentials until expiry; on `iceconnectionstate=failed`
  the client re-fetches and `pc.setConfiguration(...)` re-applies.

The current `signaling/turn.go` is structured so this drops in: replace
`buildICEConfig`'s static credential lookup with HMAC derivation, keep
the `iceServer` shape unchanged. The browser's `fetchTurnConfig` already
caches per session, so the call site does not change.

### CORS

`/turn-credentials` returns `Access-Control-Allow-Origin: *` so the
browser can fetch it from a different origin than the signaling host
(static dist on `:5173`, signaling on `:8080`). Phase 3 narrows this to
the configured client origin once we have one.

## Failure modes and what to look for

- **`iceConnectionState` stays `checking` forever.** All STUN peers
  succeeded but no candidate pair connects. Symmetric NAT on at least
  one side; need TURN.
- **`iceConnectionState: failed`.** Candidates were gathered but every
  pair failed connectivity check. Same diagnosis: TURN required, or the
  TURN server itself is unreachable.
- **`/turn-credentials` 5xx during page load.** Client falls back to
  public STUN; surfaces in the browser console. Phase 3: surface as a
  toast in the UI.
- **`turn:` URL with no `?transport=`.** Most TURN servers default to
  UDP; if the user's egress blocks UDP, ICE silently skips this entry.
  Always also publish `turn:host:port?transport=tcp` and ideally
  `turns:host:port` (TURN-over-TLS) — the client fans out automatically.

## Files

- `signaling/turn.go` — endpoint + `buildICEConfig` (env-driven).
- `signaling/turn_test.go` — table tests covering STUN-only, TURN, and
  malformed env.
- `client/src/turn.ts` — `fetchTurnConfig` with same-origin/ws://-base
  resolution and STUN-only fallback.
- `client/src/turn.test.ts` — fetch path + URL resolution tests.
- `client/main.ts` — calls `fetchTurnConfig` before
  `new RTCPeerConnection`.

## References

- [RFC 8445](https://www.rfc-editor.org/rfc/rfc8445) — ICE.
- [RFC 8489](https://www.rfc-editor.org/rfc/rfc8489) — STUN.
- [RFC 8656](https://www.rfc-editor.org/rfc/rfc8656) — TURN.
- [draft-uberti-behave-turn-rest](https://datatracker.ietf.org/doc/html/draft-uberti-behave-turn-rest-00) — the de-facto TURN-REST spec.
- coturn `use-auth-secret` mode: <https://github.com/coturn/coturn/wiki/Authentication-mechanisms>.

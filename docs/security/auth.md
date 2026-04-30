# Signaling auth (T48 / Phase-3 prep)

How the cloud-browser-webrtc signaling server authenticates websocket
connections. Lands in T48 as a Phase-3 enabler — Phase 0/1 deployments
typically run with auth disabled.

## TL;DR

- Tokens are **JWT-shaped** (`header.payload.signature`) using
  **`alg=EdDSA`**. The header is fixed: `{"alg":"EdDSA","typ":"JWT"}`.
- The server reads its **Ed25519 public key** from
  `CBWRTC_AUTH_PUBKEY` (base64-encoded raw 32-byte key). If the env
  var is unset, **auth is disabled** and the server logs a clear
  warning at startup.
- The browser sends the token as `?token=...` on the websocket URL.
  This is the only transport that works in every WebSocket client we
  care about.
- On bad / missing / mismatched tokens, the server closes the
  connection with code `1008` (Policy Violation) and a short reason
  string. Each rejection increments
  `cb_signaling_auth_failures_total{reason="..."}`.

## Token claims

```jsonc
{
  "sub":  "tenant-id",            // string, required
  "sid":  "session-id",            // must equal the path segment of /ws/{sid}
  "role": "client" | "browser",    // must equal the `from` of every envelope
  "exp":  1700001000,              // unix-seconds; rejected once now >= exp
  "iat":  1700000000,              // informational
  "nbf":  1700000000               // optional; rejected if now < nbf
}
```

| reason label    | trigger                                             |
|-----------------|-----------------------------------------------------|
| `missing`       | `?token=` empty                                     |
| `malformed`     | wrong number of dots, bad base64, bad JSON, wrong sig length |
| `bad_signature` | signature does not verify against `CBWRTC_AUTH_PUBKEY` |
| `expired`       | `exp` is in the past                                |
| `not_yet_valid` | `nbf` is in the future                              |
| `sid_mismatch`  | `sid` claim does not match the path segment         |
| `role_mismatch` | `role` claim does not match the connection's role   |
| `tenant_missing` | `sub` claim is empty                               |

## Tenant namespacing (T67)

Sessions are keyed by `(tenant_id, session_id)`, not by `session_id`
alone. The `tenant_id` comes from the verified token's `sub` claim
(see [Claims](#token-claims)).

What this gives us:

- Two tenants can both run a session called `demo` without
  cross-talking. The hub treats them as fully independent rooms.
- A token issued for `tenant-A` can never be used to read
  `tenant-B`'s offer/answer/ICE. Even if the URL paths are identical
  (`/ws/demo`), the server-side hub key differs.

What it doesn't give us:

- Resource isolation. Both tenants still share the same hub goroutine
  pool and the same ICE/STUN/TURN config. Phase 3 orchestration
  (separate K8s namespaces, separate TURN servers per tenant) is the
  durable answer.
- Quota or rate-limiting per tenant. Add in T67-followup if/when a
  noisy tenant becomes a real problem.

### Anonymous fallback

When auth is disabled (`CBWRTC_AUTH_PUBKEY` unset), every connection
is bucketed into `tenant_id = "_anonymous"`. This preserves the
pre-T67 single-namespace behaviour for dev / CI runs that don't run
the issuer. The startup warning log
(`auth disabled: CBWRTC_AUTH_PUBKEY is unset; any caller can connect`)
already telegraphs the implication; T67 doesn't add a second log,
just a metric label.

### Metrics cardinality

`cb_signaling_active_sessions{tenant}`,
`cb_signaling_active_connections{role,tenant}`, and
`cb_signaling_sessions_total{tenant}` carry the tenant id as a label.
To prevent Prometheus cardinality explosion (a malicious or buggy
client could cycle tenant ids), the server caps **distinct
non-anonymous tenants** at 100. The 101st distinct tenant and beyond
get bucketed under the synthetic `_other` label. The cap is in
`signaling/metrics.go::tenantLabelCap`; bump it when scaling demands
it (Phase 3 scale targets typically push this to a few thousand,
backed by a separate-process metrics aggregator rather than the cap).

The anonymous tenant is exempt from the cap, so an auth-disabled
deployment always has its own clean bucket.

## Revocation (T89)

Two layered mechanisms; both are on by default in any production
deployment that follows this guide. Either alone is enough for most
threat models.

### 1. Short-TTL refresh (the common case)

Issue tokens with `exp` 5–15 minutes out, not hours. Clients refresh
before expiry via `client/src/auth.ts::TokenRefresher`, which
re-calls `/issue-token` `refreshLeadMs` (default 60 s) before `exp`.

Operational story: revoking a tenant means **stopping the issuer
from minting fresh tokens** for them. Within one refresh cycle every
active session naturally falls off — no signaling-side state, no
denylist sync, no CRDT shenanigans. This handles the vast majority
of revocation cases (off-boarding, tenant disable, plan downgrade)
because in practice you control the issuer. The denylist below
exists for the cases where you need to break in-flight tokens
faster than the next refresh.

If the refresh fetch itself fails (issuer 5xx, transient network),
`TokenRefresher` retries every 30 s and keeps the previous token
live in the meantime — signaling will reject when the previous
token's `exp` actually elapses.

### 2. Denylist (the emergency case)

For "kill this tenant's connections within seconds, don't wait for
TTL," signaling consults a denylist on every authenticated connect.
Two backends:

- **`StaticDenylist`** — in-memory set, seeded from
  `CBWRTC_DENYLIST` (CSV of `tenant` or `tenant:jti` entries).
  Single-process, lost on restart. Fine for dev, CI, and small
  deployments where the admin endpoint is the only writer and the
  fleet is one signaling pod.
- **`RedisDenylist`** — shared set in Redis (key
  `cb:auth:denylist:tenants` for tenant-wide bans;
  `cb:auth:denylist:jtis` for per-token bans, value `tenant:jti`).
  Multiple signaling replicas converge on the same revocation
  state. Configured via `CBWRTC_DENYLIST_REDIS_ADDR`.

Lookup semantics:

- A tenant in `cb:auth:denylist:tenants` blocks every token for that
  tenant, regardless of `jti`. Use this for "this tenant is
  compromised" or "off-boarding."
- A `tenant:jti` in `cb:auth:denylist:jtis` blocks one specific
  token. Use when you want to surgically kill a leaked token but
  keep the tenant's other sessions alive.

Both lookups use a hard 100 ms timeout and **fail open** on Redis
errors — a Redis outage must not stall connect on every retry. The
counter `cb_signaling_auth_failures_total{reason="revoked"}`
exposes successful denylist hits; a separate WARN log captures
backend errors.

### Admin endpoint

`POST /admin/revoke` writes to whichever backend `initDenylist`
chose. Wire format:

```jsonc
POST /admin/revoke HTTP/1.1
Authorization: Bearer <admin-token>
Content-Type: application/json

{ "tenant": "alice", "jti": "abc123" (optional), "reason": "stolen" }
```

The admin token is signed by a **separate Ed25519 keypair**
(`CBWRTC_ADMIN_PUBKEY`, distinct from `CBWRTC_AUTH_PUBKEY`) and
must carry `role: "admin"`. The endpoint is registered only when
that pubkey env is set; in its absence the path returns 404.
Operationally:

1. Generate a fresh admin keypair (separate from session keypair).
2. `CBWRTC_ADMIN_PUBKEY=<base64-pub>` on the signaling deployment.
3. Run an issuer for admin tokens behind whatever gate your ops
   team uses (kubectl SSO, sudo workflow, etc.). The dev issuer
   does NOT mint admin tokens; that is deliberate.

`jti` omitted → tenant-wide ban; `jti` present → per-token ban.
Both paths return:

```jsonc
{ "ok": true, "tenant": "alice", "jti": "abc123", "scope": "jti" }
```

### Why both

Short-TTL covers the planned revocations and is zero-state on the
signaling side. The denylist is defence-in-depth for the moments
when "it's compromised, kill it now" matters more than "nice
property of statelessness." Either mechanism on its own would be
enough for most teams; both together is roughly as much complexity
as either alone and meaningfully tighter.

## Threat model

What this protects against:

- **Random session-id guessing.** Without the matching token, the
  server rejects the upgrade.
- **Role spoofing.** A token issued for `role:client` cannot be
  reused for the streamer side.
- **Cross-session token reuse.** A token for `sid: A` cannot connect
  to `sid: B`.

What this does NOT protect against (yet):

- **Compromised tokens.** Anyone with a valid token can connect
  until `exp`. There is no revocation list. Mitigation: short
  `exp` (we use 1 hour by default in the dev issuer, but production
  should consider 5–15 minutes with refresh).
- **Per-tenant key separation.** A single Ed25519 keypair signs all
  tenants' tokens. A leaked private key compromises everyone.
  Phase-3+ work: per-tenant key derivation or per-tenant signers.
- **Replay across IPs.** Tokens carry no client-IP binding.
- **TLS-level interception.** Use `wss://` and TLS terminator; the
  token is otherwise clear-readable in URL logs.
- **Issuer abuse.** The dev issuer hands out tokens to anyone who
  asks. Replace with a real issuer (your app server) before going
  past dev.

## Dev issuer

`signaling/dev-issuer.go` provides an in-process token issuer for
local development and integration tests. It activates when
`CBWRTC_DEV_ISSUER=1`:

1. Generates a fresh Ed25519 keypair on startup.
2. Sets `CBWRTC_AUTH_PUBKEY` from the public key (so `initAuth`
   verifies tokens this issuer signs).
3. Serves `/issue-token?role=...&session_id=...&tenant=...`,
   responding with `{token, exp, role, sid, sub}`.

The dev issuer **refuses to start** if `CBWRTC_AUTH_PUBKEY` is also
set explicitly — that combination is almost always a misconfigured
production deployment, and we want it to fail loudly.

A non-dev deployment must:

1. Generate the Ed25519 keypair off-cluster (e.g.,
   `openssl genpkey -algorithm ed25519`).
2. Inject the **public** half via `CBWRTC_AUTH_PUBKEY` into the
   signaling deployment.
3. Run a real issuer service (gated by app-level auth) signing
   short-lived tokens with the **private** half.
4. Never deploy `CBWRTC_DEV_ISSUER=1` to anything user-facing.

## Key rotation

Rotating the verifier key means signing a fleet of tokens with a new
private key and updating the `CBWRTC_AUTH_PUBKEY` env on the
signaling deployment. The naïve approach (single key in env)
forces a brief gap where the old key is invalid before the new one
rolls out — acceptable for short-`exp` tokens (clients re-auth and
get a new one), painful for long-`exp` tokens.

Phase-3+ improvements:

- **Multi-key verifier.** Accept either the previous or current
  public key; tokens carry a `kid` header to disambiguate.
- **JWKS endpoint.** Fetch keys from a sibling service so rotation
  is rolling.

These are deliberately deferred until we have a deployment that
needs them.

## Wire transport: query string vs. alternatives

We considered three places to put the token:

| transport             | works? | downsides                                |
|-----------------------|--------|------------------------------------------|
| `?token=...`          | yes    | shows in URL, in `Referer`, server logs  |
| `Sec-WebSocket-Protocol` | partial | inconsistent across browsers; can't co-exist with real subprotocols |
| First-frame envelope  | yes    | server must accept unauthenticated conn before deciding to drop; metrics lag |

Query-string wins on ubiquity. The privacy concern (URL leak) is
real but bounded: the URL is on the same TLS connection as the
upgrade itself, and our server logs only `remote` + `session_id`,
not the full URL.

## Files

- `signaling/auth.go` — verifier + claims struct + signing helper.
- `signaling/auth_test.go` — happy-path + every failure mode.
- `signaling/dev-issuer.go` — dev-only issuer endpoint.
- `signaling/server.go` — wsHandler reads `?token=`, calls
  `verifyToken` after the first envelope's `from` is known, closes
  with `1008` on failure.
- `signaling/metrics.go` — exposes
  `cb_signaling_auth_failures_total` (registered by `auth.go`).
- `client/src/auth.ts` — fetcher used by `client/main.ts` to get a
  token before opening the websocket.
- `tests/integration/signaling-roundtrip` — extended to cover the
  auth path end-to-end.

## TURN issuer (T76)

A second component verifies the same Ed25519 token: the
`infra/turn-issuer` daemon. When the client wants TURN credentials it
calls `POST /issue-turn-cred` with the same `Authorization: Bearer
<token>` it would attach to the signaling WS upgrade. The issuer:

1. Verifies the JWT against `CBWRTC_AUTH_PUBKEY` (same env, same
   contract — the verifier is a copy of `signaling/auth.go`'s
   `verifyToken`; tests `TestIssue_*` mirror the rejection cases).
2. Reads `sub` (tenant) and `sid` (session) from the token.
3. Mints a TURN-REST credential per RFC 7635:
   - `username = <expiry>:<tenant>:<sid>`
   - `credential = base64(hmac_sha1(SHARED_SECRET, username))`
4. Returns `{username, credential, ttl, urls, iceServers}` so the
   client can drop the response shape into `RTCPeerConnection`.

The shared secret (`CBWRTC_TURN_SHARED_SECRET`) is **separate** from
the auth pubkey — the issuer holds both:
- the **pubkey** (for verifying the caller's identity)
- the **shared secret** (for minting credentials coturn will accept)

Rotation of either is independent. See
`infra/turn-issuer/secret-rotation.md` for the shared-secret dance.

The token's `role` claim is **not** consulted by the issuer — both
`role=client` and `role=browser` are entitled to TURN. If we want to
restrict TURN to one side later, that's a one-line check after
`verifyToken`.

### Threat coverage

| Threat | Defense |
|---|---|
| Adversary mints arbitrary TURN credentials | Requires the SHARED_SECRET. Held only by the issuer + coturn; never sent to clients. Rotate periodically per `secret-rotation.md`. |
| Adversary replays a captured token to mint credentials for someone else's session | The token's `sid` claim is bound by signaling on the WS upgrade; for the TURN issuer the credential is scoped to whoever the token's `sub` (tenant) + `sid` say it is. The credential itself includes the expiry, so the leak window is bounded by the token's `exp` AND the credential's `ttl` (max 24h, default 1h). |
| Compromised issuer leaks the SHARED_SECRET | Rotate the secret (graceful, see `secret-rotation.md`). The pubkey doesn't need to rotate as a result. |
| TURN-REST username predictable enough to forge offline | `username = exp:tenant:sid` is predictable but the credential is HMAC-SHA1(secret, username); without the secret an attacker can't compute a valid credential. |

## References

- RFC 7519 (JWT)
- RFC 8037 (CFRG curves in JWS, including Ed25519 / `EdDSA`)
- RFC 6455 §10 (WebSocket security)
- RFC 7635 (TURN-REST short-term credentials)
- `infra/turn-issuer/main.go` — issuer implementation
- `infra/turn-issuer/main_test.go` — issuance + rotation grace tests
- `infra/turn-issuer/secret-rotation.md` — operational rotation
  procedure

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

## References

- RFC 7519 (JWT)
- RFC 8037 (CFRG curves in JWS, including Ed25519 / `EdDSA`)
- RFC 6455 §10 (WebSocket security)

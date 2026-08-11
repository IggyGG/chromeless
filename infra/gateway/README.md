# gateway

The single host-facing port for a **standalone** chromeless deployment: TLS,
a login, the client bundle, and a reverse proxy to the signaling broker.

```
https://localhost:8443/          the client bundle
              /login  /logout    operator credential → session cookie
              /ws/               → broker (WebSocket)
              /turn-credentials  → broker
              /probe             → broker (token-authed, no cookie)
              /healthz           liveness, unauthenticated
```

Kubernetes deployments do not use this — there, an Ingress terminates TLS and
the controller mints sessions. This exists for everyone else.

## Why

Before it, running chromeless outside Kubernetes meant publishing two ports
onto the host with nothing in front of them:

- **`8080`, the broker.** `CHROMELESS_AUTH_PUBKEY` was unset in every compose
  deployment, so `signaling/auth.go` logged *"auth disabled: any caller can
  connect"* at every boot and meant it.
- **`9222`, Chromium's DevTools.** With `--remote-allow-origins=*`. That is a
  full remote-code-execution surface: anything that can reach the port can
  drive the browser, read pages, and execute JavaScript.

Both are now internal-only.

## One port, deliberately

The page and the WebSocket share an origin. That is load-bearing, not tidiness:

- **One certificate decision.** Accepting the self-signed cert once also covers
  the `wss://` dial. Split across two ports, the browser prompts again on a URL
  the user never typed — and a WebSocket TLS failure surfaces as a bare
  `onerror` with no way to click through.
- **The client needs no configuration.** `client/src/config.ts` derives its
  endpoint from `location`, so it is right by construction whatever port or
  host the operator chose.
- **A `https://` page cannot dial `ws://`.** Mixed content is blocked outright,
  so the moment the page is served over TLS the broker must be too.

## Configuration

| variable | default | meaning |
| --- | --- | --- |
| `CHROMELESS_USER` | — | **required.** Login username. |
| `CHROMELESS_PASS` | — | **required.** Login password. Not trimmed. |
| `CHROMELESS_PORT` | `8443` | TLS listen port. |
| `CHROMELESS_TLS_CERT` / `_KEY` | — | BYO PEMs. Both or neither. |
| `CHROMELESS_TLS_DIR` | `/data/certs` | Where the self-signed pair is kept. |
| `CHROMELESS_TLS_HOSTS` | — | Extra SANs, comma-separated. |
| `CHROMELESS_SIGNALING_URL` | `http://signaling:8080` | Broker base. `ws://` accepted. |
| `CHROMELESS_CDP_URL` | `http://chromium:9222` | Worker DevTools (navigation). |
| `CHROMELESS_STATIC_DIR` | `/srv/client` | Client bundle. |
| `CHROMELESS_SESSION_TTL` | `12h` | Cookie lifetime. |
| `CHROMELESS_AUTH_PRIVKEY` | generated | Ed25519 signing key, base64 or hex. |
| `CHROMELESS_AUTH_PUBKEY` | — | Cross-check only; see below. |

There is no default credential. A gateway that boots with a built-in password
is worse than one that refuses to boot, because it looks protected.

## Certificates

With no cert supplied, a self-signed P-256 pair is generated into
`CHROMELESS_TLS_DIR` on first boot and **reused** afterwards — regenerating
would re-warn the browser on every restart and invalidate any cert the operator
had added to their trust store. SANs cover `localhost`, `127.0.0.1`, and `::1`;
add `CHROMELESS_TLS_HOSTS` to reach it by LAN IP or hostname.

Anything in `CHROMELESS_TLS_HOSTS` that parses as an IP goes into the
certificate's `IPAddresses`, not `DNSNames`. Browsers do not match a DNS SAN
against a literal IP, so the naive version produces a certificate that silently
fails to validate for the URL the operator actually typed.

## Auth model

One operator credential from the environment. Not a user system: no accounts,
no roles, no registration. That matches what the stack supports anyway — one
session per worker process — and a multi-user veneer over a single-tenant
deployment would be security theatre.

The cookie holds an opaque random id checked against a server-side map, not a
signed claim, so logout and restart both genuinely invalidate. With a signed
cookie, "log out" is only a client-side suggestion.

`/probe` is deliberately **not** cookie-gated: it carries its own JWT in the
query string (`client/src/probe.ts`) and the broker verifies it. Requiring a
cookie as well would break a legitimate non-browser caller holding a token.

## Session tokens

The cookie only proves you got past the login. The **broker** needs its own
proof, because it is reachable on the internal network and would otherwise
accept anyone — so the gateway also mints the Ed25519 JWT that
`signaling/auth.go` verifies. `/issue-token` is cookie-gated and returns
exactly the shape `client/src/auth.ts` already parses, so the client needs no
changes beyond sending its cookie.

Generate a matching pair and hand each half to the service that needs it:

```bash
eval "$(cd infra/gateway && go run ./cmd/keygen)"   # sets both env vars
docker compose -f infra/compose.yaml up
```

Both halves are generated **outside** compose because the broker needs the
public key at *its* startup, before the gateway exists — neither service can
hand the other anything at boot.

The gateway cross-checks `CHROMELESS_AUTH_PUBKEY` against its own signing key
and refuses to start if they disagree. That mismatch is the one
misconfiguration with no useful symptom: both services behave correctly and
simply disagree, so every connection dies as "signature mismatch" with nothing
in either log saying the keys differ.

Leave both unset and the broker keeps its old anonymous behaviour — useful when
driving it from a test harness, and loudly warned about at startup.

The token format lives in [`signaling/token`](../../signaling/token), shared
with the verifier rather than reimplemented. `turn-issuer` still carries its own
copy with a comment telling you to update it by hand; that is the failure this
package removes.

Tokens are short-lived (15 min) so that ceasing to issue is an effective
revocation without a denylist. `client/src/auth.ts` ships `TokenRefresher` for
exactly this.

## Tests

```bash
go test ./...          # or `make test-unit-go-modules` from the repo root
```

Covers cert generation/reuse/permissions, login and lockout, cookie
enforcement on every protected route, WebSocket upgrade headers surviving the
proxy, and the redirect-vs-401 split.

Removing a `requireSession` wrapper fails the suite — verified by mutation, not
assumed.

## Building

The image builds from the **repo root**, not this directory: it compiles the
client bundle from `client/` and shares the token package with `signaling/`.
`compose.yaml` sets `context: ..` for exactly this reason, and the root
`.dockerignore` keeps `capture/` out of the build context.

The bundle is built inside the image rather than copied from `client/dist`,
which is gitignored — on a fresh clone a `COPY` would fail, and on a stale one
it would silently ship whatever the operator last built by hand.

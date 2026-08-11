# Running chromeless standalone

For people with none of the triform infrastructure: one machine, one port, a
username and a password, a real browser in a tab.

```bash
eval "$(cd infra/gateway && go run ./cmd/keygen)"

CHROMELESS_IMAGE=my-registry/chromeless:cr7727-abc1234 \
CHROMELESS_USER=me CHROMELESS_PASS=hunter2 \
  docker compose -f infra/compose.yaml up
```

Open <https://localhost:8443>, accept the certificate once, sign in, and type a
URL.

Kubernetes deployments do not use any of this — there an Ingress terminates TLS
and the controller mints sessions. See
[`triform-deploy.md`](./triform-deploy.md) for a worked example of that.

---

## What is actually running

```
                        https://localhost:8443
                                  │
                     ┌────────────▼────────────┐
                     │  gateway                │   TLS terminates here
                     │  · login → cookie       │   the only published port
                     │  · /issue-token         │
                     │  · /api/navigate        │
                     │  · client bundle        │
                     │  · proxy /ws/           │
                     └───┬─────────────────┬───┘
                         │                 │
              signaling:8080        chromium:9222
              (broker)              (DevTools — never published)
```

Exactly one port is reachable from the host. That is the security model, not a
tidiness preference:

- **9222 is unauthenticated remote code execution.** Anything that reaches
  Chromium's DevTools can drive the session, read every page, and run arbitrary
  JavaScript. The image also starts Chromium with `--remote-allow-origins=*`,
  so a hostile web page could reach it. It used to be published by default.
- **8080 with no auth pubkey accepts anyone.** `signaling/auth.go` logs *"auth
  disabled: any caller can connect"* when `CHROMELESS_AUTH_PUBKEY` is unset,
  which was every compose deployment. Published, it is a route straight past
  the login.

If you need either for debugging, go through the container rather than
republishing:

```bash
docker compose -f infra/compose.yaml exec chromium \
  curl -s http://127.0.0.1:9222/json/version
```

## Why one port

The page and the WebSocket share an origin. Three things follow, and each is
the reason not to split them:

- **One certificate decision.** Accepting the self-signed cert once also covers
  the `wss://` dial. Split across ports, the browser prompts again for a URL
  the user never typed — and a WebSocket TLS failure surfaces as a bare
  `onerror` with nothing to click.
- **No client configuration.** `client/src/config.ts` derives its signaling
  endpoint from `location`, so it is right whatever host and port you chose.
- **A `https://` page cannot dial `ws://`.** Mixed content is blocked outright,
  so the moment the page is served over TLS the broker must be too.

## Configuration

Required — no defaults, deliberately:

| | |
| --- | --- |
| `CHROMELESS_IMAGE` | a `cloud_browser_worker` image; nothing here publishes one |
| `CHROMELESS_USER` / `CHROMELESS_PASS` | the login. A built-in default password is worse than no login: it looks like protection |

Common:

| | default | |
| --- | --- | --- |
| `CHROMELESS_PORT` | `8443` | host port |
| `CHROMELESS_TLS_CERT` / `_KEY` | — | your own certificate; both or neither |
| `CHROMELESS_TLS_HOSTS` | — | extra SANs for the generated one |
| `CHROMELESS_AUTH_PRIVKEY` / `_PUBKEY` | — | from `cmd/keygen`; see below |
| `CHROMELESS_TURN_SECRET` | — | required by the `turn` profile |
| `CHROMIUM_START_URL` | `about:blank` | page to open once the worker is up |
| `SESSION_ID` | `dev` | must match what the client connects with |

The full gateway list is in [`infra/gateway/README.md`](../../infra/gateway/README.md).

### Certificates

With none supplied, the gateway generates a self-signed pair into a named
volume on first boot and **reuses** it. Regenerating per boot would re-warn the
browser every restart and invalidate a cert you had added to a trust store.

SANs cover `localhost`, `127.0.0.1`, and `::1`. Reaching the gateway by LAN IP
or hostname needs `CHROMELESS_TLS_HOSTS=browser.lan,192.168.1.10` — a
certificate that does not name the host you typed is rejected outright.

### The keypair

The gateway mints session tokens; the broker verifies them. Both halves come
from one place because the broker needs the public key at *its* startup, before
the gateway exists — neither can hand the other anything at boot:

```bash
eval "$(cd infra/gateway && go run ./cmd/keygen)"
```

Omit them and the broker keeps its old anonymous behaviour, warned about at
startup. The gateway cross-checks the two and refuses to start if they
disagree — a mismatched pair is the one misconfiguration with no useful
symptom: both services behave correctly, disagree, and every connection dies as
"signature mismatch" with nothing in either log saying the keys differ.

## TURN

Off by default. A same-host run does not need a relay: both peers are on one
machine and host candidates connect directly.

```bash
CHROMELESS_TURN_SECRET=$(openssl rand -hex 32) \
  docker compose -f infra/compose.yaml --profile turn up
```

You need it when the peers are on different networks, or when a viewer sits
behind a symmetric NAT or a corporate firewall. Then also point the worker at
it, using the same secret:

```bash
CHROMELESS_ICE_SERVERS='[{"urls":["turn:192.168.1.10:3478"],
                          "username":"chromeless",
                          "credential":"'"$CHROMELESS_TURN_SECRET"'"}]'
```

Use a LAN address, not `localhost` — the relay has to be reachable from the
viewer's machine too.

Three things worth knowing before you debug this:

- **The profile is Linux-only.** It uses `network_mode: host`, because a relay
  allocates from a 16k UDP port range and the allocated port must be reachable.
  Docker's per-port NAT cannot express that, and `network_mode: host` is a
  no-op on Docker Desktop for macOS and Windows.
- **The credentials are static, and that is deliberate.** The Helm chart and
  the k8s manifest both use `use-auth-secret` (HMAC-REST), but
  `signaling/turn.go` only emits static credentials, and static credentials
  cannot authenticate against a `use-auth-secret` relay. Pairing them gives you
  a relay that refuses every allocation while looking healthy — no error, just
  no video. For multi-user deployments run
  [`infra/turn-issuer/`](../../infra/turn-issuer/) (HMAC-REST, rotation-aware)
  against a `use-auth-secret` relay, which is what triform does.
- **Both peers must agree.** The worker's `CHROMELESS_ICE_SERVERS` and the
  broker's `TURN_*` have to name the same relay and credential. A
  half-configured pair fails as "no candidate pair", which reads as a network
  fault rather than a config one.

## The browser on another machine

Use `infra/compose.host.yaml` where you sit and `infra/compose.worker.yaml`
where the cores are. This needs no new mechanism — the worker already learns
everything from the environment and dials out, so it is a matter of naming a
remote broker.

```bash
# on the machine you sit at
eval "$(cd infra/gateway && go run ./cmd/keygen)"
CHROMELESS_USER=me CHROMELESS_PASS=hunter2 \
CHROMELESS_TURN_SECRET=$(openssl rand -hex 32) \
  docker compose -f infra/compose.host.yaml --profile turn up

# on the machine with the browser
CHROMELESS_IMAGE=… \
CHROMELESS_SIGNALING_HOST=gateway.example.com:8443 \
CHROMELESS_SIGNALING_TOKEN=<browser-role token> \
CHROMELESS_ICE_SERVERS='[…]' \
  docker compose -f infra/compose.worker.yaml up
```

Four differences from the single-host case, all of which only appear once the
halves are apart:

1. **TURN stops being optional.** Without a relay the session negotiates and
   then produces no video, which reads as a broken build.
2. **The worker needs a token**, and cannot refresh it. Mint a browser-role one
   from the gateway's `/issue-token`. They are short-lived by design;
   `capture/signaling/cb_signaling_reconnect.h` is explicit that token refresh
   is out of scope, so a worker whose token expires exits and must be
   restarted.
3. **The worker cannot click through a self-signed certificate.** It is
   Chromium and it validates. Mount a real certificate into the gateway, or add
   the generated one to the worker machine's trust store.
4. **Navigation needs a route to the worker's DevTools.** Set
   `CHROMELESS_CDP_URL` on the gateway to reach port 9222 on the worker — over
   a private network or an SSH tunnel, never the public internet.

## What this is not

- **Not multi-user.** One credential, from the environment. No accounts, no
  roles, no registration. That matches what the stack supports anyway.
- **Not multi-session.** One session per worker process:
  `StartNativeSession` rejects a second bring-up
  (`cloud_browser_browser_main_parts.cc`). After a `bye`, restart the container
  for a new session.
- **Not multi-tab.** The client drives one page. The portal has a tab strip;
  this does not.
- **Not a hardened public deployment.** Static TURN credentials, an in-memory
  session store, and a self-signed certificate are all fine for one operator
  behind a login and none of them are what you would ship to the open internet.

## Troubleshooting

**The page loads but Connect never connects.** Check the broker actually has
the public key — `docker compose logs signaling | grep auth`. "auth enabled
(Ed25519)" is what you want; "auth disabled" means the key never arrived.

**Video never arrives; the log says "waiting for offer".** The worker is not
producing an offer. Confirm it reached signaling with the *same session id* the
client is using (`SESSION_ID` on the worker, the session field in the page).
The client now gives up and says so rather than spinning forever.

**"no offer from browser" after ~65s.** The client asked for a renegotiation
and still got nothing. Almost always a session-id mismatch or a worker that
never dialled — check `docker compose logs chromium | grep -i signaling`.

**Everything connects, no video, no errors.** Usually ICE. Without a relay,
peers on different networks negotiate and then never pair. Bring up the `turn`
profile and set both sides.

**The address bar does nothing.** The gateway needs `CHROMELESS_CDP_URL` to
reach the worker's DevTools. In the single-host stack that is automatic; in a
split deployment you have to provide the route.

**`file://` is refused.** Deliberately. The gateway allows `http` and `https`
only — `file:///data/certs/key.pem` would otherwise render its own TLS key into
the video stream.

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

To run this same stack on a cluster — worth it when your machine cannot run the
worker image, which is amd64-only — see
[`infra/k8s/standalone/`](../../infra/k8s/standalone/). That is where the whole
thing was first proven end to end, and its README lists what cost time.

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
| `SIGNALING_TOKEN` | — | the worker's own credential, also from `cmd/keygen`. Required once the keys are set |
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

### The keypair, and the worker's token

The gateway mints session tokens; the broker verifies them. Both halves come
from one place because the broker needs the public key at *its* startup, before
the gateway exists — neither can hand the other anything at boot:

```bash
eval "$(cd infra/gateway && go run ./cmd/keygen)"
```

That sets **three** variables, not two:

| | |
| --- | --- |
| `CHROMELESS_AUTH_PRIVKEY` | the gateway signs with it |
| `CHROMELESS_AUTH_PUBKEY` | the broker verifies with it |
| `SIGNALING_TOKEN` | what the *worker* presents to the broker |

The third exists because enabling auth enables it for **both** peers. The page
asks the gateway for its token after logging in; the browser has no login and
nothing to ask, so it has to be handed one. `keygen` mints a 30-day
`browser`-role token for `${SESSION_ID:-dev}` — set `SESSION_ID` before the
`eval` if you use a different one.

Until 2026-08-19 it minted only the keypair, and this quickstart followed
exactly armed the broker against a worker carrying nothing. See
[Troubleshooting](#troubleshooting) for what that looks like, which is nothing
at all.

Omit all three and the broker keeps its old anonymous behaviour, warned about
at startup. The gateway cross-checks the two keys and refuses to start if they
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

# still here — mint the far worker's token, because this is where the private
# key is. `eval`'s own SIGNALING_TOKEN would also work; mint explicitly if the
# worker uses a different session id.
SESSION_ID=dev go run ./infra/gateway/cmd/worker-token

# on the machine with the browser, with that token pasted in
CHROMELESS_IMAGE=… \
CHROMELESS_SIGNALING_HOST=gateway.example.com:8443 \
CHROMELESS_SIGNALING_TOKEN=eyJhbGciOiJFZERTQSI… \
CHROMELESS_ICE_SERVERS='[…]' \
  docker compose -f infra/compose.worker.yaml up
```

The token is minted on the gateway's machine and carried over, not generated
where the browser runs — `CHROMELESS_AUTH_PRIVKEY` must never leave the host
that signs with it.

Four differences from the single-host case, all of which only appear once the
halves are apart:

1. **TURN stops being optional.** Without a relay the session negotiates and
   then produces no video, which reads as a broken build.
2. **The worker needs a token**, and cannot refresh it. Mint one where the
   private key is, with the session id the worker will use:

   ```bash
   SESSION_ID=dev go run ./infra/gateway/cmd/worker-token
   ```

   Not from the gateway's `/issue-token` — that mints 15-minute tokens, which
   suit a page that rolls them over silently and not a browser process that
   reads its token once at launch. `capture/signaling/cb_signaling_reconnect.h`
   is explicit that token refresh is out of scope, so an expired token means
   the process exits; `cmd/worker-token` mints 30 days to make that rare.
3. **The worker cannot click through a self-signed certificate.** It is
   Chromium and it validates. Mount a real certificate into the gateway, or add
   the generated one to the worker machine's trust store.
4. **Navigation needs a route to the worker's DevTools.** Set
   `CHROMELESS_CDP_URL` on the gateway to reach port 9222 on the worker — over
   a private network or an SSH tunnel, never the public internet.

## What this is not

- **Not multi-user.** One credential, from the environment. No accounts, no
  roles, no registration. That matches what the stack supports anyway.
- **Not multi-viewer.** One viewer at a time. When a viewer leaves, the worker
  re-arms its peer connection in place and the next viewer gets the browser as
  it was left (`RearmSession` in `cloud_browser_browser_main_parts.cc`; the
  chromium pid does not change — `tests/local/README.md`). Two people cannot
  watch the same session at once. If a re-arm fails the process exits and
  supervisord starts a fresh one, which loses open tabs and in-memory state.
- **Not multi-tab.** The client drives one page. The portal has a tab strip;
  this does not.
- **Not a hardened public deployment.** Static TURN credentials, an in-memory
  session store, and a self-signed certificate are all fine for one operator
  behind a login and none of them are what you would ship to the open internet.

## Troubleshooting

**The page loads but Connect never connects.** Two causes, and they look
identical from the browser.

First check the broker's auth line — `docker compose logs signaling | grep
auth`. "auth enabled (Ed25519)" means the public key arrived; "auth disabled"
means it did not, and the login is then protecting only the HTML.

Then check the *worker* was given a credential for that same auth. This is the
one that reads as a network fault:

```bash
docker compose -f infra/compose.yaml logs chromium | grep -i "1008\|missing token"
```

`1008 missing token` means the broker refused the worker. Nothing else in the
stack shows it — the handshake failure exits the browser process, supervisord
respawns it every ~30 s, and DevTools answers `/json/version` the whole time.
The container is healthy, the page is fine, and no offer is ever produced.

The fix is to use the token `keygen` prints:

```bash
eval "$(cd infra/gateway && go run ./cmd/keygen)"   # exports SIGNALING_TOKEN too
```

Before 2026-08-19 `keygen` emitted only the keypair, so it armed the broker
against a worker it had given nothing — the quickstart, followed exactly,
produced this. If you have an older shell environment still loaded, re-run the
`eval` and bring the stack back up. Verify with `echo "${SIGNALING_TOKEN:0:12}"`;
empty means the worker will be refused.

**Video never arrives; the log says "waiting for offer".** The worker is not
producing an offer. Confirm it reached signaling with the *same session id* the
client is using (`SESSION_ID` on the worker, the session field in the page).
The client now gives up and says so rather than spinning forever.

**"no offer from browser" after ~65s.** The client asked for a renegotiation
and still got nothing. Almost always a session-id mismatch or a worker that
never dialled — check `docker compose logs chromium | grep -i signaling`.

**Everything connects, no video, no errors.** Usually ICE — and this one is
confirmed, not theoretical. On a real deployment with the worker in a cluster
and the viewer on a laptop, both behind NAT: offer and answer exchanged, VP9
negotiated, both tracks received, all five data channels open, capture
`VERDICT=PRODUCING` on the worker — and `framesDecoded` stuck at 0 forever. The
worker's ICE goes `checking -> failed` with only host and srflx candidates and
no relay. Adding TURN produced 6 relay candidates and a paired connection
immediately.

Check the worker's log for candidate types:

```bash
grep -oE "typ (host|srflx|relay)" <chromium.err.log> | sort | uniq -c
```

No `relay` line means no relay. Both peers need one — the worker via
`CHROMELESS_ICE_SERVERS`, the browser via the broker's `TURN_URLS` /
`TURN_USER` / `TURN_PASS`. Setting only one side fails the same way.

**Video worked once, then a second client gets nothing.** On a current image
this should not happen: after the first client sends `bye` the worker logs
`CV2-REARM: rebuilt; awaiting OnRenegotiationNeeded` and offers again to the
next viewer. If you see `session closed, reason=remote bye` with nothing after
it, the guest predates the re-armable driver (images before
`cr7727-8d2ce2e66288`, 2026-08-21) — roll the image, or restart the container
as a stopgap. If you see `CV2-REARM: re-arm failed`, the process exits and
supervisord respawns it within a few seconds; the next connect works but the
browser state is gone. Either way the tell is in the worker's own log, which is
not in `docker logs` — use `docker compose exec chromium cat
/var/log/supervisor/chromium.err.log`.

**The address bar does nothing.** The gateway needs `CHROMELESS_CDP_URL` to
reach the worker's DevTools. In the single-host stack that is automatic; in a
split deployment you have to provide the route.

**`file://` is refused.** Deliberately. The gateway allows `http` and `https`
only — `file:///data/certs/key.pem` would otherwise render its own TLS key into
the video stream.

# input-bridge

> **Status:** Phase 1 skeleton (T22).
> **Owner:** `platform-dev`.
> **Companion:** `client/src/input.ts` (T20) and the protocol spec in
> [`docs/protocols/input-channel.md`](../../docs/protocols/input-channel.md).

Server-side counterpart to the client's `InputChannel` encoder. It
consumes v1 input envelopes (one per stdin line, or one per WebSocket
message on a localhost endpoint), translates them into Chromium
DevTools Protocol calls, and dispatches them against a headless
Chromium running locally with `--remote-debugging-port=9222`.

## How it fits in

```
                                +-----------------------------+
   browser client (T14) --WS--->|        signaling (T13)      |
                                +-----------------------------+
                                            |
                                       (data channel)
                                            v
                            +-----------------------------+
                            |  input-bridge  (this dir)   |
                            |  --source ws | stdin        |
                            +--------------+--------------+
                                           | CDP / ws://localhost:9222
                                           v
                            +-----------------------------+
                            |  headless Chromium          |
                            |  Input.dispatchMouseEvent / |
                            |  Input.dispatchKeyEvent     |
                            +-----------------------------+
```

The relay between the WebRTC data channel and the bridge is not in
this directory — for now you point a relay at `--source ws` and have
it forward each `RTCDataChannel` message verbatim. A first
implementation can live in the streamer-page sidecar; a more durable
implementation will land alongside the signaling server.

## Build and run

```bash
# build
go build -o input-bridge .

# stdin source — most useful for ad-hoc smoke tests
echo '{"v":1,"type":"mouse_move","t":1,"seq":0,"data":{"x":10,"y":20}}' \
  | ./input-bridge --source stdin --cdp-url http://127.0.0.1:9222

# ws source — the production-ish path
./input-bridge --source ws --ws-addr 127.0.0.1:9100 \
               --cdp-url http://127.0.0.1:9222 \
               --metrics-addr 127.0.0.1:9101
# then connect to ws://127.0.0.1:9100/input and send envelopes
```

`--dry-run` skips CDP entirely (useful to confirm parsing + metrics
without a Chromium running).

## Configuration flags

| flag             | default                  | meaning                                                                 |
| ---------------- | ------------------------ | ----------------------------------------------------------------------- |
| `--source`       | `stdin`                  | `stdin` (newline-delimited JSON) or `ws` (websocket).                   |
| `--ws-addr`      | `127.0.0.1:9100`         | Listen address for the websocket source.                                |
| `--ws-path`      | `/input`                 | URL path for the websocket source.                                      |
| `--cdp-url`      | `http://127.0.0.1:9222`  | Base URL for Chromium's DevTools HTTP+WS endpoints.                     |
| `--metrics-addr` | `127.0.0.1:9101`         | Listen address for `/metrics` and `/healthz`.                           |
| `--dry-run`      | `false`                  | Parse + record metrics; do not connect to or send to CDP.               |

## Protocol coverage

Implements every event type in v1 of `docs/protocols/input-channel.md`:

| Envelope `type`               | CDP call                       |
| ----------------------------- | ------------------------------ |
| `mouse_move`                  | `Input.dispatchMouseEvent` (`mouseMoved`) |
| `mouse_button` (down/up)      | `Input.dispatchMouseEvent` (`mousePressed`/`mouseReleased`) |
| `mouse_wheel`                 | `Input.dispatchMouseEvent` (`mouseWheel`) |
| `key_down` / `key_up`         | `Input.dispatchKeyEvent`       |
| `composition_start` / `_update`| `Input.imeSetComposition`     |
| `composition_end`             | `Input.insertText`             |
| `clipboard_paste`             | `Input.insertText`             |
| `clipboard_copy_request`      | synthesized Ctrl+C key down/up |
| _unknown_                     | logged + counted, discarded    |

Modifier and button bits are remapped from the protocol's bitmask to
CDP's; see the `protocolModsToCDP` / `protocolButtonToCDP` helpers in
`main.go`.

`Page.bringToFront` is called once at startup so window-mode Chromium
delivers keystrokes to the front-most page.

## Metrics

`/metrics` is the standard Prometheus exposition format. Useful series:

| metric                                  | type      | labels    |
| --------------------------------------- | --------- | --------- |
| `input_bridge_events_total`             | counter   | `type`    |
| `input_bridge_errors_total`             | counter   | `type`    |
| `input_bridge_unknown_total`            | counter   | —         |
| `input_bridge_parse_errors_total`       | counter   | —         |
| `input_bridge_dispatch_seconds`         | histogram | `type`    |

Default histogram buckets cover 0.5 ms → 1 s; the v1 input round-trip
budget is **80 ms median** end-to-end (see
[`docs/v1-success-criteria.md`](../../docs/v1-success-criteria.md), L5),
so the bridge itself should sit comfortably below 25 ms p95.

`/healthz` returns `{"status":"ok"}` for compose / K8s probes.

## Tests

```bash
go test ./...
```

Two layers:

- **Pure-function tests** (`parseEnvelope`, `protocolModsToCDP`,
  `protocolButtonToCDP`) — table-driven, no network.
- **End-to-end dispatcher tests** against a *fake CDP target* — a
  test-server WebSocket that records the methods+params each
  `Dispatch` call produces and replies with synthetic `{"result":{}}`
  so the request/response pairing is exercised.

The fake target is deliberately strict about the discovery handshake
(`/json/list` then ws upgrade on `/devtools/page/1`) so refactors of
`dialCDP` don't silently regress against real Chromium.

## Docker

```bash
docker build -t input-bridge .
docker run --rm -p 9100:9100 -p 9101:9101 \
           --network=host \
           input-bridge
```

The image is a multi-stage build that finishes in `distroless/static-
debian12:nonroot`. CGO is disabled; the binary is fully static. Tests
run at build time so a broken commit cannot produce an image.

## Compose tie-in

In `infra/compose.yaml` (T8) the bridge becomes a service that runs
either:

1. **As a sidecar in the chromium container** — talks to CDP via
   `http://127.0.0.1:9222` on the loopback interface. This is the
   target Phase 1 layout per the project brief. Audio + capture
   already share the container; input is the same shape.
2. **As a separate service** during local development, with the
   chromium service exposing `9222` on the compose network and the
   bridge pointed at it via `--cdp-url http://chromium:9222`.

Either way, the bridge listens on `127.0.0.1:9100/input` (sidecar) or
`0.0.0.0:9100/input` (separate service) and a relay forwards each
`RTCDataChannel("input")` message into it.

## Known gaps for the skeleton

- **No auth on the websocket source.** The signaling layer is
  responsible for authorising the peer; v1 leaves the bridge
  permissive on the loopback. Don't expose `--ws-addr` beyond the
  container's localhost without addressing this.
- **No rate limiting.** Per the protocol's security note, T22's
  bridge MUST add a per-channel input-rate ceiling before this is
  exposed in any non-dev environment. Tracked separately.
- **No reconnection to CDP.** If Chromium restarts, the bridge exits
  on the next dispatch; the orchestrator restarts it (compose
  `restart: unless-stopped`). Phase 2 will add transparent
  reconnect with a backoff.
- **Wheel deltas approximate non-pixel modes** (line/page) with
  fixed multipliers (16 px/line, 800 px/page). Phase 2 replaces this
  with proper page-aware scrolling.
- **Composition events use IME APIs but no language-server context.**
  Workable for Latin/CJK input as a smoke test; full IME coverage
  needs Phase 2 work.

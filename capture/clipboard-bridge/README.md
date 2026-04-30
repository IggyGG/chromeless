# clipboard-bridge

> **Status:** Phase 1 (T31).
> **Owner:** `platform-dev`.
> **Companion:** [`docs/protocols/clipboard-channel.md`](../../docs/protocols/clipboard-channel.md)
> and the client at [`client/src/clipboard.ts`](../../client/src/clipboard.ts).

Bidirectional clipboard sync sidecar for the cloud Chromium. Speaks
the v1 `clipboard_offer` envelope on both directions:

- **Inbound (client→cloud):** receives envelopes on stdin or a
  localhost WebSocket; for each, writes the text into the remote
  Chromium's clipboard and synthesizes Ctrl+V into the focused
  element so the user observes a real paste event.
- **Outbound (cloud→client):** injects a small JS probe that listens
  for `copy` events in every page and reports the copied text via a
  `Runtime.addBinding` callback. The bridge translates each callback
  into a `cloud->client` envelope on its sink.

The protocol's narrow-path stance is enforced here: text only,
`source: "user_action"` only, 1 MiB cap, no silent polling.

## Why two CDP strategies for inbound?

There are two ways the bridge can deliver text into the cloud
Chromium for an inbound `client->cloud` envelope:

1. **`Runtime.evaluate('navigator.clipboard.writeText(...)')`** —
   actually updates the OS clipboard inside the container. Subsequent
   Ctrl+V (which we synthesize via `Input.dispatchKeyEvent`) fires a
   real `paste` event in the focused element, and any other JS that
   reads the clipboard sees the new value.
2. **`Input.insertText`** — types the text directly into whatever
   element has focus. Doesn't update the clipboard, but works when
   #1 fails (no focus, permission denied, edge cases).

The bridge **tries #1 first** with `awaitPromise: true` and
`returnByValue: true`. If it gets `"ok"` back from the wrapped
`then(()=>"ok").catch(e=>"err:"+...)`, it then dispatches Ctrl+V to
finish the paste. If anything goes wrong, it falls back to
`Input.insertText`.

`Browser.grantPermissions` is called once at install time with
`["clipboardReadWrite", "clipboardSanitizedWrite"]` so the
`navigator.clipboard.writeText` call doesn't trigger a permission
prompt. Some Chromium builds reject the second name; we retry with
just `clipboardReadWrite` and continue.

## Echo suppression

When inbound text is written to the clipboard, the bridge also fires
an `oncopy` (via the page's own selection change, in some cases) —
which would loop straight back as an outbound envelope. The bridge
remembers the last text it wrote inbound and drops outbound events
whose text matches exactly. This is sufficient for v1's user-action
gating; a more robust scheme is Phase 2 work.

## Build and run

```bash
go build -o clipboard-bridge .

# Smoke test (no CDP). Inbound text logged but not applied.
echo '{"v":1,"type":"clipboard_offer","t":1,"seq":0,"data":{"direction":"client->cloud","source":"user_action","text":"hello"}}' \
  | ./clipboard-bridge --dry-run --source stdin

# Real run, inbound on stdin, outbound on stdout, against a Chromium
# with --remote-debugging-port=9222.
./clipboard-bridge --source stdin --sink stdout

# Full sidecar mode: inbound on a localhost WS from the relay,
# outbound to a downstream WS that pushes into the data channel.
./clipboard-bridge --source ws --ws-addr 127.0.0.1:9300 --ws-path /clipboard \
                   --sink   ws --sink-url ws://127.0.0.1:9301/clipboard
```

## Flags

| flag           | default                          | meaning                                              |
| -------------- | -------------------------------- | ---------------------------------------------------- |
| `--source`     | `stdin`                          | Inbound source: `stdin` or `ws`.                     |
| `--ws-addr`    | `127.0.0.1:9300`                 | Inbound WS listen addr.                              |
| `--ws-path`    | `/clipboard`                     | Inbound WS path.                                     |
| `--cdp-url`    | `http://127.0.0.1:9222`          | Chromium DevTools base URL.                          |
| `--sink`       | `stdout`                         | Outbound sink: `stdout` or `ws`.                     |
| `--sink-url`   | `ws://127.0.0.1:9301/clipboard`  | Outbound WS URL when `--sink=ws`.                    |
| `--dry-run`    | `false`                          | Skip CDP entirely; log inbound text and skip outbound.|

## Tests

```bash
go test ./...
```

Three test layers:

- Pure-function tests for `parseEnvelope` (valid/invalid versions,
  types, sources, directions, oversize) and `quoteJSString`
  round-trip across tricky cases (quotes, newlines, tabs, emoji).
- Inbound CDP-call sequencing using a fake `cdpSender`:
  - Happy-path: writeText returns `"ok"` ⇒ bridge issues
    `Runtime.evaluate` then keyDown + keyUp (3 calls total).
  - Fallback path: writeText returns an error string ⇒ bridge
    issues `Input.insertText` and skips the keyEvents.
- Outbound: simulated `Runtime.bindingCalled` events translate to
  monotonically-sequenced `cloud->client` envelopes, with echo
  suppression verified against a primed `last` value.

## Integration test

A separate end-to-end test in
[`tests/integration/clipboard_test.go`](../../tests/integration/clipboard_test.go)
builds the binary, drives it against a fake CDP server, and asserts
both directions of the protocol behave correctly under realistic
process boundaries.

## Docker

```bash
docker build -t clipboard-bridge .
docker run --rm --network=host clipboard-bridge
```

Multi-stage; finishes in `gcr.io/distroless/static-debian12:nonroot`.
Runs `go test ./...` at build time.

## Compose tie-in

The bridge is a sidecar in the chromium container so it can dial
`http://127.0.0.1:9222` over the loopback. It pairs with the same
relay that wires `RTCDataChannel("input")` into input-bridge — the
relay also owns the `RTCDataChannel("clipboard")` and forwards it
to/from the bridge's WS endpoints. v1 leaves the relay piece TBD;
T41-style work covers it.

## Known gaps for v1

- **No reconnect to CDP.** Crash recovery is whole-container.
- **Single page-target.** First `page` target reported by
  `/json/list` is what we attach to; multi-tab support needs
  `Target.attachToTarget` session multiplexing.
- **Selection-driven `copy` only.** The probe forwards
  `e.clipboardData.getData("text/plain")` and falls back to
  `window.getSelection().toString()`. Programmatic
  `navigator.clipboard.writeText` calls inside the page won't fire
  `copy`; for those, Phase 2 should add a polling read of the
  clipboard at a coarse rate, gated by an explicit user gesture.
- **Image/HTML payloads dropped.** Per the v1 protocol; tracked for
  Phase 4.
- **No per-tenant origin scoping** of `Browser.grantPermissions` —
  granted browser-wide. Acceptable inside a single-tenant container;
  the orchestrator owns isolation.

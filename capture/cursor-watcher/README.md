# cursor-watcher

> **Status:** Phase 1 skeleton (T26).
> **Owner:** `platform-dev`.
> **Companion docs:** [`docs/protocols/cursor-channel.md`](../../docs/protocols/cursor-channel.md)
> and the client renderer at [`client/src/cursor.ts`](../../client/src/cursor.ts).

Server-side service that watches the cloud Chromium for cursor shape
and position changes, and emits one v1 envelope per change so the
client can render the cursor locally over the streamed video. Per the
project brief, server-side cursor in the framebuffer adds a frame of
latency — we render client-side from metadata.

## How

CDP doesn't expose a "cursor changed" event, so the watcher injects a
small JavaScript probe into every page via
`Page.addScriptToEvaluateOnNewDocument` and observes it through
`Runtime.addBinding`:

1. The probe tracks pointer position via `pointermove` (capture +
   passive listeners on `document`).
2. On every `requestAnimationFrame` it calls
   `getComputedStyle(elementFromPoint(x, y)).cursor` and parses the
   result into either a CSS keyword (`pointer`, `text`, …) or a
   `url(...)` custom-image reference.
3. When anything observable changes — position **or** shape **or**
   visibility — it stringifies the payload and calls
   `window.__cb_cursor__(json)`, which Chromium reports as a
   `Runtime.bindingCalled` event.
4. The Go service receives the event, dedup-checks against the last
   emitted state (we only emit on real change), and writes a v1
   envelope to its sink.

For the Phase-1 skeleton, custom-cursor URLs come through as
`shape: "custom"` with the URL logged but not yet fetched into
`custom_image_b64`. Wiring image fetch + base64 + 64 KiB cap is a
follow-up; the protocol allows the field today.

## Build and run

```bash
go build -o cursor-watcher .

# Smoke test: emit one synthetic envelope and exit. Useful for
# pipelines that just want to confirm the binary runs.
./cursor-watcher --dry-run
# {"v":1,"type":"cursor","t":...,"seq":0,"data":{"x":0,"y":0,"visible":true,"shape":"default"}}

# Real run against a Chromium with --remote-debugging-port=9222.
./cursor-watcher --cdp-url http://127.0.0.1:9222 \
                 --sink stdout

# Real run with a downstream WebSocket relay (the eventual
# RTCDataChannel("cursor") path).
./cursor-watcher --sink ws --sink-url ws://127.0.0.1:9200/cursor
```

## Flags

| flag         | default                       | meaning                                              |
| ------------ | ----------------------------- | ---------------------------------------------------- |
| `--cdp-url`  | `http://127.0.0.1:9222`       | Chromium DevTools base URL.                          |
| `--sink`     | `stdout`                      | `stdout` (newline-delimited JSON) or `ws`.           |
| `--sink-url` | `ws://127.0.0.1:9200/cursor`  | Downstream WebSocket URL when `--sink=ws`.           |
| `--dry-run`  | `false`                       | Skip CDP entirely; emit one synthetic envelope and exit. |

## Wire format

See [`docs/protocols/cursor-channel.md`](../../docs/protocols/cursor-channel.md)
for the full v1 spec. Each envelope is one line of UTF-8 JSON:

```jsonc
{ "v":1, "type":"cursor", "t":1730290000123, "seq":17,
  "data":{ "x":612, "y":401, "visible":true, "shape":"pointer" } }
```

The watcher emits **only on change** (deduped by
`(x, y, shape, visible, custom_image_b64)`). On (re)connect of the
downstream sink, the watcher emits the current full state as the
baseline before any further deltas.

## Tests

```bash
go test ./...
```

Three layers:

- Pure-function tests for `equalState` dedup, envelope shape,
  flag parsing, and the `--dry-run` path.
- An end-to-end test that spins up a `httptest.Server` speaking just
  enough CDP (`/json/list` discovery + `/devtools/page/1` ws upgrade)
  to accept the install RPCs, then synthesizes
  `Runtime.bindingCalled` events and asserts the watcher emits the
  right envelopes (and dedups identical ones).
- The dry-run smoke test asserts the binary emits exactly one
  baseline envelope.

## Docker

```bash
docker build -t cursor-watcher .
docker run --rm --network=host cursor-watcher
```

The image is a multi-stage build that finishes in
`gcr.io/distroless/static-debian12:nonroot`. CGO is disabled; the
binary is fully static. Tests run at build time.

## Compose tie-in

The watcher is intended to run as a **sidecar** in the same container
group as Chromium so it can talk to CDP via `127.0.0.1:9222`. Two
envelope-delivery shapes are supported:

1. **Sidecar piping** — the watcher writes envelopes to stdout, and
   a process inside the streamer page picks them up and pushes them
   into the WebRTC `cursor` data channel. Easiest for v0; needs a
   tiny relay alongside the streamer.
2. **WebSocket sink** — the watcher dials a ws endpoint exposed by
   the signaling server (or a bespoke relay). Cleaner separation;
   matches the shape we use for the input bridge in T22.

Either way, no auth is required at the sidecar level — both peers are
inside the per-tenant container. Per-tenant isolation is the
container boundary itself.

## Known gaps for the skeleton

- **Custom cursor images are not fetched.** `cursor: url(...)` shows
  up as `shape: "custom"` with no `custom_image_b64` payload. The
  client falls back to the default arrow when this happens.
- **No reconnect to CDP.** Crashes propagate; the orchestrator
  restarts the container.
- **No coalescing of x/y-only updates** — the probe already throttles
  to one rAF tick per change, but the protocol's "≤ 60 Hz x/y
  coalesce" SHOULD policy isn't enforced separately. Phase 2 work.
- **One target only.** The watcher attaches to the first `page`
  target reported by `/json/list`. Per the v1 architecture this is
  the streamer page, but a multi-tab future will need
  `Target.attachToTarget` session multiplexing.
- **No baseline emit on sink reconnect.** v1's spec says "on
  (re)connect emit one full envelope" — this isn't implemented for
  the wsEmitter yet because the dial happens lazily on the first
  emit. Phase 2 will add an explicit reconnect hook that flushes
  the current state.

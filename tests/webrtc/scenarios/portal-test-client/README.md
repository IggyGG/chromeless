# `portal-test-client/` — Triform-portal input-encoder test fixture

A static HTML page that loads the Triform portal's **input encoder
crate** (`triform-input-encoder`) into a wasm-bindgen glue layer
(`triform-input-encoder-test-client`), attaches DOM event listeners
to a `<canvas>`, and forwards each encoded JSON message over a
configurable WebSocket.

Used by the chromeless scenarios numbered **30+** to verify that the
portal's WS-input wire format is byte-correct end-to-end:

```
CDP Input.dispatchMouseEvent (test driver)
  → DOM mousedown on the canvas
  → encoder.web::from_mouse_event   ◀── PRODUCTION CODE PATH
  → JSON { "type": "mouse", "action": "down", "x": …, "y": …, "button": …, "modifiers": … }
  → WebSocket → stub WS server in the scenario runner
  → assertions on what arrived
```

## Why this exists

The portal's `browser_screencast.rs` is the production caller of
`triform-input-encoder::web::from_*`. It runs in a Leptos / WASM
runtime with browser pods, CDP relays, screencast frames — none of
which a fast scenario suite wants to spin up.

This fixture isolates **just the encoder** by reusing the exact same
crate and the same web-sys helpers, swapping the production WebSocket
target (physics's `/api/{circle}/{element}/ws/screencast`) for a stub
WS server the scenario runner provides. That keeps three things
honest:

1. Drift between portal and tests is a compile error in this crate
   (because we depend on the same `triform-input-encoder` crate
   portal does).
2. Wire-format regressions surface as scenario failures, not as
   silent serde-default falls (the regression class that motivated
   the encoder lift).
3. CDP modifier flags, key text encoding, scroll camelCase keys —
   each pinned by both unit tests and scenario assertions.

## Layout

```
portal-test-client/
├── README.md     (this file)
├── index.html    static HTML; loads pkg/ via ES modules
└── pkg/          wasm-pack output (committed)
    ├── triform_input_encoder_test_client.js
    ├── triform_input_encoder_test_client.d.ts
    ├── triform_input_encoder_test_client_bg.wasm
    └── triform_input_encoder_test_client_bg.wasm.d.ts
```

## Rebuilding the bundle

The `pkg/` directory is **committed** so test runs don't require a
Rust toolchain. To rebuild after a tf-multiverse encoder change:

```sh
cd <tf-multiverse>/portal/leptos-input-encoder-test-client
wasm-pack build --release --target web \
  --out-dir <chromeless>/tests/webrtc/scenarios/portal-test-client/pkg
```

Bundle size budget: the release build with `lto = true / opt-level = "s"`
clocks in around 55 KB raw / 20 KB gzipped. If it exceeds 200 KB raw,
investigate before committing — the encoder is intentionally narrow.

## Loading the page

The fixture expects a `?ws=ws://...` query param pointing at the WS
server it should forward to. Test scenarios pass this when navigating:

```js
await Page.navigate({
  url: `file://${HERE}/index.html?ws=${encodeURIComponent(stub.url)}`,
});
```

Once init runs, `window.__cbtest` is populated:

```js
window.__cbtest = {
  sent: [/* every encoded JSON string */],
  counts: { mouse: N, scroll: N, key: N, error: N },
  ws_state: "init" | "connecting" | "open" | "closed" | "error",
  last_error: null | string,
  config: { ws_url, target_id },
}
```

Scenarios assert against both the stub-WS-side received messages
*and* `window.__cbtest.sent` — they should match shape-for-shape.

## Relationship to scenarios 20-22

20-22 exercise a different wire path entirely: the chromeless
DataChannel → input-bridge sidecar → CDP. They use `WireInputBag` +
`fixture-input-mirror.html` and simulate the chromeless TS client's
view of the world.

30+ exercise the **portal**'s wire path: DOM event → portal's encoder
→ physics's `/ws/screencast?mode=input`. Same chromium, different
clients, different wire formats. Don't conflate.

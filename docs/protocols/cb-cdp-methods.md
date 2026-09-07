# `Cb.*` CDP methods

The embedder's private CDP domain — the control surface physics and the
isolator drive the guest through.

> **Status:** current as of the `feat(viewport)` landing. Additive; no
> method here has ever changed shape after shipping.

---

## Read this first: these methods are hand-dispatched

There is **no PDL and no `/json/protocol` entry** for this domain.
`CbDevToolsManagerDelegate::HandleCommand`
(`capture/build-integration/cb_devtools_agent.cc`) intercepts the method
name off the CBOR envelope, and anything it does not recognise falls
through to Chromium's dispatcher.

Two consequences that have each produced a production bug:

- **A misspelled or unknown parameter is silently ignored.** Params are
  read with bare `FindString` / `FindBool` / `FindInt` calls, so a key the
  guest does not recognise costs nothing and reports nothing. Two
  production bugs came from exactly this: a TLS flag whose reader and
  sender disagreed on the name, and `iceServers` being read only in its
  string form so an array silently fell back to public STUN. If you add a
  parameter, add it to the validation path too — and prefer an explicit
  presence check over `value_or(default)`, so a typo fails loudly instead
  of taking a plausible default.
- **Clients must connect with `local: true`.** `GET /json/protocol`
  CHECK-FATALs the worker, so an ordinary `chrome-remote-interface`
  connect SIGABRTs the browser. Nine test files carry this workaround.

Every method is **browser-scope**: it acts on process-global state and
reaches the page through main_parts' active-WebContents resolver, not
through `channel->GetAgentHost()`. That is what lets the isolator drive
them over a browser-scope session without attaching per page.

---

## `Cb.startFrameSinkCapture`

Resolve the active WebContents' `FrameSinkId` and point the video capturer
at it. Also forces the captured view SHOWING — a fresh RenderWidgetHost in
this offscreen setup starts HIDDEN and would stay BeginFrame-throttled.

**Params:** none.

**Returns:** `{"started": true, "frameSinkId": "<n:m>"}`

Safe to call repeatedly. A second call with the same target is a no-op; a
different `FrameSinkId` **retargets** the running capturer, which is how
capture follows a cross-document navigation's RenderWidgetHost swap.

---

## `Cb.startNativeSession`

Bring up the native WebRTC signaling session at runtime with per-session
parameters, instead of the cold-boot environment path. This is what a
warm-snapshot restore calls after the VM resumes.

**Params:**

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `signalingHost` | string | **yes** | |
| `signalingSessionId` | string | **yes** | |
| `signalingToken` | string | no | |
| `useTls` | bool | no | Defaults **true**; only an explicit `false` forces plain `ws://` |
| `iceServers` | **string** | no | A JSON string, not an array — read with `FindString` and parsed by `ParseIceServersJson`. Sending a JSON *array* here is silently ignored and falls back to the default single STUN server. |
| `iceTransportPolicy` | string | no | Absent leaves the default (`all`) |

**Returns:** `{"started": true, "sessionId": "..."}`

**Not idempotent by design.** A second call returns `INVALID_STATE`
without mutating anything. The isolator treats any error from this method
as fatal and releases the VM.

---

## `Cb.getCaptureStats`

Read-only poll of the frame-production counter. No side effects; does not
start capture.

**Params:** none.

**Returns:** `{"framesReceived": <int32>}`

`framesReceived` is `frames_received_from_capturer` — the capturer→ingress
delivery count. Growing means the renderer is PRODUCING; flat at zero means
RENDERER-STARVED (the `FrameSinkVideoCapturer` is a pull-consumer, so with
no BeginFrame the compositor never produces and the counter never moves).

The isolator gates warm-snapshot golden publication on this: a golden
captured from a starved renderer poisons every cold-start restored from it
with permanently-frozen video.

> **Compatibility:** `framesReceived` must stay first and must never be
> renamed — `isolator/src/vmm/pool.rs` reads it. CBOR maps are
> order-insensitive, so *adding* fields is safe in both skew directions.

---

## `Cb.setViewport`

Resize the browser. Applies to the display, the aura host, the captured
WebContents' view, and the capturer's output resolution as one unit.

**Params:**

| Field | Type | Required | Notes |
| --- | --- | --- | --- |
| `width` | int | **yes** | DIP |
| `height` | int | **yes** | DIP |
| `deviceScaleFactor` | number | no | **Omitted means keep the current scale, NOT reset to 1.0** |

**Returns:** the viewport **actually applied** —
`{"width": <int>, "height": <int>, "deviceScaleFactor": <double>}`

### Read the response back

The returned values may differ from what was requested. Callers must treat
the response as authoritative rather than assuming their request landed
verbatim — a silently-clamped resize that the caller believes succeeded is
how geometry disagreements start.

Clamping applied, in order:

1. Each dimension to `[200, 2560]`.
2. Each dimension down to an even number (I420 subsamples chroma 2×2; an
   odd dimension leaves a half-sampled edge). **Down**, never up, so
   alignment cannot push a value back over a ceiling it was just clamped
   to.
3. `deviceScaleFactor` to `[1.0, 1.0]` — HiDPI is not enabled yet (see
   below).

The 2560 ceiling is a backstop, not the policy. The guest is a Firecracker
microVM with 2048 MB and (today) one vCPU; because `is_screencast()`
selects `MAINTAIN_RESOLUTION`, an oversized software encode does not
degrade gracefully — libwebrtc holds resolution and drops frame rate, so
the session becomes a slideshow rather than a smaller picture. **Physics is
expected to apply its own per-tier cap on top of this.**

### Why CDP and not the input DataChannel

The input channel is an untrusted client→guest path. A guest that resized
itself straight off it would let a hostile client request 8192×8192 and OOM
the microVM. Physics already receives the resize and holds the tier cap, so
routing through CDP puts the arbiter in the path by construction. The
structured ack is the other half: it is what lets physics observe a clamp
instead of guessing.

Latency is not a concern — the portal already debounces resize at 250 ms.

In a standalone deployment the caller is the gateway's `POST /api/viewport`
(`infra/gateway/viewport.go`), driven by `client/src/viewport.ts`, which watches
the stage with a `ResizeObserver`, debounces at the same 250 ms, applies the
same clamp before sending, and stops asking after one failure so a guest that
predates this method costs one request rather than one per resize.

### HiDPI is not enabled yet

`deviceScaleFactor` is accepted and clamped to 1.0. The blocker is
coordinate space, not rendering: the portal derives pointer coordinates
from `video.videoWidth`/`videoHeight`, i.e. guest **pixels**, while the
input dispatchers expect **DIP**. The guest must divide — it set the scale
and knows it authoritatively, and that keeps the portal's self-correcting
property. Until that normalization lands in
`CbInputDispatchCompositeDelegate::OnInputEvent`, a DSF ≠ 1.0 would put
every click in the wrong place, which is a worse failure than no HiDPI
because it looks like it works.

### DIP vs pixels

The single easiest thing to get wrong here, so the convention is total:

| Surface | Unit |
| --- | --- |
| `Cb.setViewport` `width`/`height` | DIP |
| `display::Display` bounds (`SetScaleAndBounds`) | pixels |
| `WindowTreeHost::SetBoundsInPixels` | pixels |
| `RenderWidgetHostView::SetSize` | DIP |
| capturer resolution / encoded frame | pixels |

`pixels = dip × dsf`. At DSF 1.0 every one of these is numerically
identical — which is exactly why a DSF-1.0-only test cannot catch a
mistake here.

---

## Verifying a deployed guest carries these

Each method logs a grep-able marker, so a stale guest image can be
distinguished from a caller-side wiring bug without attaching a debugger:

```sh
grep -a CV2-CAPTURE-STATS <binary>   # Cb.getCaptureStats
grep -a CV2-VIEWPORT     <binary>    # Cb.setViewport
```

Callers should tolerate `-32601` (method not found) so that portal/physics
changes can ship ahead of a guest image roll. That tolerance is what makes
the two repos independently deployable; see
`isolator/src/vmm/pool.rs` for the established pattern.

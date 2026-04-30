# Prior art: Neko

**Project:** [m1k1o/neko](https://github.com/m1k1o/neko) — a self-hosted virtual
browser that streams a containerized X11 desktop to one or more concurrent
viewers over WebRTC.

**Stack at a glance:** Go server (Gorilla WebSocket + Pion WebRTC + cgo
GStreamer wrapper), Vue/TypeScript client, X11 desktop with Xorg/Xvfb
running an off-the-shelf browser (Firefox, Chromium, Brave, Vivaldi, …) or a
DE (XFCE, KDE).

---

## 1. Architecture

```
┌────────────────────── container ──────────────────────┐     ┌────────── browser ──────────┐
│  Xorg/Xvfb  ──►  Browser/DE                            │     │                              │
│       │                                                │     │   client/  (Vue/TS)          │
│       ▼                                                │     │     ├─ websocket signal     │
│  ximagesrc / pulsesrc                                  │     │     ├─ RTCPeerConnection    │
│       │                                                │     │     └─ DataChannel cursor   │
│  GStreamer pipeline (server/pkg/gst, cgo)              │     │                              │
│  vp8enc | vp9enc | x264enc | h264 (vaapi) | opusenc    │     │                              │
│       │                                                │     │                              │
│  appsink ─►  capture/streamsrc.go ──► Pion track       │     │                              │
│                                                        │     │                              │
│  internal/webrtc      manager.go / peer.go / handler.go│◄───►│  WebRTC media + DC           │
│       │                                                │     │                              │
│  internal/websocket   manager.go / handler/{signal,    │◄───►│  WS JSON signaling           │
│       │               control, keyboard, clipboard,    │     │                              │
│       │               screen, system, session}.go      │     │                              │
│       ▼                                                │     │                              │
│  internal/desktop  (XTest input injection, xclip,      │     │                              │
│                     file-chooser dialog scripting)     │     │                              │
│                                                        │     │                              │
│  internal/session  (members + roles, single shared     │     │                              │
│                     "room" per container)              │     │                              │
└────────────────────────────────────────────────────────┘     └──────────────────────────────┘
```

**Capture.** Neko shells out to GStreamer through a cgo wrapper at
`server/pkg/gst/{gst.c,gst.h,gst.go}`. The capture manager
(`server/internal/capture/`) builds a pipeline from a config string — typical
shapes are `ximagesrc use-damage=0 ! videoconvert ! queue ! vp8enc ...` for
video and `pulsesrc ! audioconvert ! opusenc` for audio. `streamselector.go`
runs multiple encoders in parallel for bitrate ladders. X11 is the supported
display server; Wayland is not a first-class path. `screencast.go` calls
`gst.CreatePipeline()` → `AttachAppsink()` and pushes samples into the
WebRTC track via `streamsrc.Push()`.

**Transport.** Pion WebRTC. `internal/webrtc/manager.go::CreatePeer` builds a
`MediaEngine`, applies a `SettingEngine` (UDP mux via
`ice.NewMultiUDPMuxFromPort`, optional `ice.NewTCPMuxDefault`, ICE-Lite
toggle), and calls `api.NewPeerConnection(cfg)`. `peer.go` wires
`OnICECandidate` (trickled over WS), `OnTrack` (mic/webcam upstream),
`OnDataChannel`, `OnConnectionStateChange`. RTP samples from GStreamer are
pushed track-side; nothing is re-encoded in Go.

**Signaling.** Gorilla WebSocket, JSON envelopes.
`internal/websocket/manager.go` upgrades the request and dispatches to
`handler.Message(session, data)`. Handlers split by family:
`handler/signal.go` (offer/answer/ICE), `control.go` + `keyboard.go` (host
arbitration + input), `clipboard.go`, `screen.go`, `system.go`,
`session.go`. A `legacyhandler.go` keeps older clients working — Neko has
lived through several protocol versions.

**Session model.** One container == one shared "room". Every member is a
peer of the same desktop; exactly one holds the **host** lock and gets
keyboard/mouse passthrough, the rest watch. `internal/session/` and
`internal/member/` track identity and roles, optionally serializing into
Redis. The companion project [m1k1o/neko-rooms](https://github.com/m1k1o/neko-rooms)
spawns multiple Neko containers when you want N independent rooms — Neko
itself does not orchestrate.

**Input.** X11 only. The server uses XTest for keyboard/mouse injection and
shells out for clipboard and the file-chooser dialog
(`internal/websocket/filechooserdialog.go`). Cursor shape and position are
sent over a data channel and rendered client-side (`internal/webrtc/cursor/`).

---

## 2. Strengths worth adopting

- **Pipeline-as-config.** GStreamer pipelines are config strings; swapping
  vp8 → vp9 → x264 → vaapih264enc is an edit, not a rebuild. Useful template
  for our encoder-factory hot-swap goal.
- **Pion + cgo-GStreamer split** is clean: Go owns lifecycle and signaling,
  GStreamer owns the hot media path.
- **Stream selector** lets one capture feed multiple encoders at different
  resolutions/bitrates — maps onto our future simulcast/SVC story.
- **Trickle ICE + UDP/TCP mux + ICE-Lite toggle** are configurable from day
  one. Carry the same flexibility into our signaling.
- **Handler-per-message-family** keeps the WS protocol readable: `signal.go`
  does only SDP/ICE, `control.go` only host arbitration. Easy navigation.
- **Browser variants pre-baked.** Separate Docker images for Firefox /
  Chromium / Brave / Tor — good UX template.
- **Cursor over data channel** instead of in-frame — well-known latency win,
  copy the wire format in Phase 2.

---

## 3. Cut corners / known limits

- **One Chromium per room, shared by everyone.** Fine for watch parties,
  wrong shape for a per-user cloud browser. No isolation inside a room.
- **X11-only capture.** No Wayland, no headless-Chromium-direct path, no
  Viz `FrameSinkVideoCapturer`. `ximagesrc` is a screen scrape, not a
  compositor tap — extra copy and conversion overhead.
- **Software encode is the default.** VAAPI works but isn't the happy path;
  NVENC is community-maintained. Encoders aren't tuned for sub-100 ms (no
  zero-latency tuning, intra-refresh, or small-GOP profiles enforced).
- **No per-session orchestration.** `neko-rooms` is "spawn another container
  behind nginx." No warm pool, no snapshot/restore.
- **Mobile/touch is weak.** Pointer + keyboard assumed; gestures and IME
  affordances are best-effort.
- **Audio is Opus default-config.** No DTX, no FEC tuning, no jitter-buffer
  control surface.
- **Auth is minimal** — shared password per room, OAuth only via reverse
  proxy. Not a multi-tenant SaaS auth model.

---

## 4. What we'd reuse

- **Pion + Gorilla WS signaling shape** for our `signaling/` server: WS
  upgrade, JSON `{event, payload}` envelopes, a `signal.go`-style handler
  that owns SDP/ICE only, trickle ICE on by default. T13 should crib from
  `internal/websocket/handler/signal.go`.
- **MediaEngine + SettingEngine pattern** in `webrtc/manager.go`: register
  codecs against the active encoder, configure UDP mux up front.
- **Cursor-over-data-channel** (`internal/webrtc/cursor/`) — adopt the wire
  format nearly as-is in Phase 2.
- **Per-message-family handler layout** for our WS protocol.
- **`legacyhandler.go` pattern** — version the protocol from day one.

---

## 5. Where we diverge

- **One Chromium per user, not per room.** Our brief is explicit: "never
  share Chromium across users." Neko's room model is the wrong default;
  multi-watcher becomes a Phase-4 collaboration feature.
- **No host-lock arbitration.** Single user means no host/guest role; we
  drop `control.go`'s host-passing logic entirely.
- **Capture leaves X11.** v1 uses `getDisplayMedia` from a privileged
  Chromium tab — no `ximagesrc` round trip. Phase 2 taps Viz's
  `FrameSinkVideoCapturer` inside Chromium. The GStreamer cgo dependency
  doesn't survive past v1.
- **Encoder factory in libwebrtc**, not GStreamer.
  `webrtc::VideoEncoderFactory` overrides make NVENC/VAAPI a C++ injection,
  not a pipeline string. Different extension point, same goal.
- **Latency is a measured budget.** Neko advertises <300 ms; our LAN budget
  is <100 ms. Forces tuned encoders, jitter-buffer control, and the harness
  from week 1.
- **Per-session orchestration is in scope.** Container-per-session, warm
  pool, observability are Phase 3 deliverables, not a separate project.

---

## 6. Citations

- Repo root: <https://github.com/m1k1o/neko>
- Server entry: `server/neko.go` (version metadata; real wiring is in
  `cmd/`) — <https://github.com/m1k1o/neko/blob/master/server/neko.go>
- Capture package: `server/internal/capture/` —
  <https://github.com/m1k1o/neko/tree/master/server/internal/capture>
- GStreamer cgo wrapper: `server/pkg/gst/{gst.c,gst.h,gst.go}` —
  <https://github.com/m1k1o/neko/tree/master/server/pkg/gst>
- WebRTC package: `server/internal/webrtc/` (`manager.go`, `peer.go`,
  `handler.go`, `legacyhandler.go`, `track.go`, `cursor/`, `payload/`,
  `pionlog/`) —
  <https://github.com/m1k1o/neko/tree/master/server/internal/webrtc>
- WebSocket signaling: `server/internal/websocket/` (`manager.go`,
  `peer.go`, `filechooserdialog.go`, `handler/{signal,control,keyboard,
  clipboard,screen,system,session,send,handler}.go`) —
  <https://github.com/m1k1o/neko/tree/master/server/internal/websocket>
- Session/member: `server/internal/session/`, `server/internal/member/` —
  <https://github.com/m1k1o/neko/tree/master/server/internal/session>
- Multi-room orchestration (separate repo): `m1k1o/neko-rooms` —
  <https://github.com/m1k1o/neko-rooms>
- Project site: <https://neko.m1k1o.net/>

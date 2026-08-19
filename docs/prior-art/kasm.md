# Prior Art: Kasm Workspaces

**Project:** Kasm Workspaces (commercial platform) + KasmVNC (open-source server)
**Repos:** https://github.com/kasmtech/workspaces-core-images,
https://github.com/kasmtech/workspaces-images,
https://github.com/kasmtech/KasmVNC
**Site / docs:** https://www.kasmweb.com/, https://www.kasmweb.com/docs/latest/index.html
**License:** KasmVNC server is GPL-3.0; the Workspaces orchestration platform
is a commercial product with a free Community Edition.

Kasm is the most operationally mature container-streaming product in the open
Linux remote-desktop space. They have shipped the full multi-tenant story —
image catalog, agent fleet, RBAC, session lifecycle, audit, RDP/SSH passthrough
— for years. They are also the wrong shape for what we are building: their
streaming engine is a modernized VNC, not a WebRTC-native pipeline, and almost
every architectural decision downstream of that choice differs from ours.

---

## 1. Architecture

```
                      +-------------------------+
                      | Browser (HTTPS :443)    |
                      | KasmVNC noVNC client    |
                      +-----------+-------------+
                                  |
                       WebSocket  |  optional WebRTC UDP
                       (TLS)      |  data channel upgrade
                                  v
+---------------------------- Web App role ----------------------------+
| nginx proxy  ->  API service  ->  Manager service  ->  Share service |
|                          |                  |                        |
+--------------------------|------------------|------------------------+
                           |                  |
                           v                  v
                    +------+------+    +------+------+
                    | Postgres    |    | Agent host  |  (1..N)
                    | Redis       |    |  +--------+ |
                    +-------------+    |  | docker | |
                                       |  | engine | |
                                       |  +---+----+ |
                                       |      |      |
                                       |  +---v---------------------+|
                                       |  | per-session container   ||
                                       |  |  kasm-user (uid 1000)   ||
                                       |  |  XFCE / single app      ||
                                       |  |  KasmVNC server         ||
                                       |  |  PulseAudio :4901       ||
                                       |  |  noVNC https :6901      ||
                                       |  +-------------------------+|
                                       +----------------------------+
```

Roles in a multi-server install
([multi_server_install](https://kasm.com/docs/latest/install/multi_server_install.html)):

- **Web App** — API + Manager + Share + nginx proxy. Public entrypoint on 443.
- **Database** — PostgreSQL (5432) + Redis (6379).
- **Agent(s)** — host the Docker daemon and provision per-session containers
  on demand. Check in to the Manager for assignment and health.
- **Connection Proxy (optional)** — Apache Guacamole + RDP gateway for
  RDP/VNC/SSH passthrough to non-Kasm endpoints.

Inside a workspace container
([workspaces-core-images](https://github.com/kasmtech/workspaces-core-images)):

- Base distros: Ubuntu, Debian, Alpine, Fedora, openSUSE, KasmOS.
- Entrypoint chain (no supervisord — sequential bash):
  `kasm_default_profile.sh` → `vnc_startup.sh` → `kasm_startup.sh`
  ([dockerfile-kasm-core-suse](https://github.com/kasmtech/workspaces-core-images/blob/develop/dockerfile-kasm-core-suse)).
- Ports: `5901` raw VNC, `6901` noVNC over HTTPS, `4901` audio.
- Users: `kasm-user` (uid 1000) for the desktop, `kasm-recorder` (uid 1001)
  for session recording.
- Optional services per image: PulseAudio, webcam/gamepad/printer
  passthrough, Squid HTTP filter, smartcard, session recording.

KasmVNC itself ([repo](https://github.com/kasmtech/KasmVNC)) is forked from
TigerVNC, written in C++ with some C and Perl, and intentionally breaks the
RFB wire format to add WebSocket transport, JPEG / WebP / QOI encoding, an
optional WebRTC UDP data-channel transit
([1.0 release notes](https://kasmweb.com/kasmvnc/docs/master/release_notes/1.0.0.html)),
multi-threaded encode, and per-user YAML config.

## 2. Strengths

- **Operational maturity.** Multi-server topology, agent fleet, image
  catalog, RBAC, audit logs, session recording, and Guacamole-based RDP/SSH
  passthrough all ship today
  ([multi_server_install](https://kasm.com/docs/latest/install/multi_server_install.html)).
  This is years ahead of where Selkies / Neko sit operationally.
- **Container image catalog.** `workspaces-core-images` and
  `workspaces-images` together publish dozens of curated, maintained desktop
  and single-app containers (Chromium, Firefox, Brave, VS Code, Blender,
  Kali, etc.)
  ([workspaces-images](https://github.com/kasmtech/workspaces-images)). The
  layering convention — core image at the bottom, app install scripts on top —
  is clean and worth copying.
- **Browser-only client, agentless.** noVNC over HTTPS on a single port (443
  in front, 6901 inside) means no client install, no firewall pain. Phase 0
  alignment is direct.
- **Modern VNC encoders.** JPEG/WebP CPU-aware mixing, QOI lossless mode,
  multi-threaded encode, claimed 60+ FPS at 4K
  ([release notes](https://kasmweb.com/kasmvnc/docs/master/release_notes/1.0.0.html)).
- **Per-tenant isolation by construction.** One Docker container per
  session, one user per container. The orchestration plane assumes this from
  day one — exactly the posture our Phase 3 wants.
- **Admin UX.** Image catalog, group/policy mapping, autoscaled agent
  fleets, monitoring, and session lifecycle controls (idle timeout, max
  duration) are all in the web UI and API.

## 3. Cut corners / known issues

- **VNC, not WebRTC, is the primary transport.** The default path is
  noVNC-over-WebSocket; WebRTC UDP transit is an opt-in feature that lacks
  TURN support, so it is unusable when the server is behind NAT
  ([cendio comparison](https://www.cendio.com/blog/kasm-vnc-alternatives/),
  [DeepWiki](https://deepwiki.com/kasmtech/KasmVNC/7-performance-and-testing)).
  This caps interactive latency on anything but a LAN.
- **Image-based encoding, not video.** JPEG/WebP/QOI are still-image codecs
  with frame-differencing on top. There is no temporal compression, no
  bandwidth estimator, no FEC, no pacer. Lossless QOI at HD60 is documented
  to consume **>1 Gbps**
  ([1.0 notes](https://kasmweb.com/kasmvnc/docs/master/release_notes/1.0.0.html)) —
  fine on a LAN, infeasible across the public internet.
- **Desktop-shaped capture.** Capture is a screen scrape of an X session,
  not a tap into the browser compositor. No per-tab capture, no damage
  rectangles from the source app, no exact-frame timestamping.
- **No supervisord; bash chain instead.** The entrypoint is a sequence of
  shell scripts (`vnc_startup.sh` → `kasm_startup.sh`). Process supervision
  and restart semantics are ad hoc.
- **The control plane is not OSS.** Only KasmVNC and the image base layers
  are open. The Manager / Agent / Web App are commercial. Fork-and-extend is
  not on the table.
- **Mobile and IME stories are weak.** Browser-VNC clients consistently
  trail native clients on touch/IME/clipboard fidelity, and KasmVNC is no
  exception. ([cendio comparison](https://www.cendio.com/blog/kasm-vnc-alternatives/))

## 4. What we'd reuse

- **Two-layer image structure.** `workspaces-core-images` (base distro +
  KasmVNC + audio + entrypoint) underneath `workspaces-images` (per-app
  install scripts). Our `infra/` containers should adopt the same split
  ([core](https://github.com/kasmtech/workspaces-core-images),
  [images](https://github.com/kasmtech/workspaces-images)).
- **`kasm-user` (uid 1000) convention.** A non-root, fixed-uid user inside
  the container avoids host-permission grief on bind mounts and matches
  community expectation.
- **Port discipline.** Single externally-exposed HTTPS port per container,
  internal services on conventional internal ports — keeps the K8s story
  simple.
- **Image catalog API shape.** When we get to Phase 3 orchestration, Kasm's
  registered-image / per-group policy model is a good reference for the
  control-plane data model.
- **Session recording hook.** A separate uid (`kasm-recorder`) reading the
  same display is a cleaner pattern than wedging recording into the encode
  pipeline.

## 5. What we'd diverge on and why

- **WebRTC-native, not VNC.** This is the whole reason the project exists:
  we want libwebrtc's GCC/BWE, FEC, RED, pacer, and a real video codec
  pipeline, not WebSocket-framed image deltas. Even Kasm's WebRTC mode is
  bolted onto a VNC core and lacks TURN.
- **Video codecs, not image codecs.** VP9/AV1/H.264 with intra-refresh and
  small GOPs absolutely demolish JPEG/WebP frame-diffing on bandwidth at
  equivalent perceived quality on the public internet. QOI's
  >1 Gbps lossless mode is irrelevant outside a LAN.
- **One Chromium per session, not a desktop per session.** Kasm streams an
  XFCE desktop with apps inside it; we stream a single browser. Smaller
  attack surface, smaller image, no window manager, no IME stack to debug.
- **`supervisord` (or systemd-nspawn) over a bash entrypoint chain.** We
  want first-class process supervision and crash semantics for Chromium,
  Xvfb, PulseAudio, and the capture/encode bridge — `vnc_startup.sh` style
  is too thin.
- **Capture inside Chromium, not over X.** Phase 2 hooks Viz's
  `FrameSinkVideoCapturer`; Phase 1 uses `getDisplayMedia`. Either way the
  bits never leave Chromium's process tree, which Kasm's design cannot
  match.
- **Open-source control plane.** Our orchestrator, signaling, and admin
  surfaces must all be OSS so the project is forkable end-to-end.

---

**Bottom line:** Kasm is the operational reference — image layout,
per-session container model, port discipline, RBAC and image-catalog
ergonomics — and the architectural anti-reference. Their VNC-and-image-codec
core is exactly the foundation we are choosing not to inherit, and it
justifies the full WebRTC + Chromium-internal-capture stack on the other side
of Phase 1.

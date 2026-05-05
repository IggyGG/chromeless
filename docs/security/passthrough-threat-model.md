# Passthrough threat model — webcam + microphone

> **Status:** Phase 4 (T81). Reviewed by `platform-dev`; pending
> review from `qa-tester` (test plan) and a security review pass
> before any production rollout.
>
> **Companion:** [`../protocols/webcam-mic-passthrough.md`](../protocols/webcam-mic-passthrough.md)
> (the protocol design).

This document enumerates the attack surface introduced by allowing
the client's camera and microphone into the cloud Chromium, and the
controls that mitigate each risk.

---

## Why this is a real risk

Until T81, the cloud-browser data plane is **strictly downstream**:
the cloud Chromium streams its viewport + audio to the user, and
the user sends back keyboard/mouse/clipboard events. Adding camera
and microphone inverts that: the user's physical environment
becomes legible to a Chromium tab that runs untrusted web pages on
their behalf.

A page loaded inside the cloud browser that the user *thinks* they
control could:

1. Record their face/voice and exfiltrate it to an attacker.
2. Use the camera as a side channel for fingerprinting (background,
   ambient noise, etc).
3. Spoof presence — the user thinks they hung up a Meet call but
   the camera is still streaming because a different tab is
   holding the loopback.

These are not new browser-platform risks — they exist whenever any
browser runs an untrusted page with camera permission. The novelty
here is that the camera is *specifically* the user's, but accessed
through a Chromium that is one degree further removed from their
control than their local browser is.

---

## Threat model

| Threat | Likelihood | Impact | Mitigation |
| ------ | ---------- | ------ | ---------- |
| **T1.** A page inside the cloud Chromium accesses the camera without an explicit consent dialog the user attributes to it. | High if no controls | High | Permission gating chain (below). The streamer page proxies a single user-visible prompt to the local user; Chromium's `--use-fake-ui-for-media-stream` is **dev-only**. |
| **T2.** Camera/mic content is sent to a third-party DC after a Meet-style call has visibly ended. | Medium | High | Track lifetime is bound to the user's `getUserMedia` MediaStream + the WebRTC peer connection; client-side "Stop sharing" button hard-stops `track.stop()` AND issues `request_renegotiate` to remove the m= sections. Chromium-side: streamer page owns the v4l2 sink and unbinds it on `oninactive`. |
| **T3.** A new session reuses a previous session's loopback device and sees stale frames. | Low | Medium | Per-session loopback device claim (see deployment shape 2). On Pod teardown the device is released; the controller never re-mounts a device that hasn't been zeroed. |
| **T4.** The client side enumerates cameras the user did NOT grant access to. | Low | Low | We pass exact device-id constraints to `getUserMedia`; never `enumerateDevices()` without an existing grant. |
| **T5.** Camera content is leaked through the metric / log paths. | Low | Medium | Metrics emitted by the bridge are bytes-sent counters only; no content. Log lines never include any frame payload. |
| **T6.** The privileged container path lets a compromised session escape to the host. | High in privileged shape | High | Privileged shape is **dev-only**. Production uses the host-DaemonSet shape — only the DaemonSet is privileged, session Pods remain non-privileged. |
| **T7.** A user-loaded page enumerates the **real** host's cameras (not the loopback). | Low | Medium | Chromium policy — `permissions.devices.video.allowed_devices` is configured to expose only the loopback device id; the host's cameras are not on the device list inside Chromium. |
| **T8.** Camera content is captured by the bandwidth-estimator metric exporter that T58 wires for downstream encoder tuning. | Low | Low | BWE sees encoded byte counts and packet timings, not pixel data. The same code path already runs on the downstream direction without leaking screen content. |
| **T9.** A page inside cloud Chromium MITMs the loopback device path and records frames before they reach the consuming page. | Low | Medium | Pages inside Chromium can't open `/dev/video10` directly — only `getUserMedia` can. A malicious *Chromium extension* could; we run with `--disable-extensions` per T57. |
| **T10.** Camera/mic state survives a session reconnect (T37) and the user can't tell. | Medium | Medium | On any peer-connection rebuild (T37), the client side re-prompts before re-enabling passthrough; the server side teardown of `oninactive` triggers a fresh permission flow. |

---

## Permission gating chain

Camera/mic content reaches the cloud Chromium **only when all four
gates pass**, in order:

```
   user clicks "Share camera/mic" in client UI
     │
     ▼
   navigator.mediaDevices.getUserMedia({video, audio})
     │  ↓ user-agent prompt — UA-controlled; we never bypass
     ▼
   pc.addTrack(track, stream); negotiationneeded fires
     │
     ▼
   streamer page launched with ?passthrough=true (orchestrator-set)
     │
     ▼
   pod scheduled with chromeless.passthrough/device-claim allocation
     │
     ▼
   v4l2 sink active for THIS session; cloud Chromium sees
   "Cloud Browser Camera" in its getUserMedia device list
```

If any gate is false, the track is rejected at that stage:
- No client UI click → no `getUserMedia` call.
- UA prompt denied → `getUserMedia` rejects; the client logs the
  error and never adds the track to the peer connection.
- Streamer not configured for passthrough → the streamer's
  `ontrack` handler logs a warning and immediately calls
  `track.stop()`. The track never reaches the v4l2 sink.
- No device claim → the v4l2 writer fails to open `/dev/video10`
  and the streamer reports `passthrough_unavailable` to the
  client.

---

## Per-tenant configuration

Camera/mic passthrough is **off by default per tenant**. Enabling
it requires:

1. The tenant config has `passthrough.camera.enabled = true` AND/OR
   `passthrough.microphone.enabled = true`.
2. The orchestrator passes `?passthrough=true&audio=…&video=…` to
   the streamer launch URL when scheduling a session for that
   tenant.
3. The session pod has a `chromeless.passthrough/device-claim` annotation
   set by the controller.

Tenants who don't enable passthrough get the existing data plane
unchanged — no v4l2 module, no Pod-level requirements, no client
UI button surfaced.

---

## Opt-out and revocation

- **At session start:** the client UI never auto-enables. The user
  must click "Share camera/mic" each time. There is no
  "remember-this-decision" persistence in v1.
- **During a session:** the client UI shows a "Stop sharing"
  toggle that calls `track.stop()` and triggers a renegotiation to
  drop the m= sections. The streamer's `oninactive` handler
  releases the v4l2 sink within ~1 second.
- **On disconnect:** any peer-connection close stops all senders.
  Session-pod teardown unloads the per-session v4l2 device.
- **Page-load defense:** the streamer page logs a warning if the
  inbound track outlives the user's MediaStream — this should
  never happen but the diagnostic is cheap and might surface a
  bug.

---

## What's deferred

- **Recording-disclosure UI** — a green "camera active" pill in
  the client overlaying the video element. Phase 4 polish.
- **Per-page consent prompts** — distinguishing "Cloud Browser
  Camera" requests from same-page-Meet vs. another tab. Phase 4
  policy work.
- **Audit log** — every track-add / track-stop event logged with
  upload_id-style correlation. Phase 4 observability.
- **Encrypted-at-rest temporary frames** — currently frames pass
  through a Unix socket → v4l2 device, both in-memory; if a
  privileged process snapshots `/proc/.../mem`, frames are
  visible. Phase 5 hardening.

---

## Sign-off requirements before production rollout

This document is the floor, not the ceiling. Before
camera/mic passthrough is enabled for any production tenant:

1. A second pair of eyes from outside `platform-dev` reviews this
   threat model.
2. The host-DaemonSet deployment shape is implemented and tested —
   the privileged-container path is **never** used in production.
3. The "Stop sharing" UI flow is verified end-to-end including the
   renegotiation drop.
4. A pen-test pass exercises threats T1–T10 against a real
   deployment.
5. The recording-disclosure UI lands.

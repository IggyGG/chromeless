# v4l2-writer

> **Status:** Phase 4 (T92). Closes the webcam/mic passthrough loop
> opened by [T81](../../docs/protocols/webcam-mic-passthrough.md).
> **Owner:** `platform-dev`.

Native helper that consumes the streamer-page → Unix socket frame
stream and writes:

- **`--mode=video`** → `v4l2loopback` device (`/dev/video10` by
  default) as I420 frames so cloud Chromium's `getUserMedia` sees
  it as a real camera.
- **`--mode=audio`** → PulseAudio sink (via `pacat --playback
  --raw`) so cloud Chromium's `getUserMedia({audio:true})` sees a
  real microphone.

Two instances run per session pod — one each, as separate sidecar
containers (per K8s manifest). The wire format is the one
documented in
[`docs/protocols/webcam-mic-passthrough.md`](../../docs/protocols/webcam-mic-passthrough.md):
length-prefixed I420 / S16LE frames with magic-tagged init headers.

## How

```
   streamer.js (capture/streamer-page) installs
   pc.ontrack handlers and pushes:
                         ┌──────────────────────────────┐
   inbound video track ─►│  /run/chromeless-passthrough/        │
                         │      video.sock              │
                         └──────────┬───────────────────┘
                                    ▼
                         ┌──────────────────────┐
                         │  v4l2-writer         │
                         │  --mode=video        │
                         │  ioctl VIDIOC_S_FMT  │
                         │  write(I420 frames)  │
                         └──────────┬───────────┘
                                    ▼
                              /dev/video10
                                    │
                                    └─► cloud Chromium
                                        getUserMedia({video:true})
                                        → "Cloud Browser Camera"
```

Audio path is symmetric, via `pacat` into the PulseAudio sink
loaded by `infra/pulse-default.pa`.

## Build and run

```bash
go build -o v4l2-writer ./cmd/v4l2-writer

# Video sidecar (compose-dev)
./v4l2-writer --mode=video \
              --socket=/run/chromeless-passthrough/video.sock \
              --device=/dev/video10

# Audio sidecar (compose-dev)
./v4l2-writer --mode=audio \
              --socket=/run/chromeless-passthrough/audio.sock \
              --pulse-sink=cb_passthrough
```

The streamer page (`capture/streamer-page/streamer.js`) connects
to these sockets when launched with `?passthrough=true`. The
streamer is the *client* of these sockets, the writer is the
*server* — connection direction matches the "I'm a sink" mental
model.

## Flags

| flag             | default                              | meaning                                                      |
| ---------------- | ------------------------------------ | ------------------------------------------------------------ |
| `--mode`         | (required)                           | `video` or `audio`.                                          |
| `--socket`       | (required)                           | Unix-socket path to listen on. Match the streamer config.    |
| `--device`       | `/dev/video10`                       | v4l2loopback device path (video mode).                       |
| `--pulse-sink`   | `cb_passthrough`                     | PulseAudio sink name (audio mode).                           |

## Wire format

See
[`docs/protocols/webcam-mic-passthrough.md`](../../docs/protocols/webcam-mic-passthrough.md).
The init header is 16 bytes (magic + 3 × u32 BE) followed by
length-prefixed frames (`size(4) | pts_ms(4) | data[size]`, all
big-endian). Magic constants:

- video: `"CBV1"` (Cloud-Browser Video v1)
- audio: `"CBA1"` (Cloud-Browser Audio v1)

The writer rejects mismatched magic with `ErrBadMagic` and closes
the connection — protects against accidentally pointing the audio
sidecar at the video socket.

## Connection model

One connection at a time per writer. On peer hangup the writer
closes its sink, then accepts the next connection. This handles
the "streamer page reload" path cleanly — the new connection
re-sends the init header and the writer reopens the device.

If you want true concurrent uploads (e.g. two streamer instances
in the same pod), run two writer sidecars on different sockets.
The K8s manifest is set up for one each.

## Tests

```bash
go test ./...
```

- `internal/wire` — round-trip + bad-magic + truncated-stream +
  buffer-reuse + oversize-frame guards.
- `cmd/v4l2-writer` — flag parsing, handleVideo + handleAudio
  happy paths against fakes, mid-stream truncation surfaces as an
  error, listen() unlinks stale sockets, run() accepts multiple
  connections sequentially.

The Linux v4l2 ioctl path (`internal/v4l2/writer_linux.go`) is
build-tagged and only compiled on Linux. There's no automated
test for it because v4l2loopback isn't loadable in CI — the path
is exercised by the manual end-to-end smoke documented in T81's
threat model + this directory.

## Docker

```bash
docker build -t v4l2-writer .
```

Multi-stage; finishes in **debian-bookworm-slim**, NOT distroless,
because pacat needs `/etc/nsswitch.conf` for its PulseAudio
protocol lookups. Image size is ~120 MiB which is acceptable for a
sidecar that only ships in passthrough-enabled session pods.

The runtime user is uid `65532` (nobody) in gid `44` (`video`),
which gives /dev/video10 access without `--privileged`. The K8s
manifest in `infra/k8s/cloud-browser-session.yaml` mounts the
device with mode 0666 from the host-DaemonSet-loaded loopback so
the unprivileged session pod can write to it.

## Compose / K8s tie-in

- **Compose dev (privileged):** `infra/init-passthrough.sh`
  modprobes v4l2loopback inside the chromium container and the
  v4l2-writer sidecars run alongside, sharing the
  `chromeless-passthrough-sockets` emptyDir.
- **K8s production (host DaemonSet):** `infra/host-daemonset.yaml`
  loads v4l2loopback once per node with N pre-allocated devices.
  Session pods bind one device via a hostPath mount; the session
  pod itself is **not** privileged. This is the deployable shape.

## Known gaps

- **No reconnect to a wedged PA daemon.** If pacat exits because
  PulseAudio crashed, the audio writer surfaces the error to its
  caller and the connection closes; the streamer page reconnects
  on its next try, which spawns a fresh pacat. Phase 5 may add
  in-process retry, but the reconnect-from-streamer path covers
  the common cases.
- **No shutdown timer on pacat.** `Close()` does `cmd.Wait()` and
  trusts pacat to exit on stdin EOF (which it does ~100ms in
  practice). A wedged pacat would block teardown — file a
  follow-up if you see this in the wild.
- **v4l2 colorspace is hard-coded to REC.709.** That's correct for
  WebRTC's negotiated output but won't match a future SDR/HDR mix.
  Phase 5.
- **No metrics endpoint.** A Prometheus `/metrics` for
  bytes-written / connections / errors would tie into T38; not
  done in v1.

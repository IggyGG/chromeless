# Audio works for the first viewer only; every re-armed session is silent

**Status:** OPEN. Measured 2026-09-07 on the k8s standalone stack, worker
`cr7727-c5f2eb91c6f0` (digest `5813600c…`), three viewers on one process with
the viewport request blocked in the test browser so the resize defect
(`viewport-resize-freezes-beginframe-and-crashes-gpu.md`) could not confound
it. Lives in `capture/audio/` + `cloud_browser_browser_main_parts.cc`; needs a
lane build.

**Impact for a standalone deployment:** the first person to connect after the
browser starts gets sound. Everyone after that — including the same person
after a reload — gets video with no audio, for the life of the process. Since
the process now survives viewer changes (`RearmSession`), "for the life of the
process" is days. The client currently plays nothing anyway (`<video muted>`,
roadmap Track 1 item 2), which is why nobody has heard the silence.

## What happens

Same process (pid 21), three viewers in a row, each injecting a 440 Hz tone at
the worker over CDP and polling the client's `inbound-rtp` audio stats:

```
20:35:19  viewer 1: audio bytesReceived=2114          (fresh process)
20:36:15  viewer 2: audio bytesReceived=0    worker: failed to activate recording
20:37:13  viewer 3: audio bytesReceived=0    worker: failed to activate recording
```

`pid 21 -> 21` throughout; the guest's own `CV2-RTP outbound[audio]` counter
reads `packets_sent=0` for viewers 2 and 3. The one new line per re-armed
session is libwebrtc's:

```
ERROR:third_party/webrtc/modules/audio_device/linux/audio_device_pulse_linux.cc:1084] failed to activate recording
```

That is `AudioDeviceLinuxPulse::StartRecording` failing to connect its
PulseAudio record stream. It fires once per re-armed session, at the moment
libwebrtc starts the new session's audio send stream, and never on the first
session of a process.

## Why

The `AudioDeviceModule` is created **once per process** and handed to the
PeerConnectionFactory (`cloud_browser_browser_main_parts.cc`, the
`CreateCloudBrowserDefaultAudioDeviceModule()` block; `adm_for_audio_lifecycle_`
is a raw pointer to it). Every re-armed PeerConnection shares it. The session
layer above it was already made re-armable: `CbAudioLifecycle::Rearm()`
(`capture/audio/cb_audio_lifecycle.cc`) returns the lifecycle to `kIdle` so the
next session's transceiver/track/source can be adopted — that fix is what makes
the *transceiver* come back. It does nothing to the ADM. `StopInternal` stops
the transceiver and relies on libwebrtc's AudioState to "transitively" call
`ADM::StopRecording` when the last send stream goes; whatever state the pulse
ADM is left in, its next `StartRecording` → `ActivateRecording` fails.

`docs/findings/one-session-per-worker-process.md` predicted exactly this gap
under "Audio teardown" when the re-armable driver was proposed ("the orphan
window would stay open across every viewer change"), and `RearmSession`'s own
comment records the transceiver half being fixed ("without this the worker
keeps video but loses AUDIO from the second viewer onward"). This is the other
half.

## Why it looks like something else

- **`tests/e2e/05-audio-receives` fails with a message that blames PulseAudio
  routing or the Opus rtpmap** ("T24 PulseAudio null-sink regressed?"). The
  routing is fine — `pactl` shows `cb_audio` → loopback → `cb_capture` running
  and the first viewer proves the whole path — and the SDP carries Opus every
  time. The discriminator is **which viewer number** you are: restart the
  worker and run the spec once, it passes; run it twice, the second fails.
- **It reads as a flaky test.** It is deterministic: the first session on a
  process passes, every later one fails. A CI stack that boots a fresh worker
  per run (compose) never sees it; a long-lived stack sees it on every run but
  the first, which is how it stayed invisible behind the `DEVTOOLS_URL` skip.

## The fix

In `CloudBrowserBrowserMainParts::RearmSession` step 3b (next to
`audio_lifecycle_->Rearm()`), or inside `CbAudioLifecycle::Rearm()` if it is
given the ADM pointer it already logs (`adm_debug_`): make sure the pulse ADM's
recording is fully torn down before the next session's `StartRecording` —
`StopRecording()` (and if that is not enough, `Terminate()` + `Init()` +
`InitRecording()` on the worker thread, the same order
`cb_audio_device_module_test.cc` documents as load-bearing). Diagnose first with
a `LOG(INFO)` of `adm->Recording()` / `RecordingIsInitialized()` at the top of
`RearmSession`: if it reports still-recording, `StopInternal`'s "transitively
stops the ADM" assumption is false for this ADM and the explicit call is the
fix; if it reports stopped, the pulse stream was disconnected but not released
and the `Terminate()/Init()` cycle is.

## Verifying it

`tests/e2e/05-audio-receives.spec.ts` with `CHROMELESS_E2E_DEVTOOLS_URL` set,
run **twice against the same worker process** — the second run is the test.
Add the same to `tests/local/rearm-scenarios.spec.ts`: after the re-arm
scenario, assert inbound-rtp audio `bytesReceived > 0` on the second viewer.
The scratch driver that measured this is the shape to copy: block
`*/api/viewport` in the test browser (`Network.setBlockedURLs`) until the resize
finding is fixed, or the GPU crash will end the process before viewer 2.

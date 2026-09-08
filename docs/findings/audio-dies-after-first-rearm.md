# Audio works for the first viewer only; every re-armed session is silent

**Status:** OPEN, and THREE fixes have been tried and refuted by measurement.
Read "What has been ruled out" before proposing a fourth. Measured across
2026-09-07 and 2026-09-08 on the k8s standalone stack; the current guest
(`cr7727-fbd8e55c948b`) still has it.

**Impact for a standalone deployment:** the first person to connect after the
browser starts gets sound. Everyone after that — including the same person
after a reload — gets video with no audio, for the life of the process. Since
the process now survives viewer changes, that is days. The client plays
nothing today anyway (`<video muted>`, roadmap Track 1 item 2), so no user has
heard the silence yet; whoever unmutes it will.

## What happens

Three viewers in a row on ONE process, each injecting a 440 Hz tone at the
worker over CDP and polling the client's `inbound-rtp` audio stats. The
viewport follower is off, so the resize defect cannot confound it:

```
viewer 1: audio bytesReceived=2341     (fresh process)
viewer 2: audio bytesReceived=0        worker: failed to activate recording
viewer 3: audio bytesReceived=0        worker: failed to activate recording
```

pid unchanged throughout. The guest's own `CV2-RTP outbound[audio]` counter
reads `packets_sent=0` for viewers 2 and 3. The one new line per re-armed
session is libwebrtc's:

```
ERROR:third_party/webrtc/modules/audio_device/linux/audio_device_pulse_linux.cc:1084] failed to activate recording
```

## What has been ruled out

Each of these was implemented, built on the lane, deployed, and measured.

1. **"StopInternal's transitive stop does not run, so the ADM is still
   recording."** FALSE. The re-arm path was made to ask before acting, and the
   guest answered `recording=0 rec_initialized=0; StopRecording() rc=0`. It
   was already stopped; an explicit stop changes nothing.

2. **"A full Terminate/Init cycle would reset it."** UNSAFE, and would make
   things permanently worse. `AudioDeviceLinuxPulse::Terminate` sets `quit_`
   and **nothing in the file ever clears it** (one write, no reset), so the
   record thread would exit and never run again for the life of the process.

3. **"Pre-arm the ADM so libwebrtc skips its own start."**
   `AudioState::AddSendingStream` only calls `InitRecording`/`StartRecording`
   when `!adm->Recording()`, so putting the ADM into a recording state first
   should have made it attach to the running capture. The pre-arm SUCCEEDED —
   `StopRecording=0 InitRecording=0 StartRecording=0 now_recording=1` — and
   the session was still silent, with `failed to activate recording` appearing
   *after* the successful pre-arm. So libwebrtc is not taking the
   `!adm->Recording()` branch this reasoning assumed, or something between the
   send stream and the ADM re-enters the start path anyway.

The 4 ms detail that started theory 3 still stands and is still the sharpest
clue: `StartRecording` waits up to **ten seconds** on `_recStartEvent` for its
record thread to connect the stream, and the failure arrives 4 ms in. That is
a wait returning immediately on an event left SET by a previous session, then
finding `_recording` false. Nothing in the ADM clears that event between
sessions.

## The next probe (do this before writing any more code)

Stop instrumenting the ADM; instrument the layer above it. The question that
is still unanswered is **what libwebrtc does with the audio send stream on the
second session**, and every theory so far has guessed at it:

- Log around `AddSendSendingStream`/`RemoveSendingStream` — is a send stream
  even created for viewer 2, and does `AudioState` see `adm->Recording()` as
  true (theory 3's premise) or false?
- Confirm whether the re-armed transceiver produces a NEW `AudioSourceInterface`
  or reuses the old one (`CbAudioLifecycle::AdoptBindings` takes fresh ones;
  whether libwebrtc's voice engine treats them as a new send stream is the
  open question).
- The pulse ADM's `_recStartEvent` is a `webrtc::Event` with no public reset.
  If the send-stream evidence points back at it, the fix is upstream-shaped:
  either a patch in `patches/` (this tree already carries five) or avoiding
  the second `StartRecording` entirely.

## Why it looks like something else

- **The E2E audio spec blames PulseAudio routing or the Opus rtpmap** ("T24
  PulseAudio null-sink regressed?"). The routing is fine — `pactl` shows
  `cb_audio` → loopback → `cb_capture` running, and viewer 1 proves the whole
  path — and the SDP carries Opus every time. The discriminator is **which
  viewer number you are**: restart the worker, run the spec once, it passes;
  run it twice, the second fails.
- **It reads as flaky.** It is deterministic: first session on a process
  passes, every later one fails.

## Verifying a fix

`tests/e2e/05-audio-receives.spec.ts` with `CHROMELESS_E2E_DEVTOOLS_URL` set,
run **twice against the same worker process** — the second run is the test.
The scratch driver used for all three attempts blocks `*/api/viewport` in the
test browser so the resize path cannot confound the result.

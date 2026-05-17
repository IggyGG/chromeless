// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// RED-by-construction test for the M5.5 R1 native AudioDeviceModule —
// see capture/audio/cb_audio_device_module.h (R1 / CV2-28).
//
// This file is M5.5 R2 (CV2-29). It exists to settle the (b)-vs-(a)
// choice empirically: choice (b) is the built-in libwebrtc PulseAudio
// ADM that R1 wires up; choice (a) is a custom libpulse ADM bound
// explicitly to cb_capture.monitor (sketched in the escalation block
// at the bottom of this header).
//
// The iron acceptance criterion (Plane CV2-29):
//
//     tone -> recorded frames at the ADM with RMS above silence
//     threshold within N seconds; test asserts active path (b or a)
//     and emits machine-readable verdict artifact (M0 gate JSON shape).
//
// Test surface:
//   * CloudBrowserAdmTest.SmokeAdmConstructible
//       R1's helper returns a non-null ADM with non-zero recording
//       devices count. Falsifies the most trivial RED — "the built-in
//       Pulse backend isn't even linked" — without depending on a
//       running PulseAudio.
//   * CloudBrowserAdmTest.RecordingPumpRunsOnDefaultSource
//       Init() + InitRecording() + StartRecording() succeed and at
//       least one RecordedDataIsAvailable callback lands within the
//       N-second deadline. Falsifies the next RED — "the ADM linked
//       but the capture pump never fires against the server-default
//       source" — independently of whether the pump captures audible
//       energy.
//   * CloudBrowserAdmTest.MonitorSourceDeliversToneRms
//       The IRON test. The M0 R7 harness pre-emits a known tone into
//       cb_capture.monitor before this test runs; the test asserts
//       buffered-sample RMS exceeds kSilenceFloorRms. RED if the
//       libwebrtc Pulse ADM accepts the server-default but only ever
//       delivers silence (most likely cause: monitor sources rejected
//       by libwebrtc's default-source resolution). RED → escalation
//       to choice (a) per the file-bottom block.
//
// Tone-driving contract — the M0 R7 harness side:
//   The harness script at harness/m5.5/run_red_test.sh (DRAFT in this
//   patch) is responsible for:
//     (1) ensuring PulseAudio is up and cb_capture.monitor is the
//         server-default source (already pinned by
//         infra/pulse-default.pa);
//     (2) starting a paplay-driven tone before invoking this binary —
//         e.g. `paplay --device=cb_capture sine_1khz_10s.wav &`;
//     (3) running this test binary and capturing the M0-shape verdict
//         JSON emitted on stdout (see EmitVerdictJson below);
//     (4) tearing the tone down regardless of test outcome.
//
//   The test ITSELF does not paplay; doing so from inside the test
//   process would couple the assertion (RMS > floor) to the harness's
//   subprocess lifecycle and make RED categorisation harder
//   ("harness never started the tone" looks identical to "ADM never
//   captured the tone"). The harness's responsibility is separation;
//   the test's responsibility is measurement.
//
// Why RED-by-construction (the test author's expectation):
//   The team-lead M5.5 R1 brief says: "if test fails (RED), the path
//   to option (a) is custom libpulse ADM". The plausible RED outcomes
//   in priority order:
//     1. ADM construction returns nullptr — patches/0003 still needs
//        the audio_device_impl widening flagged in R1's TODO.
//     2. ADM constructs but RecordingDevices() == 0 — server-default
//        resolution by libwebrtc's Pulse backend doesn't see
//        cb_capture (PulseAudio source vs sink enumeration policy).
//     3. ADM constructs and pump fires but every recorded buffer is
//        silence — libwebrtc's Pulse ADM is consuming a different
//        source than the M0 harness's paplay tone is feeding (most
//        likely: ADM bound to "default" which resolves under our
//        pinning, but tone is going to cb_capture's sink-not-monitor).
//
//   Each of those RED categories is observable in this test's verdict
//   JSON (path_active + adm_constructed + sample_count + rms). The
//   escalation block at the bottom names which categories specifically
//   demand choice (a).
//
// Cross-references:
//   * capture/audio/cb_audio_device_module.h (R1 / CV2-28) — the helper
//     under test.
//   * capture/audio/cb_audio_test_recorder.{h,cc} (this patch) — the
//     observer half.
//   * harness/m5.5/run_red_test.sh (this patch, DRAFT) — the
//     paplay-driven harness side.
//   * infra/pulse-default.pa — pins cb_capture.monitor as
//     server-default. Untouched per CV2-29 spec.
//   * Plane CV2-29 — this R2 ticket.

#include "capture/audio/cb_audio_device_module.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "api/audio/audio_device.h"
#include "api/scoped_refptr.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "api/task_queue/task_queue_factory.h"
#include "capture/audio/cb_audio_test_recorder.h"
#include "rtc_base/logging.h"
#include "test/gmock.h"
#include "test/gtest.h"

namespace cloud_browser {
namespace {

// The silence floor — RMS below this is treated as "no signal", above
// it as "signal present". Provenance: the 1 kHz sine wave emitted by
// the M0 R7 harness's paplay driver is normalised at -12 dBFS, which
// for 16-bit PCM corresponds to RMS ≈ 0.25 * 32768 ≈ 8192. The floor
// is 1/16th of that nominal level (≈ 512), giving a 24 dB safety
// margin against PulseAudio's mixer attenuating the tone but well
// above the typical noise-floor RMS of an idle Xvfb-on-Pulse rig
// (~10–30 in informal measurement).
//
// TODO(M55-R2-floor-empirical): re-measure the idle floor on the
// build pod once first-green ships and tighten if the 512 number
// proves too conservative or too loose.
constexpr double kSilenceFloorRms = 512.0;

// Acceptance window — how long we let the capture pump run before
// computing RMS. Matches the M0 R7 paplay tone duration (10 s) less
// 1 s of startup slack the ADM needs to drain its priming buffers.
// The Plane spec leaves "N seconds" unspecified; 9 s is the smallest
// window that lets us assert with confidence the tone was running for
// the full measurement.
constexpr int kRecordingWindowMs = 9000;

// Minimum buffered samples before we trust the RMS measurement. Below
// this, ComputeRms() can report misleadingly small values purely from
// a short capture window. Set to 1 s @ 48 kHz mono so even a slow
// PulseAudio init still yields a meaningful read by the time
// kRecordingWindowMs elapses.
constexpr size_t kMinSamplesForRms = 48000;

// Holds the libwebrtc TaskQueueFactory across the test's lifetime —
// matches R1's "process-static factory, leaked intentionally" pattern
// at the PCF construction site. Each test gets its own factory; the
// ADM holds it for the duration of recording.
class ScopedAdmEnvironment {
 public:
  ScopedAdmEnvironment()
      : task_queue_factory_(webrtc::CreateDefaultTaskQueueFactory()) {}
  webrtc::TaskQueueFactory* task_queue_factory() {
    return task_queue_factory_.get();
  }

 private:
  std::unique_ptr<webrtc::TaskQueueFactory> task_queue_factory_;
};

// Emit a verdict JSON line to stdout. Shape matches the M0 R2 gate
// JSON contract — single line, fields:
//   {"module":"m5.5","requirement":"r2",
//    "path_active":"b"|"a"|"none","adm_constructed":bool,
//    "recording_devices":int,"sample_count":int,
//    "last_sample_rate_hz":int,"rms":float,"verdict":"PASS"|"FAIL"|"SKIPPED",
//    "reason":"..."}
//
// Why a hand-rolled JSON emitter rather than a JSON library: the
// gtest binary is linked into a chromium build that exposes
// base::JSONWriter, but reaching for it would couple the M5.5 verdict
// shape to chromium-internal symbol visibility. The fields are fixed
// and few; a hand-roll is the smaller surface.
void EmitVerdictJson(const char* path_active,
                     bool adm_constructed,
                     int recording_devices,
                     size_t sample_count,
                     uint32_t last_sample_rate_hz,
                     double rms,
                     const char* verdict,
                     const std::string& reason) {
  std::fprintf(stdout,
               "M55-R2-VERDICT: "
               "{\"module\":\"m5.5\",\"requirement\":\"r2\","
               "\"path_active\":\"%s\","
               "\"adm_constructed\":%s,"
               "\"recording_devices\":%d,"
               "\"sample_count\":%zu,"
               "\"last_sample_rate_hz\":%u,"
               "\"rms\":%.2f,"
               "\"verdict\":\"%s\","
               "\"reason\":\"%s\"}\n",
               path_active,
               adm_constructed ? "true" : "false",
               recording_devices,
               sample_count,
               last_sample_rate_hz,
               rms,
               verdict,
               reason.c_str());
  std::fflush(stdout);
}

// --- Test 1: smoke — ADM is constructible ---------------------------

TEST(CloudBrowserAdmTest, SmokeAdmConstructible) {
  ScopedAdmEnvironment env;
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm =
      CreateCloudBrowserNativeAudioDeviceModule(env.task_queue_factory());

  // RED expectations:
  //   * nullptr → R1's "PulseAudio not up / kPlatformDefaultAudio
  //     rejected" path fired. Probable cause: patches/0003 still
  //     needs the audio_device_impl widening flagged in R1's
  //     header TODO. Escalate per file-bottom block.
  if (adm == nullptr) {
    EmitVerdictJson("none", false, 0, 0, 0, 0.0, "FAIL",
                    "CreateCloudBrowserNativeAudioDeviceModule returned "
                    "nullptr — likely patches/0003 audio_device_impl "
                    "widening needed (see R1 TODO)");
    FAIL() << "ADM construction returned nullptr";
    return;
  }

  // RecordingDevices() — number of capture devices the ADM enumerates.
  // For the built-in Pulse backend on a working container with
  // pulse-default.pa loaded, expect ≥ 1 (the server-default).
  const int16_t devices = adm->RecordingDevices();
  if (devices < 1) {
    EmitVerdictJson("b", true, devices, 0, 0, 0.0, "FAIL",
                    "ADM constructed but RecordingDevices() < 1 — "
                    "server-default not visible to libwebrtc Pulse "
                    "backend; escalate to choice (a)");
    FAIL() << "RecordingDevices() = " << devices << ", expected ≥ 1";
    return;
  }

  EmitVerdictJson("b", true, devices, 0, 0, 0.0, "PASS",
                  "ADM constructed and enumerates ≥ 1 recording device");
}

// --- Test 2: pump runs --------------------------------------------

TEST(CloudBrowserAdmTest, RecordingPumpRunsOnDefaultSource) {
  ScopedAdmEnvironment env;
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm =
      CreateCloudBrowserNativeAudioDeviceModule(env.task_queue_factory());
  ASSERT_NE(adm, nullptr)
      << "ADM construction failed — see SmokeAdmConstructible";

  test::CbAudioTestRecorder recorder;

  // libwebrtc ADM init protocol:
  //   Init() → register AudioTransport → InitRecording() → StartRecording()
  //
  // The Init/Register/InitRecording/StartRecording ordering matters —
  // RegisterAudioCallback before Init is undefined; some backends
  // tolerate it, others crash. The order below is the one the M1 PCF
  // test fixture's TestAudioDeviceModule callers use and matches the
  // libwebrtc Linux Pulse ADM's documented expectations.
  ASSERT_EQ(adm->Init(), 0);
  ASSERT_EQ(adm->RegisterAudioCallback(&recorder), 0);

  // No SetRecordingDevice — the M5.5 R1 contract is that the server-
  // default IS the recording device. Setting it explicitly here would
  // both moot the test (we'd be testing the explicit-binding path
  // we're trying to falsify) and contradict R1's intentional
  // omission.
  ASSERT_EQ(adm->InitRecording(), 0);
  ASSERT_EQ(adm->StartRecording(), 0);

  // Pump-runs check: at least kMinSamplesForRms (1 s @ 48 kHz) within
  // the recording window. A shorter wait would risk a false RED on a
  // slow PulseAudio init; the M0 R7 harness allows up to 5 s of
  // pre-test priming, so 9 s of in-test window is generous.
  const bool got_samples =
      recorder.WaitForSamples(kMinSamplesForRms, kRecordingWindowMs);

  // Stop+release before fixture tear-down so the capture pump quiesces
  // while AudioTransport is still alive (libwebrtc may dispatch one
  // last buffer mid-Stop; tearing the transport down first would race
  // and CHECK).
  ASSERT_EQ(adm->StopRecording(), 0);

  const size_t buffered = recorder.BufferedSampleCount();
  const size_t callbacks = recorder.RecordedCallCount();
  const uint32_t rate = recorder.LastSampleRateHz();

  if (!got_samples) {
    EmitVerdictJson("b", true, adm->RecordingDevices(), buffered, rate, 0.0,
                    "FAIL",
                    "InitRecording+StartRecording succeeded but capture "
                    "pump delivered < kMinSamplesForRms within deadline; "
                    "callbacks=" + std::to_string(callbacks));
    FAIL() << "capture pump produced " << buffered << " samples in "
           << kRecordingWindowMs << " ms (expected ≥ " << kMinSamplesForRms
           << "); callbacks=" << callbacks;
    return;
  }

  EmitVerdictJson("b", true, adm->RecordingDevices(), buffered, rate, 0.0,
                  "PASS",
                  "capture pump delivered ≥ kMinSamplesForRms within "
                  "deadline; callbacks=" + std::to_string(callbacks));
}

// --- Test 3: iron — tone in, RMS out ------------------------------

TEST(CloudBrowserAdmTest, MonitorSourceDeliversToneRms) {
  // Honour an opt-in env so an operator can run the smoke + pump
  // tests on a workstation without paplay set up, and have THIS test
  // SKIP rather than spuriously fail. The M0 R7 harness sets
  // CB_M55_R2_TONE_RUNNING=1 immediately before invoking the test
  // binary; a missing env var means "no tone configured" → SKIPPED,
  // not RED.
  //
  // SKIPPED is correctly distinguished from FAIL in the verdict
  // emitter (see EmitVerdictJson), so the M0 gate scaffold's
  // CV2-12 (R2-fix: SKIPPED → non-blocking) treats it the same way
  // it treats any other skipped assertion.
  const char* tone_env = std::getenv("CB_M55_R2_TONE_RUNNING");
  if (tone_env == nullptr || std::string(tone_env) != "1") {
    EmitVerdictJson("b", false, 0, 0, 0, 0.0, "SKIPPED",
                    "CB_M55_R2_TONE_RUNNING not set — harness did not "
                    "drive a tone; skipping iron test");
    GTEST_SKIP() << "CB_M55_R2_TONE_RUNNING not set (harness contract)";
    return;
  }

  ScopedAdmEnvironment env;
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm =
      CreateCloudBrowserNativeAudioDeviceModule(env.task_queue_factory());
  ASSERT_NE(adm, nullptr);

  test::CbAudioTestRecorder recorder;
  ASSERT_EQ(adm->Init(), 0);
  ASSERT_EQ(adm->RegisterAudioCallback(&recorder), 0);
  ASSERT_EQ(adm->InitRecording(), 0);
  ASSERT_EQ(adm->StartRecording(), 0);

  // Wait for the full window — we want the RMS averaged over enough
  // samples that an isolated capture-pump glitch (one bad 10ms chunk)
  // can't dominate the measurement.
  const bool got_samples =
      recorder.WaitForSamples(kMinSamplesForRms * 4, kRecordingWindowMs);

  ASSERT_EQ(adm->StopRecording(), 0);

  const size_t buffered = recorder.BufferedSampleCount();
  const uint32_t rate = recorder.LastSampleRateHz();
  const double rms = recorder.ComputeRms();

  if (!got_samples) {
    EmitVerdictJson("b", true, adm->RecordingDevices(), buffered, rate, rms,
                    "FAIL",
                    "tone was driving but capture pump produced too few "
                    "samples — escalate to choice (a) custom libpulse ADM");
    FAIL() << "tone was driving but only " << buffered << " samples in "
           << kRecordingWindowMs << " ms";
    return;
  }

  if (rms < kSilenceFloorRms) {
    EmitVerdictJson("b", true, adm->RecordingDevices(), buffered, rate, rms,
                    "FAIL",
                    "tone was driving and capture pump ran, but RMS " +
                        std::to_string(rms) + " < kSilenceFloorRms " +
                        std::to_string(kSilenceFloorRms) +
                        " — libwebrtc Pulse ADM is consuming a "
                        "different source than the harness's tone; "
                        "escalate to choice (a) custom libpulse ADM "
                        "bound explicitly to cb_capture.monitor");
    FAIL() << "RMS " << rms << " < kSilenceFloorRms " << kSilenceFloorRms;
    return;
  }

  EmitVerdictJson("b", true, adm->RecordingDevices(), buffered, rate, rms,
                  "PASS",
                  "tone delivered with RMS " + std::to_string(rms) +
                      " > kSilenceFloorRms " +
                      std::to_string(kSilenceFloorRms) +
                      " — choice (b) ratified, no escalation needed");
}

// =====================================================================
// Escalation block — what choice (a) looks like, and when we take it.
// =====================================================================
//
// The Plane CV2-29 spec ("on failure (monitor rejected by libwebrtc
// Pulse ADM): implement (a) capture/audio/cb_pulse_adm.{h,cc}") names
// the file path but not the implementation strategy. The strategy
// implied by the surrounding R1 work + M0 R7 harness shape is:
//
//   * cb_pulse_adm.h declares a CreateCloudBrowserPulseAudioDeviceModule
//     factory with the same signature as R1's
//     CreateCloudBrowserNativeAudioDeviceModule, so the M5.5 wire-up
//     swap is exactly one line in cloud_browser_pcf.cc.
//
//   * cb_pulse_adm.cc implements webrtc::AudioDeviceModule directly,
//     opening libpulse with pa_stream_new_with_proplist + pa_stream_
//     connect_record(stream, "cb_capture.monitor", ...) — the
//     EXPLICIT source-name binding the spec calls for. pulse-default.pa
//     remains untouched (we just stop relying on the server-default
//     resolution path).
//
//   * libpulse linkage: chromium ships libpulse symbols dynamically
//     dlopen'd through //media/audio/pulse for its OWN audio path;
//     we cannot piggy-back on those (visibility restricted). The
//     escalation requires either:
//       (a1) a chromium-side patch to expose //media/audio/pulse's
//            libpulse symbol-loader to out-of-tree consumers; OR
//       (a2) a fresh dynamic-loader local to capture/audio that
//            dlsym's libpulse-simple's API itself.
//     (a2) is the lower-blast-radius choice for an escalation; the
//     patches/0001 surface is already crowded.
//
//   * AudioTransport hookup is identical to choice (b) — the test
//     fixture above swaps factory function names and re-runs.
//
// Which RED outcomes specifically demand the (a) escalation:
//
//   ┌────────────────────────────────────────────┬─────────────────────┐
//   │ Symptom                                    │ Action              │
//   ├────────────────────────────────────────────┼─────────────────────┤
//   │ SmokeAdmConstructible: ADM nullptr         │ widen patches/0003  │
//   │   (audio_device_impl not linked)           │ FIRST; (a) only if  │
//   │                                            │ widening doesn't    │
//   │                                            │ make it linkable    │
//   ├────────────────────────────────────────────┼─────────────────────┤
//   │ SmokeAdmConstructible: RecordingDevices=0  │ ESCALATE TO (a)     │
//   │   (libwebrtc Pulse rejects server-default) │                     │
//   ├────────────────────────────────────────────┼─────────────────────┤
//   │ RecordingPumpRunsOnDefaultSource: no       │ ESCALATE TO (a)     │
//   │   samples within deadline                  │                     │
//   ├────────────────────────────────────────────┼─────────────────────┤
//   │ MonitorSourceDeliversToneRms: RMS < floor  │ ESCALATE TO (a) —   │
//   │                                            │ this is the         │
//   │                                            │ load-bearing RED    │
//   │                                            │ the spec singles    │
//   │                                            │ out                 │
//   └────────────────────────────────────────────┴─────────────────────┘
//
// The DRAFT does NOT write cb_pulse_adm.{h,cc} preemptively — per the
// spec, (a) is contingent on RED from this test. Writing the skeleton
// in this same patch would land code that has no in-tree caller until
// a later commit, which the gn-check pre-validation in M0 won't
// tolerate.

}  // namespace
}  // namespace cloud_browser

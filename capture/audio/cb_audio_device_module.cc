// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cb_audio_device_module.cc — see cb_audio_device_module.h.
//
// CV2-28 R1: choice b — built-in libwebrtc PulseAudio ADM.

#include "capture/audio/cb_audio_device_module.h"

#include "api/audio/audio_device.h"
#include "api/audio/create_audio_device_module.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/scoped_refptr.h"
#include "api/task_queue/task_queue_factory.h"
#include "rtc_base/logging.h"

namespace cloud_browser {

webrtc::scoped_refptr<webrtc::AudioDeviceModule>
CreateCloudBrowserNativeAudioDeviceModule(
    webrtc::TaskQueueFactory* task_queue_factory) {
  if (task_queue_factory == nullptr) {
    // The PCF construction site owns a process-static factory; a null
    // pointer here means a programmer error, not a recoverable runtime
    // condition. Log + return nullptr so the PCF wrapper can fall back
    // to the dummy ADM and the worker still boots — the alternative
    // (crash) would make the M5.5 landing more dangerous than its
    // worst-case behaviour deserves.
    RTC_LOG(LS_ERROR) << "CloudBrowser: ADM construction skipped — "
                         "task_queue_factory is null";
    return nullptr;
  }

  // build-czar iter 6 (2026-05-17): chromium-bundled libwebrtc removed
  // the static webrtc::AudioDeviceModule::Create(audio_layer,
  // task_queue_factory*) factory and replaced it with the free function
  // webrtc::CreateAudioDeviceModule(env, audio_layer), taking a
  // webrtc::Environment instead of a raw TaskQueueFactory pointer. The
  // Environment owns the TaskQueueFactory + clock + field-trial + RTC
  // event log under one umbrella, constructed via
  // webrtc::CreateEnvironment().
  //
  // patches/0003-add-cloud-browser-webrtc-overrides.patch line 145
  // re-exports //third_party/webrtc/api/audio:create_audio_device_module
  // through webrtc_api_passthrough's public_deps, and lines for
  // api/environment:environment + environment_factory provide the
  // Environment type + CreateEnvironment factory. So this call site
  // resolves cleanly through the cloud-browser passthrough; no patch
  // widening needed.
  //
  // task_queue_factory parameter retained for ABI compat with M1 call
  // sites (cloud_browser_browser_main_parts.cc + test fixtures); the
  // Environment subsumes its role. Marked unused via (void) to keep
  // -Werror happy.
  (void)task_queue_factory;
  webrtc::Environment env = webrtc::CreateEnvironment();
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm =
      webrtc::CreateAudioDeviceModule(
          env, webrtc::AudioDeviceModule::kPlatformDefaultAudio);

  if (adm == nullptr) {
    // Most likely cause on this container: the PulseAudio server isn't
    // up yet (init ordering) or the cb_capture.monitor source isn't
    // registered. Treat as a soft failure — the PCF wrapper falls back
    // to its own kDummyAudio path so the worker keeps booting and the
    // rest of the pipeline (video, datachannel) still comes up.
    RTC_LOG(LS_ERROR) << "CloudBrowser: CreateAudioDeviceModule("
                         "kPlatformDefaultAudio) returned nullptr — "
                         "is PulseAudio up? (see infra/pulse-default.pa)";
    return nullptr;
  }

  // CV2-75 Ring N+2 fix-forward — defensive Init() before promoting
  // the ADM to the caller. Background: AudioDeviceLinuxPulse's ctor
  // does NOT connect to the PulseAudio server (that's deferred to
  // Init()). If the server is unreachable (test pod without a Pulse
  // sidecar, init-order race, etc.), ctor succeeds + returns non-null,
  // and the failure surfaces later when libwebrtc's media-engine
  // initialization calls adm_helpers (third_party/webrtc/media/engine/
  // adm_helpers.cc:39), which CHECK-fails the worker process FATAL
  // when Init returns non-zero. Empirically observed on rv4 against
  // the test pod (which doesn't run a Pulse server):
  //
  //   ERROR: audio_device_pulse_linux.cc:1621 failed to connect context
  //                                            error=-1
  //   ERROR: audio_device_pulse_linux.cc:161  failed to initialize Pulse
  //   FATAL: adm_helpers.cc:39                (Init CHECK failure)
  //
  // The fix: call adm->Init() here (we're on the worker thread per
  // the fa96f1c marshal, so the AudioDeviceLinuxPulse SequenceChecker
  // is satisfied). On failure, return nullptr so the caller's
  // dummy-ADM fallback (cloud_browser_pcf.cc:CreateCloudBrowserDefault
  // AudioDeviceModule line 138-160) engages naturally. The worker
  // boots with no audio capture but with PCF + datachannels + video
  // intact — same shape as the rv1/rv2 dummy-ADM path that was the
  // baseline before M5.5 R1 landed.
  //
  // Production cb-chromium in environments WITH a Pulse server: Init
  // succeeds, this branch is a single boolean test, fall through to
  // the INFO log + return adm. Cost is one extra Init call (compared
  // to letting libwebrtc do it later), which is fine because Init is
  // idempotent (re-Init returns InitStatus::OK without reconnecting).
  //
  // M5.5 R1-R5 (real audio pipeline end-to-end) is deferred to a
  // future sub-campaign (CV2-82 per team-lead) that provides a
  // Pulse-equipped test environment. CV2-75's M5.5 R0 acceptance
  // shape evolves from "Pulse-pipeline-up" to "defensive-fallback-
  // graceful + no FATAL" — both rings (3 + N+1 + N+2) preserved.
  if (adm->Init() != 0) {
    RTC_LOG(LS_WARNING) << "CloudBrowser: native AudioDeviceModule Init "
                           "failed (PulseAudio server unreachable?); "
                           "falling back to kDummyAudio (no-audio path)";
    return nullptr;
  }

  RTC_LOG(LS_INFO) << "CloudBrowser: native AudioDeviceModule "
                      "constructed AND initialized (kPlatformDefault"
                      "Audio / built-in PulseAudio); recording device "
                      "resolves to PulseAudio server-default source";
  return adm;
}

}  // namespace cloud_browser

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cb_audio_device_module.cc — see cb_audio_device_module.h.
//
// CV2-28 R1: choice b — built-in libwebrtc PulseAudio ADM.

#include "capture/audio/cb_audio_device_module.h"

#include "api/audio/audio_device.h"
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

  // Construct the platform-default ADM. On Linux this resolves to
  // libwebrtc's built-in PulseAudio backend, which reads from the
  // server-default source — pinned to cb_capture.monitor by
  // infra/pulse-default.pa. NO SetRecordingDevice on purpose: the
  // server-default IS the contract, and routing it through a
  // libpulse-level handle inside the worker would re-introduce the
  // exact coupling the M5.5 "choice b" decision is meant to avoid.
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm =
      webrtc::AudioDeviceModule::Create(
          webrtc::AudioDeviceModule::kPlatformDefaultAudio,
          task_queue_factory);

  if (adm == nullptr) {
    // Most likely cause on this container: the PulseAudio server isn't
    // up yet (init ordering) or the cb_capture.monitor source isn't
    // registered. Treat as a soft failure — the PCF wrapper falls back
    // to the dummy ADM so the worker keeps booting and the rest of
    // the pipeline (video, datachannel) still comes up.
    //
    // TODO(M5.5-R1-init-ordering): once the container-boot harness in
    // M0-R7 is stable, decide whether the worker should hard-fail
    // instead. For DRAFT we prefer best-effort so a misconfigured
    // PulseAudio doesn't block video-track validation in M2's
    // acceptance gate.
    RTC_LOG(LS_ERROR) << "CloudBrowser: AudioDeviceModule::Create("
                         "kPlatformDefaultAudio) returned nullptr — "
                         "is PulseAudio up? (see infra/pulse-default.pa)";
    return nullptr;
  }

  RTC_LOG(LS_INFO) << "CloudBrowser: native AudioDeviceModule "
                      "constructed (kPlatformDefaultAudio / built-in "
                      "PulseAudio); recording device resolves to "
                      "PulseAudio server-default source";
  return adm;
}

}  // namespace cloud_browser

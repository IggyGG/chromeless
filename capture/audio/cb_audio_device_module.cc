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

  // build-czar iter 5 (2026-05-17): M5.5 R1's
  // webrtc::AudioDeviceModule::Create(audio_layer, task_queue_factory*)
  // was removed in the chromium-bundled libwebrtc — replaced by the
  // free function webrtc::CreateAudioDeviceModule(env, audio_layer),
  // which takes a webrtc::Environment instead of a raw TaskQueueFactory
  // pointer. See third_party/webrtc/api/audio/create_audio_device_module.h
  // and cloud_browser_pcf.cc:113-119 for the API-drift note.
  //
  // For M-stack validation we DEFER native ADM construction entirely:
  // return nullptr unconditionally so the PCF caller propagates
  // deps.adm = nullptr and libwebrtc bypasses audio device init. This
  // is strictly an audio-disabled CV2 acceptance — video + DataChannel
  // + cursor + input all stay functional through M4/M5/M6. M5.5 audio
  // polish (Environment + CreateAudioDeviceModule wiring, OR migrating
  // to the passthrough-exported environment_factory) is tracked in a
  // follow-up IMPROVE ticket and re-enables native PulseAudio audio
  // without re-walking the gn-visibility + iter-4 nullptr-fallback
  // cycle on the same M-stack tip.
  //
  // task_queue_factory parameter retained for ABI compat with the M1
  // call sites in cloud_browser_browser_main_parts.cc and the test
  // fixtures; ignored in this iter.
  (void)task_queue_factory;
  RTC_LOG(LS_WARNING) << "CloudBrowser: native ADM deferred to IMPROVE "
                         "ticket — returning nullptr (audio disabled "
                         "for M-stack validation cycle)";
  return nullptr;
}

}  // namespace cloud_browser

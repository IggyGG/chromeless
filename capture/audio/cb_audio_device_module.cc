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

  RTC_LOG(LS_INFO) << "CloudBrowser: native AudioDeviceModule "
                      "constructed (kPlatformDefaultAudio / built-in "
                      "PulseAudio); recording device resolves to "
                      "PulseAudio server-default source";
  return adm;
}

}  // namespace cloud_browser

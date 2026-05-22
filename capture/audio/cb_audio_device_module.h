// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cb_audio_device_module — Module M5.5 of the ChromelessV2 native-peer
// migration (parent CV2-8; this R1 is CV2-28).
//
// CV2-28 ratifies "choice b": construct a real webrtc::AudioDeviceModule
// via libwebrtc's built-in PulseAudio backend rather than rolling a
// custom libpulse ADM. The constructor returned by this header is a
// thin wrapper around
//
//     webrtc::AudioDeviceModule::Create(
//         webrtc::AudioDeviceModule::kPlatformDefaultAudio,
//         task_queue_factory);
//
// On the container-boot image that hosts the cloud-browser worker the
// platform-default backend resolves to PulseAudio, which in turn
// resolves the recording device to the server-default source pinned
// by `infra/pulse-default.pa` —
//
//     set-default-source cb_capture.monitor
//
// — so this ADM transparently captures the same monitor source that
// today's getDisplayMedia({audio:true}) browser flow consumes. There
// is intentionally NO SetRecordingDevice call in M5.5; the server-
// default source IS the contract, and the M5.5 ADM honors it without
// pinning to a libpulse-level handle inside the worker.
//
// Why a separate file (not just an inline body in cloud_browser_pcf.cc):
//   - cb_audio_device_module is the M5.5 cross-module hand-off seam.
//     Future M5.5 revisions (R2 escalation to custom libpulse ADM if
//     the built-in path proves insufficient) replace ONLY this file's
//     body; cloud_browser_pcf.cc and cloud_browser_browser_main_parts
//     stay stable.
//   - The PulseAudio backend lives under //third_party/webrtc/modules/
//     audio_device:audio_device_impl. Keeping the audio-impl deps
//     localized to this source_set keeps the PCF source_set audio-
//     agnostic (mirrors how the encoder-factory deps stayed localized
//     in capture/encoder/ rather than smeared into build-integration).
//
// Cross-references:
//   * capture/build-integration/cloud_browser_pcf.{h,cc}
//     M5.5 plugs in via CreateCloudBrowserDefaultAudioDeviceModule(),
//     which now forwards to CreateCloudBrowserNativeAudioDeviceModule()
//     in this file. The function name on the PCF side stays the same
//     so every M1 call site (cloud_browser_browser_main_parts.cc) is
//     unchanged.
//   * infra/pulse-default.pa
//     Pins set-default-source cb_capture.monitor. The contract this
//     ADM rides on.
//   * patches/0003-add-cloud-browser-webrtc-overrides.patch
//     The webrtc-overrides passthrough. TODO(M5.5-R1-passthrough):
//     this draft assumes audio_device_impl is reachable transitively
//     through api:libjingle_peerconnection_api (the dummy-ADM kDummy
//     path works on integration/native-peer today, suggesting the
//     symbol is already linked). If gn check / autoninja rejects the
//     new cc file because AudioDeviceModule::Create can't resolve the
//     PulseAudio backend symbols, patches/0003 must be widened to
//     re-export //third_party/webrtc/modules/audio_device:
//     audio_device_impl. This is the cross-cut with gn-investigator's
//     prior patches/0003 work — please coordinate before widening.
//   * Plane CV2-28 (the R1 ticket itself).

#ifndef CAPTURE_AUDIO_CB_AUDIO_DEVICE_MODULE_H_
#define CAPTURE_AUDIO_CB_AUDIO_DEVICE_MODULE_H_

#include "api/audio/audio_device.h"
#include "api/scoped_refptr.h"
#include "api/task_queue/task_queue_factory.h"

namespace cloud_browser {

// Construct a real libwebrtc Linux AudioDeviceModule using the built-in
// PulseAudio backend (kPlatformDefaultAudio on Linux).
//
// |task_queue_factory| — the TaskQueueFactory the ADM uses for its
//   internal task queues (capture pump, processing pump, etc.). MUST
//   outlive the returned ADM. The PCF construction site in
//   cloud_browser_pcf.cc owns a process-static factory via
//   CreateDefaultTaskQueueFactory() and intentionally leaks it; the
//   same pointer is threaded through to this constructor.
//
// Returns:
//   * On success: a non-null AudioDeviceModule. Caller is expected to
//     hand it directly to PeerConnectionFactoryDependencies::adm and
//     let PCF construction drive Init() / InitRecording() on the PCF
//     worker thread.
//   * On failure: nullptr. The PCF wrapper interprets this as "fall
//     back to the dummy ADM" so the browser-process worker still
//     boots when PulseAudio is unavailable (e.g. when the container
//     init didn't bring up the cb_capture.monitor source yet).
//
// Threading:
//   * The Create call itself is cheap and may be invoked from any
//     thread. libwebrtc moves the ADM's heavy lifting (Init,
//     InitRecording, StartRecording) onto its own task queues drawn
//     from |task_queue_factory|.
//   * No SetRecordingDevice is called — the PulseAudio server-default
//     source (cb_capture.monitor per infra/pulse-default.pa) IS the
//     contract.
webrtc::scoped_refptr<webrtc::AudioDeviceModule>
CreateCloudBrowserNativeAudioDeviceModule(
    webrtc::TaskQueueFactory* task_queue_factory);

}  // namespace cloud_browser

#endif  // CAPTURE_AUDIO_CB_AUDIO_DEVICE_MODULE_H_

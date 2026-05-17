// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cloud_browser_pcf.cc — see cloud_browser_pcf.h.

#include "capture/build-integration/cloud_browser_pcf.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "api/audio/audio_device.h"
#include "api/audio/create_audio_device_module.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/enable_media.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/peer_connection_interface.h"
#include "api/rtp_parameters.h"
#include "api/scoped_refptr.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "api/task_queue/task_queue_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "capture/audio/cb_audio_device_module.h"
#include "capture/encoder/encoder_factory.h"
#include "rtc_base/logging.h"
#include "rtc_base/thread.h"

namespace cloud_browser {

webrtc::PeerConnectionFactoryDependencies BuildCloudBrowserPcfDependencies(
    webrtc::Thread* network_thread,
    webrtc::Thread* worker_thread,
    webrtc::Thread* signaling_thread,
    const webrtc::Environment& env,
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm) {
  webrtc::PeerConnectionFactoryDependencies deps;

  // Three dedicated webrtc::Threads — owned by the caller. The CV2-26
  // R-thread DECISION rationale: simpler lifetime ordering for the
  // mandated PCF-teardown-before-browser_context_ ordering vs.
  // ThreadWrapper around chromium task runners.
  deps.network_thread = network_thread;
  deps.worker_thread = worker_thread;
  deps.signaling_thread = signaling_thread;

  // Environment carries the clock + task-queue factory + field-trial
  // config. Stored by value (PeerConnectionFactoryDependencies takes
  // a const& at construction, owns its own copy thereafter).
  deps.env = env;

  // Video encoder factory — the CloudBrowserVideoEncoderFactory under
  // default Config{} (VP9 + H264 + AV1 enabled, VP8 disabled). The
  // SAME factory the renderer-side install used to consume; in M1 it
  // moves here. The renderer-side install site is deleted in M7.
  deps.video_encoder_factory = std::make_unique<CloudBrowserVideoEncoderFactory>(
      CloudBrowserVideoEncoderFactory::Config{});

  // Video decoder factory — libwebrtc builtin default. We don't decode
  // outgoing video in the browser-process worker; the slot still has
  // to be populated for libwebrtc to wire up the media engine.
  deps.video_decoder_factory = webrtc::CreateBuiltinVideoDecoderFactory();

  // Audio enc/dec factories — libwebrtc builtins. Whether any audio
  // actually flows is controlled by the ADM slot below.
  deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
  deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();

  // Audio device module — the M5.5 cross-module injection slot. When
  // the caller passes nullptr we substitute the M1 default (dummy /
  // no-audio). When the caller passes a real ADM we honor it directly
  // — this is the substitution path M5.5 uses.
  if (adm) {
    deps.adm = std::move(adm);
  } else {
    deps.adm = CreateCloudBrowserDefaultAudioDeviceModule();
  }

  return deps;
}

webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
CreateCloudBrowserPcf(
    webrtc::Thread* network_thread,
    webrtc::Thread* worker_thread,
    webrtc::Thread* signaling_thread,
    const webrtc::Environment& env,
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm) {
  webrtc::PeerConnectionFactoryDependencies deps =
      BuildCloudBrowserPcfDependencies(network_thread, worker_thread,
                                       signaling_thread, env,
                                       std::move(adm));

  // EnableMedia populates the media engine slots (video / audio
  // encoders + decoders + ADM) from the deps above. The result is a
  // self-consistent deps struct ready to construct a PCF.
  webrtc::EnableMedia(deps);

  return webrtc::CreateModularPeerConnectionFactory(std::move(deps));
}

webrtc::scoped_refptr<webrtc::AudioDeviceModule>
CreateCloudBrowserDefaultAudioDeviceModule() {
  // M5.5-R1 (CV2-28): the "default" ADM is now the real libwebrtc
  // built-in PulseAudio backend (kPlatformDefaultAudio), constructed
  // by capture/audio/cb_audio_device_module.cc. The function name is
  // kept stable so every M1 call site
  // (cloud_browser_browser_main_parts.cc, the test fixtures, and the
  // BuildCloudBrowserPcfDependencies fallback above) is unchanged.
  //
  // chromium-bundled libwebrtc replaced the old
  // `AudioDeviceModule::Create(audio_layer, task_queue_factory*)`
  // static factory with the free function
  // `webrtc::CreateAudioDeviceModule(env, audio_layer)` — env is now
  // the only dependency container (Environment owns the
  // TaskQueueFactory + clock + field-trial + RTC event log under one
  // umbrella). See third_party/webrtc/api/audio/create_audio_device_module.h.
  //
  // The Environment is constructed once per process and intentionally
  // leaked, mirroring the old leaky-TaskQueueFactory shape. The ADM
  // is owned by the PCF, which is owned by CloudBrowserBrowserMainParts
  // and torn down before the process exits — the leak is bounded by
  // process lifetime.
  //
  // Fallback: if cb-audio's native ADM construction fails (PulseAudio
  // server not up yet, cb_capture.monitor not registered, etc.), fall
  // back to the kDummyAudio path so the browser-process worker still
  // boots — video + datachannel come up regardless and the M2/M3/M4
  // pipeline keeps its M0 R5/R6 acceptance gates green. The error is
  // logged inside CreateCloudBrowserNativeAudioDeviceModule.
  static webrtc::TaskQueueFactory* const task_queue_factory =
      webrtc::CreateDefaultTaskQueueFactory().release();
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm =
      CreateCloudBrowserNativeAudioDeviceModule(task_queue_factory);
  if (adm != nullptr) {
    return adm;
  }
  // build-czar iter 4 (2026-05-17): the chromium-bundled libwebrtc has
  // removed the static webrtc::AudioDeviceModule::Create(kDummyAudio,
  // task_queue_factory*) factory that this fallback used to call. The
  // dummy-ADM no-audio path is no longer reachable via a one-liner;
  // returning nullptr is the chromium-canonical "skip audio entirely"
  // signal — PCF caller sets deps.adm = nullptr, libwebrtc then bypasses
  // audio device init. M5.5 R1's CreateCloudBrowserNativeAudioDeviceModule
  // is the only supported audio path in CV2; a hard nullptr here is
  // strictly better than a silent dummy that masks native-ADM failures.
  RTC_LOG(LS_WARNING) << "CloudBrowser: native ADM unavailable — "
                         "no-audio path (deps.adm = nullptr)";
  return nullptr;
}

std::string FormatPcfVideoCodecLogLine(
    const std::vector<webrtc::RtpCodecCapability>& video_send_codecs) {
  // Format contract (LOAD-BEARING — M0 R5 scrape regex):
  //   "CloudBrowser: PCF video sender codecs = [VP9,H264,AV1]"
  //
  // - prefix verbatim
  // - bracket open, comma-no-space separator, bracket close
  // - stable input order (no sort)
  // - empty list renders as "[]"
  std::string out = "CloudBrowser: PCF video sender codecs = [";
  bool first = true;
  for (const auto& codec : video_send_codecs) {
    if (!first) {
      out += ",";
    }
    out += codec.name;
    first = false;
  }
  out += "]";
  return out;
}

}  // namespace cloud_browser

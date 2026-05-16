// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cloud_browser_pcf.cc — see cloud_browser_pcf.h.

#include "capture/build-integration/cloud_browser_pcf.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "api/audio/audio_device.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/enable_media.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/peer_connection_interface.h"
#include "api/rtp_parameters.h"
#include "api/scoped_refptr.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "api/task_queue/task_queue_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "capture/encoder/encoder_factory.h"
#include "rtc_base/thread.h"

namespace cloud_browser {

webrtc::PeerConnectionFactoryDependencies BuildCloudBrowserPcfDependencies(
    rtc::Thread* network_thread,
    rtc::Thread* worker_thread,
    rtc::Thread* signaling_thread,
    const webrtc::Environment& env,
    rtc::scoped_refptr<webrtc::AudioDeviceModule> adm) {
  webrtc::PeerConnectionFactoryDependencies deps;

  // Three dedicated rtc::Threads — owned by the caller. The CV2-26
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

rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
CreateCloudBrowserPcf(
    rtc::Thread* network_thread,
    rtc::Thread* worker_thread,
    rtc::Thread* signaling_thread,
    const webrtc::Environment& env,
    rtc::scoped_refptr<webrtc::AudioDeviceModule> adm) {
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

rtc::scoped_refptr<webrtc::AudioDeviceModule>
CreateCloudBrowserDefaultAudioDeviceModule() {
  // M1's default ADM is the kDummyAudio path — satisfies libwebrtc's
  // non-null ADM expectation but emits no real audio frames.
  //
  // The TaskQueueFactory is created once per process and intentionally
  // leaked. AudioDeviceModule::Create takes a bare TaskQueueFactory*
  // and holds it across the ADM's lifetime; the ADM is owned by the
  // PCF, which is owned by CloudBrowserBrowserMainParts and torn down
  // before the process exits. A static/leaky factory is the simplest
  // shape that satisfies the lifetime requirement without paying the
  // cost of an extra owning member on every embedder seam.
  //
  // TODO(M5.5): replace this body with the real cb-audio ADM
  // construction. The signature stays the same; every M1 call site
  // (cloud_browser_browser_main_parts.cc) is audio-agnostic.
  static webrtc::TaskQueueFactory* const task_queue_factory =
      webrtc::CreateDefaultTaskQueueFactory().release();
  return webrtc::AudioDeviceModule::Create(
      webrtc::AudioDeviceModule::kDummyAudio, task_queue_factory);
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

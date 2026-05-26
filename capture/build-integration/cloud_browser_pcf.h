// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Browser-process PeerConnectionFactory (PCF) seam — extracted from
// CloudBrowserBrowserMainParts so the assembly, construction, and
// codec-line log formatting can be exercised by unit tests that don't
// need a full chromium browser-process boot.
//
// Module M1 of the ChromelessV2 native-peer migration. M1's iron
// acceptance test (CV2-26): after browser-process bootstrap, the PCF
// is non-null and its video-sender RTP capabilities match the codec
// set produced by CloudBrowserVideoEncoderFactory under the default
// Config{} — VP9 + H264 + AV1 (the M1 Finding-A-ratified set).
//
// The seam has four pieces:
//
//   1. BuildCloudBrowserPcfDependencies(...)
//      Pure assembly. 3 dedicated webrtc::Threads (network / worker /
//      signaling — see CV2-26 R-thread DECISION) + a webrtc::
//      Environment + an AudioDeviceModule are slotted into a
//      webrtc::PeerConnectionFactoryDependencies struct alongside our
//      injected CloudBrowserVideoEncoderFactory (default Config{}),
//      the libwebrtc builtin video decoder factory, and the libwebrtc
//      builtin audio encoder + decoder factories. Slot-by-slot
//      testable; no PCF object construction.
//
//   2. CreateCloudBrowserPcf(...)
//      Thin wrapper = BuildCloudBrowserPcfDependencies + EnableMedia +
//      CreateModularPeerConnectionFactory. Returns the constructed
//      PCF (or nullptr on failure). This is what
//      CloudBrowserBrowserMainParts::PreMainMessageLoopRun calls.
//
//   3. CreateCloudBrowserDefaultAudioDeviceModule()
//      M1's default ADM — the dummy / no-audio path. This is the
//      M5.5 cross-module injection slot; M5.5 substitutes the real
//      cb-audio ADM at the audio_device_module slot without
//      touching any other M1 code (see CV2-26 audio-seam
//      sub-deliverable).
//
//   4. FormatPcfVideoCodecLogLine(...)
//      Pure formatter for the deterministic codec-cap probe log
//      line that M0 R5's assertion #3 swappable probe scrapes by
//      regex. CV2-27 hard contract; format drift silently breaks
//      assertion #3 — see fn comment for the exact format rules.
//
// Cross-references:
//   * patches/0003-add-cloud-browser-webrtc-overrides.patch (the
//     gn visibility passthrough this file's #includes ride on)
//   * capture/encoder/encoder_factory.h (the encoder factory
//     injected at the video_encoder_factory slot)
//   * cloud_browser_browser_main_parts.{h,cc} (consumer)
//   * cloud_browser_pcf_test.cc (RED tests for items 1-4)

#ifndef CAPTURE_BUILD_INTEGRATION_CLOUD_BROWSER_PCF_H_
#define CAPTURE_BUILD_INTEGRATION_CLOUD_BROWSER_PCF_H_

#include <string>
#include <vector>

#include "api/audio/audio_device.h"
#include "api/environment/environment.h"
#include "api/peer_connection_interface.h"
#include "api/rtp_parameters.h"
#include "api/scoped_refptr.h"
#include "rtc_base/thread.h"

namespace cloud_browser {

// Build the PeerConnectionFactoryDependencies struct for the browser-
// process PCF.
//
// Pure assembly. Inputs are slotted directly into the deps struct;
// no PCF object is constructed by this function. Caller owns the
// three threads + the environment + the ADM and must keep them
// alive for the lifetime of any PCF built from the returned deps.
//
// |network_thread|, |worker_thread|, |signaling_thread|: the three
// dedicated webrtc threads. Per CV2-26 R-thread DECISION, the
// browser-process embedder owns these as bare webrtc::Thread members
// (not ThreadWrappers around chromium task runners) — simpler
// lifetime ordering for the mandated PCF-teardown-before-
// browser_context_ ordering.
//
// |env|: the webrtc::Environment supplying clock + task-queue
// factory + field-trial config. Construct via webrtc::
// CreateEnvironment().
//
// |adm|: the AudioDeviceModule. When null, the assembly substitutes
// CreateCloudBrowserDefaultAudioDeviceModule() — the M1 dummy/no-
// audio path. Callers that want the dummy path explicitly may pass
// null; callers that want a custom ADM (M5.5+) pass it directly.
//
// Other slots filled by this fn (no caller-visible knob):
//   * video_encoder_factory = make_unique<CloudBrowserVideoEncoderFactory>
//     with default Config{} — VP9 + H264 + AV1 enabled, VP8 disabled.
//   * video_decoder_factory = libwebrtc builtin default.
//   * audio_encoder_factory = libwebrtc builtin via
//     webrtc::CreateBuiltinAudioEncoderFactory().
//   * audio_decoder_factory = libwebrtc builtin via
//     webrtc::CreateBuiltinAudioDecoderFactory().
//
// The caller is expected to then run webrtc::EnableMedia(deps) and
// hand the deps off to CreateModularPeerConnectionFactory(). The
// CreateCloudBrowserPcf wrapper below does both of these in one shot.
webrtc::PeerConnectionFactoryDependencies BuildCloudBrowserPcfDependencies(
    webrtc::Thread* network_thread,
    webrtc::Thread* worker_thread,
    webrtc::Thread* signaling_thread,
    const webrtc::Environment& env,
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm);

// Construct the browser-process PeerConnectionFactory.
//
// Thin wrapper:
//   1. Build deps via BuildCloudBrowserPcfDependencies(...).
//   2. Run webrtc::EnableMedia(deps) so the media engine slots are
//      populated from the encoder/decoder + ADM combo above.
//   3. Call webrtc::CreateModularPeerConnectionFactory(std::move(deps)).
//
// Returns the constructed PCF, or nullptr if PCF construction failed.
webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
CreateCloudBrowserPcf(
    webrtc::Thread* network_thread,
    webrtc::Thread* worker_thread,
    webrtc::Thread* signaling_thread,
    const webrtc::Environment& env,
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm);

// The cloud-browser default AudioDeviceModule.
//
// As of M5.5-R1 (CV2-28) this is the real libwebrtc built-in
// PulseAudio ADM (kPlatformDefaultAudio), constructed via
// capture/audio/cb_audio_device_module.cc. The recording device
// resolves to the PulseAudio server-default source — pinned to
// cb_capture.monitor by infra/pulse-default.pa.
//
// Fallback: if cb-audio's native ADM construction fails (PulseAudio
// not up, cb_capture.monitor not registered, etc.) the implementation
// falls back to webrtc::AudioDeviceModule::kDummyAudio so the
// browser-process worker still boots. The failure is logged.
//
// The function name is kept stable so every M1 call site
// (cloud_browser_browser_main_parts.cc, BuildCloudBrowserPcf
// Dependencies' nullptr-ADM fallback above) is unchanged. This is the
// cross-module hand-off seam M1-R2 set up specifically so M5.5 could
// land without touching M1 call sites.
webrtc::scoped_refptr<webrtc::AudioDeviceModule>
CreateCloudBrowserDefaultAudioDeviceModule();

// Format the deterministic codec-cap probe log line that M0 R5's
// assertion #3 swappable probe scrapes by regex.
//
// Output format (LOAD-BEARING):
//   "CloudBrowser: PCF video sender codecs = [VP9,H264,AV1]"
//
// Format rules — these are pinned by the M0 R5 scrape regex and
// format drift silently breaks assertion #3:
//   * Uppercase mime name (RtpCodecCapability::name is already
//     uppercase in libwebrtc, but the formatter does not assume
//     and does not lower-case).
//   * Comma separator, no surrounding spaces.
//   * Square brackets around the comma-joined list.
//   * Stable order = the input vector's order. Callers feeding this
//     fn must NOT pre-sort; the order reflects the encoder factory's
//     GetSupportedFormats() preference, which is observable to the
//     SDP layer and thus part of the contract.
//   * Empty input vector renders as the literal string
//     "CloudBrowser: PCF video sender codecs = []".
std::string FormatPcfVideoCodecLogLine(
    const std::vector<webrtc::RtpCodecCapability>& video_send_codecs);

// Return the transceiver codec-preference order used for native first-light
// verification.
//
// The PCF still advertises the full M1-ratified capability set above. This
// helper only reorders the per-transceiver offer preference so broadly
// supported H.264 is attempted before VP9. f76c6fc live smoke proved the VP9
// path can negotiate and send RTP but its payload metadata is malformed enough
// that receivers reject every packet before a decoded frame is produced.
std::vector<webrtc::RtpCodecCapability> BuildFirstLightVideoCodecPreferences(
    const std::vector<webrtc::RtpCodecCapability>& video_send_codecs);

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CLOUD_BROWSER_PCF_H_

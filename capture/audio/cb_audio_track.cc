// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cb_audio_track.cc — see cb_audio_track.h.
//
// CV2-31 R4: compose source + track + sendonly transceiver on the
// browser-process native peer. Stitches M5.5 R1 (ADM, via the PCF's
// audio_device_module slot) + M5.5 R3 (AudioOptions helper) + M1 PCF
// + M3 R4 PC into a single observable transceiver.

#include "capture/audio/cb_audio_track.h"

#include <string>
#include <utility>

#include "api/media_stream_interface.h"
#include "api/media_types.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/rtp_transceiver_direction.h"
#include "api/rtp_transceiver_interface.h"
#include "api/scoped_refptr.h"
#include "rtc_base/logging.h"

namespace cloud_browser {

namespace {

constexpr char kLogPrefix[] = "CloudBrowser: M5.5-R4: ";

}  // namespace

SendOnlyAudioTransceiver AddSendOnlyAudioTransceiver(
    webrtc::PeerConnectionFactoryInterface* pcf,
    webrtc::PeerConnectionInterface* pc,
    const webrtc::AudioOptions& audio_options,
    const std::string& track_label) {
  SendOnlyAudioTransceiver result;

  if (pcf == nullptr) {
    // A null PCF here means the browser-process PCF construction in
    // M1's CloudBrowserBrowserMainParts::PreMainMessageLoopRun never
    // ran (or ran and failed and the embedder didn't bail). That is a
    // programmer error, not a recoverable runtime condition — log and
    // return an empty triple so the caller can decide whether to
    // CHECK or continue without audio. We do not CHECK here because
    // M5.5 R5's lifecycle owner is allowed to gracefully proceed with
    // a video-only peer if audio cannot be wired.
    RTC_LOG(LS_ERROR) << kLogPrefix << "AddSendOnlyAudioTransceiver "
                         "called with null PCF — caller skipped M1 "
                         "PCF construction or didn't bail on M1 "
                         "failure";
    return result;
  }
  if (pc == nullptr) {
    RTC_LOG(LS_ERROR) << kLogPrefix << "AddSendOnlyAudioTransceiver "
                         "called with null PC — caller invoked R4 "
                         "before M3 R4's offerer driver finished "
                         "CreatePeerConnection";
    return result;
  }

  // Step 1: AudioSource.
  //
  // PCF::CreateAudioSource is sync — it constructs the source and
  // returns a ref. The source is bound to the ADM the PCF was built
  // with (M5.5 R1's CreateCloudBrowserNativeAudioDeviceModule, slotted
  // by M1's CreateCloudBrowserDefaultAudioDeviceModule). |options|
  // configures the source-level APM behaviour; R3's helper produces
  // a value with APM disabled (display-capture audio is already mixed
  // program audio).
  //
  // TODO(M55-R4-create-audio-source-overload): some chromium-bundled
  // webrtc revisions expose CreateAudioSource as either
  // CreateAudioSource(const webrtc::AudioOptions&) or
  // CreateAudioSource(const webrtc::AudioOptions&, AudioSourceOptions*).
  // The draft uses the single-arg form per the prevailing M1-era
  // convention; if first-compile on triform-8 reports an ambiguous
  // overload, drop in std::nullopt as the second arg.
  result.source = pcf->CreateAudioSource(audio_options);
  if (result.source == nullptr) {
    RTC_LOG(LS_ERROR) << kLogPrefix << "PCF::CreateAudioSource "
                         "returned null — most likely cause: ADM "
                         "construction earlier returned the dummy "
                         "fallback (see R1 log) and the dummy ADM "
                         "rejects source creation. Skipping audio.";
    return result;  // source/track/transceiver all null.
  }

  // Step 2: AudioTrack.
  //
  // CreateAudioTrack takes a NON-OWNING raw pointer to the source —
  // libwebrtc internally takes a strong ref via the track. We still
  // hold `result.source` so the source can outlive the track if
  // teardown ordering requires it (R5's parity-with-M2 contract).
  result.track = pcf->CreateAudioTrack(track_label, result.source.get());
  if (result.track == nullptr) {
    RTC_LOG(LS_ERROR) << kLogPrefix << "PCF::CreateAudioTrack "
                         "returned null for label=" << track_label
                      << " — should be infallible if the source is "
                         "non-null; treating as fatal for audio "
                         "wiring";
    result.source = nullptr;
    return result;
  }

  // Step 3: sendonly transceiver.
  //
  // RtpTransceiverInit defaults to direction=kSendRecv. We override
  // to kSendOnly because the cloud-browser worker never accepts
  // inbound audio (see header §"Why sendonly and not sendrecv").
  //
  // AddTransceiver returns RTCErrorOr<...>. On success the contained
  // value is the new transceiver; on failure RTCError carries a
  // message that we log verbatim — this is how libwebrtc surfaces
  // invariant violations (e.g. PC already closed, signaling-thread
  // assertion).
  webrtc::RtpTransceiverInit init;
  init.direction = webrtc::RtpTransceiverDirection::kSendOnly;

  // TODO(M55-R4-add-transceiver-overload): two AddTransceiver
  // overloads exist in modern libwebrtc:
  //   (a) AddTransceiver(webrtc::scoped_refptr<MediaStreamTrackInterface>,
  //                      const RtpTransceiverInit&)
  //   (b) AddTransceiver(cricket::MediaType, const RtpTransceiverInit&)
  // Form (a) is what we want — the transceiver is bound to our track
  // from construction, eliminating the SetTrack round-trip. If the
  // chromium-bundled tree's signature requires an explicit upcast,
  // wrap with webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface>(
  //     result.track).
  webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>>
      transceiver_or = pc->AddTransceiver(result.track, init);
  if (!transceiver_or.ok()) {
    RTC_LOG(LS_ERROR) << kLogPrefix << "PC::AddTransceiver(audio, "
                         "sendonly) failed: "
                      << transceiver_or.error().message();
    result.source = nullptr;
    result.track = nullptr;
    return result;
  }
  result.transceiver = transceiver_or.MoveValue();

  // Log the success line at INFO so a single grep against the worker
  // log can confirm M5.5 wiring landed. The format is informational
  // only — no scrape regex depends on this (the M5.5 acceptance check
  // in CV2-25 reads the SDP m-section, not this log).
  RTC_LOG(LS_INFO) << kLogPrefix << "audio transceiver attached "
                      "(direction=sendonly, track_label="
                   << track_label << ", source=" << result.source.get()
                   << ", track=" << result.track.get()
                   << ", transceiver=" << result.transceiver.get()
                   << ")";

  return result;
}

}  // namespace cloud_browser

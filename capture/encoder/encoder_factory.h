// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Encoder factory extension point for the cloud browser WebRTC pipeline.
//
// This is the single seam through which we plug video encoders into
// libwebrtc. The same interface is used for software encoders (libvpx VP9,
// x264) in v1 and hardware encoders (NVENC, VAAPI) in Phase 4. Adding a
// new encoder backend means adding a CreateVideoEncoder() branch and an
// SdpVideoFormat entry to GetSupportedFormats(); it never means changing
// the rest of the pipeline.
//
// Reference base class:
//   third_party/webrtc/api/video_codecs/video_encoder_factory.h
//   (libwebrtc, branch-heads of the pinned Chromium release; see
//   docs/build/chromium-from-source.md §6).
//
// Cross-references:
//   - docs/internal/encoder-factory-design.md  (rationale, low-latency
//     tuning, BWE hooks, HW slots)
//   - docs/build/chromium-from-source.md       (libwebrtc reference rev)
//   - docs/prior-art/selkies.md                (Selkies' encoder list +
//     SELKIES_ENCODER selection model we are mirroring)

#ifndef CAPTURE_ENCODER_ENCODER_FACTORY_H_
#define CAPTURE_ENCODER_ENCODER_FACTORY_H_

#include <memory>
#include <string>
#include <vector>

#include "absl/types/optional.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"

namespace cloud_browser {

class BweAdapter;  // capture/encoder/bwe_adapter.h

// CloudBrowserVideoEncoderFactory is the single VideoEncoderFactory
// installed on the libwebrtc PeerConnectionFactory used by the Phase 1
// streamer (and, in Phase 2+, by the Chromium-internal capture path).
//
// Contract:
//   * GetSupportedFormats() returns formats in preference order. The
//     order is observable to the SDP layer and influences which codec
//     the remote peer will pick — list our preferred codec first.
//   * CreateVideoEncoder(format) MUST return a working VideoEncoder for
//     any SdpVideoFormat returned by GetSupportedFormats(). It returns
//     nullptr (and only nullptr) for formats we do not support.
//   * QueryCodecSupport() reports whether a (format, scalability_mode)
//     pair is supported AND whether the implementation is "power
//     efficient" (i.e., hardware-backed). The default base
//     implementation answers from GetSupportedFormats(); we override it
//     in Phase 4 once HW encoders land so libwebrtc's encoder selector
//     can prefer them.
//
// All methods MUST be safe to call from libwebrtc's worker threads.
// Implementations own no state that would prevent that, except a const
// configuration provided at construction.
class CloudBrowserVideoEncoderFactory : public webrtc::VideoEncoderFactory {
 public:
  struct Config {
    // Software-encoder selection knobs. Keep this struct shaped so the
    // Phase 4 HW additions slot in without breaking call sites.
    bool enable_vp9 = true;
    bool enable_h264 = true;
    bool enable_vp8 = false;   // intentionally off in v1; included for
                                // future scalability experiments.

    // Latency tuning (applied to whichever encoder we instantiate). See
    // docs/internal/encoder-factory-design.md for the rationale behind
    // each knob; the defaults below match the Phase 1 budget.
    bool zero_latency = true;            // sets each encoder's lowest-
                                          // latency preset.
    bool disable_b_frames = true;        // no display-order reordering.
    bool intra_refresh = true;           // diffuse keyframes via
                                          // intra-refresh, not GOP-aligned
                                          // IDRs.
    int gop_length_frames = 240;         // small GOPs; rotated by
                                          // intra-refresh in steady state.

    // Optional BWE adapter (T58). When non-null, every encoder this
    // factory creates is wrapped so it registers with the adapter on
    // InitEncode and unregisters on Release. The adapter then routes
    // libwebrtc BWE updates to all active encoders. Owned by the
    // caller; must outlive every encoder this factory produces.
    BweAdapter* bwe_adapter = nullptr;

    // HW-encoder preferences (T63). When true and the runtime probe
    // succeeds, the factory hands back an NvencEncoder instead of
    // the SW wrapper for that codec. SW always remains the fallback
    // — see capture/encoder/README.md for the runtime-selection
    // rule.
    bool prefer_nvenc_h264 = false;
    bool prefer_nvenc_hevc = false;
    bool prefer_nvenc_av1  = false;
  };

  explicit CloudBrowserVideoEncoderFactory(Config config);
  ~CloudBrowserVideoEncoderFactory() override;

  // webrtc::VideoEncoderFactory:
  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;

  std::unique_ptr<webrtc::VideoEncoder> CreateVideoEncoder(
      const webrtc::SdpVideoFormat& format) override;

  webrtc::VideoEncoderFactory::CodecSupport QueryCodecSupport(
      const webrtc::SdpVideoFormat& format,
      absl::optional<std::string> scalability_mode) const override;

 private:
  const Config config_;
};

}  // namespace cloud_browser

#endif  // CAPTURE_ENCODER_ENCODER_FACTORY_H_

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// SimulcastEncoder — encoder-side half of T77's simulcast design.
//
// libwebrtc's VideoStreamEncoder, given an SDP with multiple `a=rid`
// entries, asks our VideoEncoderFactory for *one* VideoEncoder. That
// encoder is expected to internally produce N spatial layers (one
// per rid). This file is that encoder.
//
// Implementation strategy: hold N inner encoders, one per layer.
// Each inner encoder is a single-layer encoder we already wrote
// (Vp9Encoder T35, H264Encoder T36, etc.). On Encode() we downscale
// the source frame N-1 times and feed the layer-appropriate frame to
// each inner encoder. On SetRates() we split the
// VideoBitrateAllocation per spatial layer and forward.
//
// Why not libvpx's native `ts_number_layers`? Native libvpx
// simulcast in libvpx is temporal-scalability only — same resolution,
// different frame rates / temporal IDs. Our T77 ladder is spatial
// (1080p / 540p / 270p), so we need separate encoder instances at
// different resolutions. Same constraint applies to x264 — its
// simulcast support is limited and resolution-scaling lives outside
// the encoder. SVT-AV1 has scalability modes but they're aimed at
// VOD encoding, not realtime. NVENC and VAAPI have multi-resolution
// support but the API is per-vendor; the simplest unifying pattern
// is "N inner encoders behind one webrtc::VideoEncoder facade",
// which is what libwebrtc's own SimulcastEncoderAdapter does for
// the same reasons.
//
// Cross-references:
//   * capture/encoder/encoder_factory.h    (T19 — the factory seam)
//   * capture/encoder/bwe_adapter.h        (T58 — per-layer rate
//                                            distribution arrives via
//                                            our outer SetRates)
//   * docs/internal/simulcast-encoder-design.md (this task's design)
//   * docs/protocols/simulcast.md          (T77 — SDP / client side)

#ifndef CAPTURE_ENCODER_SIMULCAST_FACTORY_H_
#define CAPTURE_ENCODER_SIMULCAST_FACTORY_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "api/video/video_frame.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"
#include "base/memory/raw_ptr.h"

namespace cloud_browser {

// Per-layer config. Same shape as the JS-side ladder in
// capture/streamer-page/streamer.js (T77's parseSimulcastLayersParam).
struct SimulcastLayer {
  std::string rid;            // "layer0", "layer1", ...
  int scale_resolution_down_by = 1;  // 1 = full, 2 = half, 4 = quarter.
  int max_framerate_fps = 0;  // 0 = inherit from outer config.
  int max_bitrate_bps = 0;    // 0 = let BWE decide.

  // Declared out-of-line to satisfy chromium-style ("Complex
  // class/struct needs an explicit out-of-line constructor"). The
  // struct holds non-trivial members; pinning lifecycle bodies in
  // the .cc keeps them out of every TU that #includes this header.
  SimulcastLayer();
  // Convenience ctor for brace-init test fixtures and explicit setup.
  SimulcastLayer(std::string rid,
                 int scale_resolution_down_by,
                 int max_framerate_fps,
                 int max_bitrate_bps);
  ~SimulcastLayer();
  SimulcastLayer(const SimulcastLayer&);
  SimulcastLayer& operator=(const SimulcastLayer&);
  SimulcastLayer(SimulcastLayer&&);
  SimulcastLayer& operator=(SimulcastLayer&&);
};

// Factory closure: produces a fresh single-layer encoder per layer.
// The closure pattern keeps this header agnostic to which codec
// we are simulcasting — Vp9 / H264 / Nvenc / Vaapi all build their
// inner encoders with their own Config struct, so we let the caller
// inject one closure per (codec, layer) combination.
using InnerEncoderBuilder =
    std::function<std::unique_ptr<webrtc::VideoEncoder>(
        const SimulcastLayer& layer)>;

class SimulcastEncoder : public webrtc::VideoEncoder {
 public:
  // Explicit-ladder constructor — used by tests that want to pin a
  // specific layer set without going through libwebrtc's
  // VideoCodec.simulcastStream[]. `layers` ordered from top (highest
  // resolution) to bottom. `build_inner` is invoked once per layer
  // at InitEncode() to produce that layer's underlying encoder.
  SimulcastEncoder(std::vector<SimulcastLayer> layers,
                    InnerEncoderBuilder build_inner,
                    std::string codec_name);

  // Deferred-ladder constructor — the path the factory takes.
  // The layer set is derived from VideoCodec::simulcastStream[]
  // inside InitEncode. If InitEncode sees numberOfSimulcastStreams
  // < 2, we degenerate to a single inner at the source resolution
  // and the wrapper has near-zero overhead (one extra dispatch per
  // Encode + SetRates) — this is the "single-layer fast path stays
  // unchanged" promise from T83.
  SimulcastEncoder(InnerEncoderBuilder build_inner,
                    std::string codec_name);
  ~SimulcastEncoder() override;

  SimulcastEncoder(const SimulcastEncoder&) = delete;
  SimulcastEncoder& operator=(const SimulcastEncoder&) = delete;

  // webrtc::VideoEncoder:
  int32_t InitEncode(const webrtc::VideoCodec* codec_settings,
                     const webrtc::VideoEncoder::Settings& settings) override;

  int32_t Encode(
      const webrtc::VideoFrame& frame,
      const std::vector<webrtc::VideoFrameType>* frame_types) override;

  int32_t RegisterEncodeCompleteCallback(
      webrtc::EncodedImageCallback* callback) override;

  int32_t Release() override;

  void SetRates(const RateControlParameters& parameters) override;

  EncoderInfo GetEncoderInfo() const override;

 private:
  // Per-layer state kept on the Encode hot path. The `inner` is the
  // actual codec instance for this layer; `tagging_callback` rewrites
  // EncodedImage::spatial_index_ + simulcast_idx so libwebrtc's RTP
  // packetizer routes the output to the right SSRC.
  struct LayerState;

  std::vector<SimulcastLayer> layers_;
  InnerEncoderBuilder build_inner_;
  std::string codec_name_;

  std::vector<std::unique_ptr<LayerState>> states_;
  raw_ptr<webrtc::EncodedImageCallback> outer_callback_ = nullptr;

  // Source dimensions captured at InitEncode; used for the per-
  // layer downscale.
  int source_width_ = 0;
  int source_height_ = 0;
  bool initialized_ = false;
};

// Helpers — inspect a VideoCodec to decide whether it carries
// simulcast (libwebrtc's VideoCodec::numberOfSimulcastStreams >= 2),
// and convert its simulcastStream[] into our SimulcastLayer vector
// for SimulcastEncoder construction.
bool IsSimulcast(const webrtc::VideoCodec& codec_settings);
std::vector<SimulcastLayer> SimulcastLayersFromCodec(
    const webrtc::VideoCodec& codec_settings);

}  // namespace cloud_browser

#endif  // CAPTURE_ENCODER_SIMULCAST_FACTORY_H_

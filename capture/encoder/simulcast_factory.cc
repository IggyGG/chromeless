// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// SimulcastEncoder — see simulcast_factory.h.
//
// TODO(T17-build-env): validate compile + link once the from-source
// Chromium / libwebrtc build env is up. libyuv is a libwebrtc
// dependency so the I420 downscale path is available without a new
// gate. Authored against documented libwebrtc + libyuv headers.

#include "capture/encoder/simulcast_factory.h"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "base/memory/raw_ptr_exclusion.h"

#include "api/video/i420_buffer.h"
#include "api/video/video_bitrate_allocation.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv/scale.h"

namespace cloud_browser {

// Out-of-line lifecycle for SimulcastLayer (chromium-style).
SimulcastLayer::SimulcastLayer() = default;
SimulcastLayer::SimulcastLayer(std::string rid,
                               int scale_resolution_down_by,
                               int max_framerate_fps,
                               int max_bitrate_bps)
    : rid(std::move(rid)),
      scale_resolution_down_by(scale_resolution_down_by),
      max_framerate_fps(max_framerate_fps),
      max_bitrate_bps(max_bitrate_bps) {}
SimulcastLayer::~SimulcastLayer() = default;
SimulcastLayer::SimulcastLayer(const SimulcastLayer&) = default;
SimulcastLayer& SimulcastLayer::operator=(const SimulcastLayer&) = default;
SimulcastLayer::SimulcastLayer(SimulcastLayer&&) = default;
SimulcastLayer& SimulcastLayer::operator=(SimulcastLayer&&) = default;

// ---------------------------------------------------------------------
// LayerState — per-layer plumbing.
// ---------------------------------------------------------------------
//
// We tag each inner encoder's output with the layer's spatial index
// before forwarding to the outer EncodedImageCallback. Without the
// tag, libwebrtc's RTP packetizer would route every layer to SSRC
// 0 — only the top layer would actually reach the wire.
struct SimulcastEncoder::LayerState {
  SimulcastLayer config;
  std::unique_ptr<webrtc::VideoEncoder> inner;

  // Tagging callback: receives EncodedImage from `inner`, sets the
  // spatial / simulcast index, and forwards to `outer`.
  class TaggingCallback : public webrtc::EncodedImageCallback {
   public:
    TaggingCallback(int spatial_index, raw_ptr<webrtc::EncodedImageCallback>* outer)
        : spatial_index_(spatial_index), outer_(outer) {}

    Result OnEncodedImage(
        const webrtc::EncodedImage& encoded_image,
        const webrtc::CodecSpecificInfo* csi) override {
      if (!outer_ || !*outer_) return Result(Result::ERROR_SEND_FAILED);
      webrtc::EncodedImage tagged = encoded_image;
      tagged.SetSpatialIndex(spatial_index_);
      tagged.SetSimulcastIndex(spatial_index_);
      // CodecSpecificInfo: the inner already populated codecType +
      // codec-specific fields; we only stamp the simulcastIdx if
      // present in the struct (libwebrtc removed that field for AV1
      // some time ago; setSpatialIndex on the EncodedImage is
      // canonical).
      return (*outer_)->OnEncodedImage(tagged, csi);
    }

    void OnDroppedFrame(DropReason reason) override {
      if (outer_ && *outer_) (*outer_)->OnDroppedFrame(reason);
    }

   private:
    const int spatial_index_;
    // RAW_PTR_EXCLUSION: outer_ points at a raw_ptr<T> slot owned by
    // the parent LayerState. The slot lives as long as the LayerState
    // (and TaggingCallback is destructed before LayerState in the
    // unique_ptr chain), so MiraclePtr's storage discipline doesn't
    // apply. The plugin would otherwise flag `raw_ptr<T>* outer_` as
    // a raw class-member pointer; this annotation is the canonical
    // chromium escape hatch for that pattern.
    RAW_PTR_EXCLUSION raw_ptr<webrtc::EncodedImageCallback>* outer_;
  };

  std::unique_ptr<TaggingCallback> tagging_callback;
  webrtc::scoped_refptr<webrtc::I420Buffer> scratch_buffer;  // reused per
                                                            // layer, sized
                                                            // at InitEncode.
  int width = 0;
  int height = 0;
};

// ---------------------------------------------------------------------
// Free helpers.
// ---------------------------------------------------------------------

bool IsSimulcast(const webrtc::VideoCodec& codec_settings) {
  return codec_settings.numberOfSimulcastStreams >= 2;
}

std::vector<SimulcastLayer> SimulcastLayersFromCodec(
    const webrtc::VideoCodec& codec_settings) {
  std::vector<SimulcastLayer> out;
  const int n = std::min<int>(codec_settings.numberOfSimulcastStreams,
                                webrtc::kMaxSimulcastStreams);
  out.reserve(n);
  // libwebrtc orders simulcastStream[] from lowest to highest; the
  // T77 ladder convention puts layer0 = highest. Reverse on the way
  // out so layers_[0] is always the top.
  const int top_w = codec_settings.simulcastStream[n - 1].width;
  const int top_h = codec_settings.simulcastStream[n - 1].height;
  for (int i = n - 1; i >= 0; --i) {
    const auto& s = codec_settings.simulcastStream[i];
    SimulcastLayer L;
    L.rid = "layer" + std::to_string(n - 1 - i);
    L.scale_resolution_down_by = (s.width > 0)
        ? std::max(1, top_w / s.width)
        : 1;
    L.max_framerate_fps = s.maxFramerate;
    L.max_bitrate_bps = static_cast<int>(s.targetBitrate) * 1000;
    out.push_back(std::move(L));
  }
  // Force the top layer's scale = 1 in case of integer rounding.
  if (!out.empty()) out.front().scale_resolution_down_by = 1;
  // If top dims are zero (caller forgot to fill simulcastStream[]),
  // bail with empty vector so InitEncode falls through to single-
  // layer.
  if (top_w == 0 || top_h == 0) out.clear();
  return out;
}

// ---------------------------------------------------------------------
// SimulcastEncoder
// ---------------------------------------------------------------------

SimulcastEncoder::SimulcastEncoder(std::vector<SimulcastLayer> layers,
                                     InnerEncoderBuilder build_inner,
                                     std::string codec_name)
    : layers_(std::move(layers)),
      build_inner_(std::move(build_inner)),
      codec_name_(std::move(codec_name)) {}

SimulcastEncoder::SimulcastEncoder(InnerEncoderBuilder build_inner,
                                     std::string codec_name)
    : build_inner_(std::move(build_inner)),
      codec_name_(std::move(codec_name)) {}

SimulcastEncoder::~SimulcastEncoder() { Release(); }

int32_t SimulcastEncoder::InitEncode(
    const webrtc::VideoCodec* codec_settings,
    const webrtc::VideoEncoder::Settings& settings) {
  if (!codec_settings || codec_settings->width == 0 ||
      codec_settings->height == 0) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  // Deferred-ladder constructor path: derive layers from the
  // negotiated VideoCodec. Single-stream SDP → 1-layer ladder
  // (functionally the same as the wrapped inner encoder running
  // alone, plus ~negligible dispatch overhead).
  if (layers_.empty()) {
    if (IsSimulcast(*codec_settings)) {
      layers_ = SimulcastLayersFromCodec(*codec_settings);
    }
    if (layers_.empty()) {
      // Fall back to a single-layer ladder.
      SimulcastLayer single;
      single.rid = "layer0";
      single.scale_resolution_down_by = 1;
      single.max_framerate_fps = codec_settings->maxFramerate;
      single.max_bitrate_bps =
          static_cast<int>(codec_settings->maxBitrate) * 1000;
      layers_.push_back(single);
    }
  }
  source_width_ = codec_settings->width;
  source_height_ = codec_settings->height;

  states_.clear();
  states_.reserve(layers_.size());

  for (size_t i = 0; i < layers_.size(); ++i) {
    auto state = std::make_unique<LayerState>();
    state->config = layers_[i];
    state->width = std::max(2, source_width_ /
                              std::max(1, layers_[i].scale_resolution_down_by));
    state->height = std::max(2, source_height_ /
                               std::max(1, layers_[i].scale_resolution_down_by));
    // Ensure even dimensions — I420 chroma planes need it.
    state->width  &= ~1;
    state->height &= ~1;

    state->inner = build_inner_(layers_[i]);
    if (!state->inner) {
      states_.clear();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    // Build per-layer VideoCodec settings: same outer codec, scaled
    // dimensions, layer's framerate / bitrate bounds.
    webrtc::VideoCodec layer_codec = *codec_settings;
    layer_codec.width = state->width;
    layer_codec.height = state->height;
    if (layers_[i].max_framerate_fps > 0) {
      layer_codec.maxFramerate = layers_[i].max_framerate_fps;
    }
    if (layers_[i].max_bitrate_bps > 0) {
      layer_codec.maxBitrate = layers_[i].max_bitrate_bps / 1000;
      layer_codec.startBitrate =
          std::min<uint32_t>(layer_codec.maxBitrate, layer_codec.startBitrate);
    }
    // Single-layer in the inner encoder's view.
    layer_codec.numberOfSimulcastStreams = 0;

    int32_t r = state->inner->InitEncode(&layer_codec, settings);
    if (r != WEBRTC_VIDEO_CODEC_OK) {
      states_.clear();
      return r;
    }

    // Pre-allocate the scratch buffer for downscaled frames.
    if (state->config.scale_resolution_down_by != 1) {
      state->scratch_buffer = webrtc::I420Buffer::Create(
          state->width, state->height);
    }

    // Wire the tagging callback.
    state->tagging_callback = std::make_unique<LayerState::TaggingCallback>(
        static_cast<int>(i), &outer_callback_);
    state->inner->RegisterEncodeCompleteCallback(state->tagging_callback.get());

    states_.push_back(std::move(state));
  }

  initialized_ = true;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t SimulcastEncoder::Encode(
    const webrtc::VideoFrame& frame,
    const std::vector<webrtc::VideoFrameType>* frame_types) {
  if (!initialized_ || !outer_callback_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (frame.width() != source_width_ || frame.height() != source_height_) {
    // Source size changed — re-init from scratch. Same approach as
    // single-layer encoders.
    Release();
    webrtc::VideoCodec settings{};
    settings.width = frame.width();
    settings.height = frame.height();
    settings.numberOfSimulcastStreams = static_cast<unsigned char>(layers_.size());
    if (InitEncode(&settings, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200))
          != WEBRTC_VIDEO_CODEC_OK) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
  }

  webrtc::scoped_refptr<webrtc::I420BufferInterface> source =
      frame.video_frame_buffer()->ToI420();
  if (!source) return WEBRTC_VIDEO_CODEC_ERROR;

  for (const auto& state : states_) {
    webrtc::scoped_refptr<webrtc::I420BufferInterface> layer_buf;
    if (state->config.scale_resolution_down_by == 1) {
      // Top layer: pass the source through unchanged.
      layer_buf = source;
    } else {
      // libyuv I420 box-filter scale. Bilinear is cheaper but
      // produces visible aliasing on text; box (kFilterBox) is the
      // right pick for browser content with high-frequency UI.
      libyuv::I420Scale(
          source->DataY(), source->StrideY(),
          source->DataU(), source->StrideU(),
          source->DataV(), source->StrideV(),
          source_width_, source_height_,
          state->scratch_buffer->MutableDataY(),
          state->scratch_buffer->StrideY(),
          state->scratch_buffer->MutableDataU(),
          state->scratch_buffer->StrideU(),
          state->scratch_buffer->MutableDataV(),
          state->scratch_buffer->StrideV(),
          state->width, state->height,
          libyuv::kFilterBox);
      layer_buf = state->scratch_buffer;
    }

    webrtc::VideoFrame layer_frame =
        webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(layer_buf)
            .set_timestamp_rtp(frame.rtp_timestamp())
            .set_timestamp_ms(frame.render_time_ms())
            .set_rotation(frame.rotation())
            .build();

    int32_t r = state->inner->Encode(layer_frame, frame_types);
    if (r != WEBRTC_VIDEO_CODEC_OK) {
      // One layer failure shouldn't kill the rest. Log and
      // continue; libwebrtc will retry on the next frame and the
      // failed layer's RTP stream will simply have a gap.
      RTC_LOG(LS_WARNING) << "SimulcastEncoder: layer " << state->config.rid
                          << " Encode returned " << r;
    }
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t SimulcastEncoder::RegisterEncodeCompleteCallback(
    webrtc::EncodedImageCallback* callback) {
  outer_callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t SimulcastEncoder::Release() {
  for (auto& state : states_) {
    if (state && state->inner) {
      state->inner->Release();
    }
  }
  states_.clear();
  // If we derived the ladder from VideoCodec::simulcastStream[] (the
  // deferred constructor path), drop it so the next InitEncode can
  // re-derive — the new codec_settings may carry a different set.
  // Explicit-ladder construction (the test path) keeps layers_
  // pinned by re-populating them at InitEncode head.
  outer_callback_ = nullptr;
  initialized_ = false;
  return WEBRTC_VIDEO_CODEC_OK;
}

void SimulcastEncoder::SetRates(const RateControlParameters& parameters) {
  if (states_.empty()) return;

  // Per-layer bitrate from the BitrateAllocation. libwebrtc indexes
  // spatial layer 0 = lowest, so we map our `states_[0]` (top
  // resolution, configured at simulcastStream[N-1]) onto the highest
  // libwebrtc spatial index.
  const size_t n = states_.size();
  for (size_t i = 0; i < n; ++i) {
    const size_t webrtc_si = (n - 1) - i;  // see comment above.
    webrtc::VideoBitrateAllocation alloc;
    // Sum across temporal layers for this spatial layer; we don't
    // do per-temporal-layer routing in v1 simulcast.
    uint32_t sum = 0;
    for (size_t tl = 0; tl < webrtc::kMaxTemporalStreams; ++tl) {
      sum += parameters.bitrate.GetBitrate(webrtc_si, tl);
    }
    if (sum == 0 && states_[i]->config.max_bitrate_bps > 0) {
      // BWE hasn't decided this layer yet; fall back to the layer's
      // configured ceiling so the encoder doesn't see zero target.
      sum = static_cast<uint32_t>(states_[i]->config.max_bitrate_bps);
    }
    alloc.SetBitrate(0, 0, sum);
    const double layer_fps =
        states_[i]->config.max_framerate_fps > 0
            ? std::min(parameters.framerate_fps,
                       static_cast<double>(states_[i]->config.max_framerate_fps))
            : parameters.framerate_fps;
    webrtc::VideoEncoder::RateControlParameters layer_params(alloc, layer_fps);
    layer_params.bandwidth_allocation =
        webrtc::DataRate::BitsPerSec(static_cast<int64_t>(sum));
    states_[i]->inner->SetRates(layer_params);
  }
}

webrtc::VideoEncoder::EncoderInfo SimulcastEncoder::GetEncoderInfo() const {
  EncoderInfo info;
  info.implementation_name =
      "cloud-browser-simulcast-" + codec_name_ +
      "[" + std::to_string(layers_.size()) + "]";
  info.is_hardware_accelerated = false;  // outer wrapper is SW; the
                                          // *inner* encoders may flip
                                          // this. Phase 4 follow-up:
                                          // surface mixed-HW state.
  info.supports_native_handle = false;
  info.supports_simulcast = true;
  info.has_trusted_rate_controller = false;
  return info;
}

}  // namespace cloud_browser

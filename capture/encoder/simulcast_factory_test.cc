// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Tests for SimulcastEncoder (T83). Mock per-layer encoders so we
// can assert layer count, per-layer bitrate split, downscale
// dimensions, and Done()-on-Release semantics without needing
// libvpx / x264 / NVENC etc. on the host.
//
// TODO(T17-build-env): wire into the libwebrtc gtest target.

#include "capture/encoder/simulcast_factory.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "api/video/i420_buffer.h"
#include "api/video/video_bitrate_allocation.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_codec.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "test/gmock.h"
#include "test/gtest.h"

namespace cloud_browser {
namespace {

class FakeEncoder : public webrtc::VideoEncoder {
 public:
  struct Sink {
    int init_count = 0;
    int release_count = 0;
    std::vector<int> encode_widths;
    std::vector<int> encode_heights;
    std::vector<int> set_rates_bps;
    std::vector<double> set_rates_fps;
    int last_codec_width = 0;
    int last_codec_height = 0;
  };

  explicit FakeEncoder(Sink* sink) : sink_(sink) {}

  int32_t InitEncode(const webrtc::VideoCodec* codec_settings,
                     const webrtc::VideoEncoder::Settings&) override {
    ++sink_->init_count;
    if (codec_settings) {
      sink_->last_codec_width = codec_settings->width;
      sink_->last_codec_height = codec_settings->height;
    }
    return WEBRTC_VIDEO_CODEC_OK;
  }
  int32_t Encode(
      const webrtc::VideoFrame& frame,
      const std::vector<webrtc::VideoFrameType>*) override {
    sink_->encode_widths.push_back(frame.width());
    sink_->encode_heights.push_back(frame.height());
    if (callback_) {
      // Emit a stub EncodedImage so the SimulcastEncoder's tagging
      // callback has something to tag + forward.
      webrtc::EncodedImage image;
      static constexpr uint8_t kStubPayload = 0xab;
      image.SetEncodedData(webrtc::EncodedImageBuffer::Create(&kStubPayload, 1));
      image._frameType = webrtc::VideoFrameType::kVideoFrameDelta;
      image._encodedWidth = frame.width();
      image._encodedHeight = frame.height();
      webrtc::CodecSpecificInfo csi{};
      csi.codecType = webrtc::kVideoCodecVP9;
      callback_->OnEncodedImage(image, &csi);
    }
    return WEBRTC_VIDEO_CODEC_OK;
  }
  int32_t RegisterEncodeCompleteCallback(
      webrtc::EncodedImageCallback* cb) override {
    callback_ = cb;
    return WEBRTC_VIDEO_CODEC_OK;
  }
  int32_t Release() override { ++sink_->release_count; return WEBRTC_VIDEO_CODEC_OK; }
  void SetRates(const RateControlParameters& p) override {
    sink_->set_rates_bps.push_back(static_cast<int>(p.bitrate.get_sum_bps()));
    sink_->set_rates_fps.push_back(p.framerate_fps);
  }
  EncoderInfo GetEncoderInfo() const override { return EncoderInfo(); }

 private:
  Sink* sink_;
  webrtc::EncodedImageCallback* callback_ = nullptr;
};

class CapturingOuterCallback : public webrtc::EncodedImageCallback {
 public:
  Result OnEncodedImage(
      const webrtc::EncodedImage& encoded_image,
      const webrtc::CodecSpecificInfo*) override {
    spatial_indices_.push_back(encoded_image.SpatialIndex().value_or(-1));
    simulcast_indices_.push_back(encoded_image.SimulcastIndex().value_or(-1));
    return Result(Result::OK);
  }
  const std::vector<int>& spatial_indices() const { return spatial_indices_; }
  const std::vector<int>& simulcast_indices() const { return simulcast_indices_; }
 private:
  std::vector<int> spatial_indices_;
  std::vector<int> simulcast_indices_;
};

webrtc::VideoFrame MakeFrame(int w, int h) {
  webrtc::scoped_refptr<webrtc::I420Buffer> buf = webrtc::I420Buffer::Create(w, h);
  std::memset(buf->MutableDataY(), 128, buf->StrideY() * h);
  std::memset(buf->MutableDataU(), 128, buf->StrideU() * (h / 2));
  std::memset(buf->MutableDataV(), 128, buf->StrideV() * (h / 2));
  return webrtc::VideoFrame::Builder()
      .set_video_frame_buffer(buf)
      .set_timestamp_rtp(0)
      .set_timestamp_ms(0)
      .build();
}

webrtc::VideoCodec ThreeLayerCodec(int w, int h) {
  webrtc::VideoCodec s{};
  s.codecType = webrtc::kVideoCodecVP9;
  s.width = w;
  s.height = h;
  s.maxFramerate = 30;
  s.startBitrate = 4000;
  s.numberOfSimulcastStreams = 3;
  // libwebrtc orders simulcastStream[] from lowest → highest.
  s.simulcastStream[0].width = w / 4;  s.simulcastStream[0].height = h / 4;
  s.simulcastStream[0].maxFramerate = 15;
  s.simulcastStream[0].targetBitrate = 400;  // kbps.
  s.simulcastStream[1].width = w / 2;  s.simulcastStream[1].height = h / 2;
  s.simulcastStream[1].maxFramerate = 30;
  s.simulcastStream[1].targetBitrate = 1500;
  s.simulcastStream[2].width = w;      s.simulcastStream[2].height = h;
  s.simulcastStream[2].maxFramerate = 30;
  s.simulcastStream[2].targetBitrate = 4000;
  return s;
}

webrtc::VideoCodec SingleLayerCodec(int w, int h) {
  webrtc::VideoCodec s{};
  s.codecType = webrtc::kVideoCodecVP9;
  s.width = w;
  s.height = h;
  s.maxFramerate = 30;
  s.startBitrate = 4000;
  s.numberOfSimulcastStreams = 0;
  return s;
}

// ---------------------------------------------------------------------
// IsSimulcast / SimulcastLayersFromCodec
// ---------------------------------------------------------------------

TEST(SimulcastHelpers, IsSimulcastDetectsMultiStream) {
  EXPECT_TRUE(IsSimulcast(ThreeLayerCodec(1920, 1080)));
  EXPECT_FALSE(IsSimulcast(SingleLayerCodec(1920, 1080)));
}

TEST(SimulcastHelpers, LayersFromCodecOrderTopFirst) {
  auto layers = SimulcastLayersFromCodec(ThreeLayerCodec(1920, 1080));
  ASSERT_EQ(3u, layers.size());
  EXPECT_EQ("layer0", layers[0].rid);
  EXPECT_EQ(1, layers[0].scale_resolution_down_by);  // top.
  EXPECT_EQ("layer1", layers[1].rid);
  EXPECT_EQ(2, layers[1].scale_resolution_down_by);  // half.
  EXPECT_EQ("layer2", layers[2].rid);
  EXPECT_EQ(4, layers[2].scale_resolution_down_by);  // quarter.
  EXPECT_EQ(15, layers[2].max_framerate_fps);        // bottom layer ½ fps.
}

// ---------------------------------------------------------------------
// SimulcastEncoder — explicit ladder constructor (test path).
// ---------------------------------------------------------------------

TEST(SimulcastEncoderTest, InitEncodeAllocatesNInnerEncoders) {
  std::vector<FakeEncoder::Sink> sinks(3);
  size_t i = 0;
  InnerEncoderBuilder build = [&](const SimulcastLayer&) {
    return std::make_unique<FakeEncoder>(&sinks[i++]);
  };
  std::vector<SimulcastLayer> layers = {
      {"layer0", 1, 0, 4'000'000},
      {"layer1", 2, 0, 1'500'000},
      {"layer2", 4, 15, 400'000},
  };
  SimulcastEncoder enc(layers, build, "vp9");
  CapturingOuterCallback cb;
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto codec = SingleLayerCodec(640, 360);  // single-stream codec; the
                                            // explicit ladder still wins
                                            // because layers_ is preset.
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&codec, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200)));
  EXPECT_EQ(1, sinks[0].init_count);
  EXPECT_EQ(1, sinks[1].init_count);
  EXPECT_EQ(1, sinks[2].init_count);
  EXPECT_EQ(640, sinks[0].last_codec_width);
  EXPECT_EQ(320, sinks[1].last_codec_width);
  EXPECT_EQ(160, sinks[2].last_codec_width);
}

TEST(SimulcastEncoderTest, EncodeFansOutToAllLayersWithCorrectDimensions) {
  std::vector<FakeEncoder::Sink> sinks(3);
  size_t i = 0;
  InnerEncoderBuilder build = [&](const SimulcastLayer&) {
    return std::make_unique<FakeEncoder>(&sinks[i++]);
  };
  std::vector<SimulcastLayer> layers = {
      {"layer0", 1, 0, 0},
      {"layer1", 2, 0, 0},
      {"layer2", 4, 0, 0},
  };
  SimulcastEncoder enc(layers, build, "vp9");
  CapturingOuterCallback cb;
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto codec = SingleLayerCodec(640, 360);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&codec, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200)));
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.Encode(MakeFrame(640, 360), nullptr));
  ASSERT_EQ(1u, sinks[0].encode_widths.size());
  EXPECT_EQ(640, sinks[0].encode_widths.back());
  EXPECT_EQ(320, sinks[1].encode_widths.back());
  EXPECT_EQ(160, sinks[2].encode_widths.back());
}

TEST(SimulcastEncoderTest, EncodeOutputCarriesSpatialIndex) {
  std::vector<FakeEncoder::Sink> sinks(2);
  size_t i = 0;
  InnerEncoderBuilder build = [&](const SimulcastLayer&) {
    return std::make_unique<FakeEncoder>(&sinks[i++]);
  };
  std::vector<SimulcastLayer> layers = {
      {"layer0", 1, 0, 0},
      {"layer1", 2, 0, 0},
  };
  SimulcastEncoder enc(layers, build, "vp9");
  CapturingOuterCallback cb;
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto codec = SingleLayerCodec(640, 360);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&codec, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200)));
  enc.Encode(MakeFrame(640, 360), nullptr);
  ASSERT_EQ(2u, cb.spatial_indices().size());
  EXPECT_EQ(0, cb.spatial_indices()[0]);
  EXPECT_EQ(1, cb.spatial_indices()[1]);
  // simulcast_idx should match.
  EXPECT_EQ(cb.simulcast_indices()[0], cb.spatial_indices()[0]);
  EXPECT_EQ(cb.simulcast_indices()[1], cb.spatial_indices()[1]);
}

TEST(SimulcastEncoderTest, SetRatesDistributesPerSpatialLayer) {
  std::vector<FakeEncoder::Sink> sinks(3);
  size_t i = 0;
  InnerEncoderBuilder build = [&](const SimulcastLayer&) {
    return std::make_unique<FakeEncoder>(&sinks[i++]);
  };
  std::vector<SimulcastLayer> layers = {
      {"layer0", 1, 0, 4'000'000},
      {"layer1", 2, 0, 1'500'000},
      {"layer2", 4, 0, 400'000},
  };
  SimulcastEncoder enc(layers, build, "vp9");
  CapturingOuterCallback cb;
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto codec = SingleLayerCodec(640, 360);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&codec, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200)));

  // libwebrtc orders spatial layer 0 = lowest. Our states_[0] is
  // the top, so SetRates routes spatial 2 → states_[0], spatial 1
  // → states_[1], spatial 0 → states_[2].
  webrtc::VideoBitrateAllocation alloc;
  alloc.SetBitrate(0, 0,   400'000);  // bottom.
  alloc.SetBitrate(1, 0, 1'500'000);  // mid.
  alloc.SetBitrate(2, 0, 4'000'000);  // top.
  webrtc::VideoEncoder::RateControlParameters params(alloc, 30.0);
  enc.SetRates(params);

  ASSERT_EQ(1u, sinks[0].set_rates_bps.size());
  EXPECT_EQ(4'000'000, sinks[0].set_rates_bps.back()) << "top layer";
  EXPECT_EQ(1'500'000, sinks[1].set_rates_bps.back()) << "mid layer";
  EXPECT_EQ(  400'000, sinks[2].set_rates_bps.back()) << "bottom layer";
}

TEST(SimulcastEncoderTest, ReleaseTearsDownAllLayers) {
  std::vector<FakeEncoder::Sink> sinks(3);
  size_t i = 0;
  InnerEncoderBuilder build = [&](const SimulcastLayer&) {
    return std::make_unique<FakeEncoder>(&sinks[i++]);
  };
  std::vector<SimulcastLayer> layers = {
      {"layer0", 1, 0, 0},
      {"layer1", 2, 0, 0},
      {"layer2", 4, 0, 0},
  };
  SimulcastEncoder enc(layers, build, "vp9");
  CapturingOuterCallback cb;
  enc.RegisterEncodeCompleteCallback(&cb);
  auto codec = SingleLayerCodec(640, 360);
  enc.InitEncode(&codec, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200));
  enc.Release();
  EXPECT_EQ(1, sinks[0].release_count);
  EXPECT_EQ(1, sinks[1].release_count);
  EXPECT_EQ(1, sinks[2].release_count);
}

// ---------------------------------------------------------------------
// Deferred-ladder constructor (factory path).
// ---------------------------------------------------------------------

TEST(SimulcastEncoderTest, DeferredLadderPathDerivesLayersFromCodec) {
  std::atomic<int> built{0};
  std::vector<FakeEncoder::Sink> sinks(3);
  InnerEncoderBuilder build = [&](const SimulcastLayer&) {
    int idx = built.fetch_add(1);
    return std::make_unique<FakeEncoder>(&sinks[idx]);
  };
  SimulcastEncoder enc(build, "vp9");
  CapturingOuterCallback cb;
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto codec = ThreeLayerCodec(1920, 1080);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&codec, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200)));
  EXPECT_EQ(3, built.load());
  EXPECT_EQ(1920, sinks[0].last_codec_width);
  EXPECT_EQ(960,  sinks[1].last_codec_width);
  EXPECT_EQ(480,  sinks[2].last_codec_width);
}

TEST(SimulcastEncoderTest, DeferredLadderSingleStreamFastPath) {
  // Single-stream SDP → 1 inner encoder, near-zero overhead.
  std::atomic<int> built{0};
  std::vector<FakeEncoder::Sink> sinks(1);
  InnerEncoderBuilder build = [&](const SimulcastLayer&) {
    built.fetch_add(1);
    return std::make_unique<FakeEncoder>(&sinks[0]);
  };
  SimulcastEncoder enc(build, "vp9");
  CapturingOuterCallback cb;
  enc.RegisterEncodeCompleteCallback(&cb);
  auto codec = SingleLayerCodec(640, 360);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&codec, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200)));
  EXPECT_EQ(1, built.load());
  enc.Encode(MakeFrame(640, 360), nullptr);
  ASSERT_EQ(1u, sinks[0].encode_widths.size());
  EXPECT_EQ(640, sinks[0].encode_widths.back());
  ASSERT_EQ(1u, cb.spatial_indices().size());
  EXPECT_EQ(0, cb.spatial_indices()[0]);
}

}  // namespace
}  // namespace cloud_browser

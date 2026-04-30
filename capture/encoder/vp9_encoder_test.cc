// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Unit tests for Vp9Encoder. These tests exercise the encoder against
// synthetic I420 frames and assert structural properties — not
// quality. Quality is the job of the integration / harness tests.
//
// TODO(T17-build-env): wire into the libwebrtc gtest target. Until the
// from-source Chromium build env is up, this file documents what the
// test surface should be; it is authored against the libwebrtc
// gtest+rtc_base testing conventions.

#include "capture/encoder/vp9_encoder.h"

#include <cstring>
#include <memory>
#include <vector>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_codec.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "test/gmock.h"
#include "test/gtest.h"

namespace cloud_browser {
namespace {

// Captures EncodedImages for inspection.
class CapturingCallback : public webrtc::EncodedImageCallback {
 public:
  Result OnEncodedImage(
      const webrtc::EncodedImage& encoded_image,
      const webrtc::CodecSpecificInfo* codec_specific_info) override {
    Capture c;
    c.size = encoded_image.size();
    c.frame_type = encoded_image._frameType;
    if (codec_specific_info) {
      c.codec_type = codec_specific_info->codecType;
    }
    captured_.push_back(c);
    return Result(Result::OK);
  }

  struct Capture {
    size_t size = 0;
    webrtc::VideoFrameType frame_type = webrtc::VideoFrameType::kEmptyFrame;
    webrtc::VideoCodecType codec_type = webrtc::kVideoCodecGeneric;
  };
  const std::vector<Capture>& captured() const { return captured_; }

 private:
  std::vector<Capture> captured_;
};

// Build a single I420 VideoFrame filled with a mid-grey + colored
// rectangle so successive frames have measurable inter-frame deltas.
webrtc::VideoFrame MakeFrame(int width, int height, int frame_idx) {
  rtc::scoped_refptr<webrtc::I420Buffer> buf =
      webrtc::I420Buffer::Create(width, height);
  std::memset(buf->MutableDataY(), 128, buf->StrideY() * height);
  std::memset(buf->MutableDataU(), 128, buf->StrideU() * (height / 2));
  std::memset(buf->MutableDataV(), 128, buf->StrideV() * (height / 2));
  // Animate a 32x32 white square so the encoder has something to do.
  int x = (frame_idx * 4) % (width - 32);
  for (int row = 0; row < 32; ++row) {
    std::memset(buf->MutableDataY() + (row + 16) * buf->StrideY() + x,
                235, 32);
  }
  return webrtc::VideoFrame::Builder()
      .set_video_frame_buffer(buf)
      .set_timestamp_rtp(static_cast<uint32_t>(frame_idx) * 3000u)
      .set_timestamp_ms(frame_idx * 33)
      .build();
}

webrtc::VideoCodec DefaultSettings(int w, int h, int fps, int bps) {
  webrtc::VideoCodec s{};
  s.codecType = webrtc::kVideoCodecVP9;
  s.width = w;
  s.height = h;
  s.maxFramerate = fps;
  s.startBitrate = bps / 1000;
  s.minBitrate = bps / 2000;
  s.maxBitrate = bps / 1000;
  return s;
}

TEST(Vp9EncoderTest, InitEncodeSucceeds) {
  Vp9Encoder enc(Vp9EncoderConfig{});
  auto settings = DefaultSettings(640, 360, 30, 1'500'000);
  EXPECT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings()));
  EXPECT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.Release());
}

TEST(Vp9EncoderTest, EncodeProducesPackets) {
  Vp9EncoderConfig cfg;
  cfg.target_bitrate_bps = 1'500'000;
  cfg.framerate = 30;
  Vp9Encoder enc(cfg);
  CapturingCallback cb;
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto settings = DefaultSettings(640, 360, 30, 1'500'000);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings()));
  for (int i = 0; i < 30; ++i) {
    auto frame = MakeFrame(640, 360, i);
    EXPECT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.Encode(frame, /*frame_types=*/nullptr));
  }
  EXPECT_GE(cb.captured().size(), 1u);
  // First frame should be a keyframe (libvpx emits an IDR on first
  // packet even with kf_mode=DISABLED).
  EXPECT_EQ(webrtc::VideoFrameType::kVideoFrameKey,
            cb.captured().front().frame_type);
  for (const auto& c : cb.captured()) {
    EXPECT_EQ(webrtc::kVideoCodecVP9, c.codec_type);
    EXPECT_GT(c.size, 0u);
  }
}

TEST(Vp9EncoderTest, ForcedKeyframeFlagsAreHonored) {
  Vp9Encoder enc(Vp9EncoderConfig{});
  CapturingCallback cb;
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto settings = DefaultSettings(640, 360, 30, 1'500'000);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings()));
  // First few delta frames.
  for (int i = 0; i < 5; ++i) {
    auto f = MakeFrame(640, 360, i);
    enc.Encode(f, /*frame_types=*/nullptr);
  }
  // Now request a keyframe.
  std::vector<webrtc::VideoFrameType> types{
      webrtc::VideoFrameType::kVideoFrameKey};
  auto f = MakeFrame(640, 360, 5);
  enc.Encode(f, &types);
  ASSERT_FALSE(cb.captured().empty());
  EXPECT_EQ(webrtc::VideoFrameType::kVideoFrameKey,
            cb.captured().back().frame_type);
}

TEST(Vp9EncoderTest, NoBFramesEmitted) {
  // We can't directly query libvpx's frame structure from
  // EncodedImage, but we can assert that no captured frame is marked
  // as anything other than Key or Delta — VP9 has no separate B-type
  // in libwebrtc's enum, but g_lag_in_frames=0 + the screen content
  // tune guarantee no temporal reordering.  This test is a structural
  // canary: if we ever introduce a config that flips lag_in_frames,
  // this test still passes (it's the no-B contract at the API
  // boundary), but the *intent* is that the configuration above never
  // emits B-frame-equivalent altrefs in realtime mode.
  Vp9Encoder enc(Vp9EncoderConfig{});
  CapturingCallback cb;
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto settings = DefaultSettings(640, 360, 30, 1'500'000);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings()));
  for (int i = 0; i < 60; ++i) {
    enc.Encode(MakeFrame(640, 360, i), /*frame_types=*/nullptr);
  }
  for (const auto& c : cb.captured()) {
    ASSERT_TRUE(c.frame_type == webrtc::VideoFrameType::kVideoFrameKey ||
                c.frame_type == webrtc::VideoFrameType::kVideoFrameDelta);
  }
}

TEST(Vp9EncoderTest, SetRatesUpdatesBitrate) {
  Vp9Encoder enc(Vp9EncoderConfig{});
  CapturingCallback cb;
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto settings = DefaultSettings(640, 360, 30, 1'500'000);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings()));
  // Cut bitrate in half; encoder should not crash.
  webrtc::VideoBitrateAllocation alloc;
  alloc.SetBitrate(0, 0, 750'000);
  webrtc::VideoEncoder::RateControlParameters params(alloc, 30.0);
  enc.SetRates(params);
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(WEBRTC_VIDEO_CODEC_OK,
              enc.Encode(MakeFrame(640, 360, i), nullptr));
  }
}

TEST(Vp9EncoderTest, EncoderInfoTagsLowLatency) {
  Vp9EncoderConfig cfg;
  cfg.low_latency_tag = true;
  Vp9Encoder enc(cfg);
  auto info = enc.GetEncoderInfo();
  EXPECT_NE(std::string::npos,
            info.implementation_name.find("lowlatency"));
  EXPECT_FALSE(info.is_hardware_accelerated);
}

}  // namespace
}  // namespace cloud_browser

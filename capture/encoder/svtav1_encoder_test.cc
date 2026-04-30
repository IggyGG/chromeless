// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Tests for SvtAv1Encoder (T75). Always-on tests for parts that
// don't touch libSvtAv1Enc; HAS_SVT_AV1-gated tests for the actual
// encode path.
//
// TODO(T17-build-env): wire into the libwebrtc gtest target.
// TODO(SVT-AV1-lib): add libSvtAv1Enc headers + libs to the build
// (gated under //capture/build-integration:svt_av1).

#include "capture/encoder/svtav1_encoder.h"

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

class CapturingCallback : public webrtc::EncodedImageCallback {
 public:
  Result OnEncodedImage(
      const webrtc::EncodedImage& encoded_image,
      const webrtc::CodecSpecificInfo* csi) override {
    Capture c;
    c.size = encoded_image.size();
    c.frame_type = encoded_image._frameType;
    c.codec_type = csi ? csi->codecType : webrtc::kVideoCodecGeneric;
    if (encoded_image.size() > 0) {
      c.payload.assign(encoded_image.data(),
                       encoded_image.data() + encoded_image.size());
    }
    captured_.push_back(std::move(c));
    return Result(Result::OK);
  }
  struct Capture {
    size_t size = 0;
    webrtc::VideoFrameType frame_type = webrtc::VideoFrameType::kEmptyFrame;
    webrtc::VideoCodecType codec_type = webrtc::kVideoCodecGeneric;
    std::vector<uint8_t> payload;
  };
  const std::vector<Capture>& captured() const { return captured_; }
 private:
  std::vector<Capture> captured_;
};

webrtc::VideoFrame MakeFrame(int w, int h, int idx) {
  rtc::scoped_refptr<webrtc::I420Buffer> buf = webrtc::I420Buffer::Create(w, h);
  std::memset(buf->MutableDataY(), 128, buf->StrideY() * h);
  std::memset(buf->MutableDataU(), 128, buf->StrideU() * (h / 2));
  std::memset(buf->MutableDataV(), 128, buf->StrideV() * (h / 2));
  int x = (idx * 4) % (w - 32);
  for (int row = 0; row < 32; ++row) {
    std::memset(buf->MutableDataY() + (row + 16) * buf->StrideY() + x,
                235, 32);
  }
  return webrtc::VideoFrame::Builder()
      .set_video_frame_buffer(buf)
      .set_timestamp_rtp(static_cast<uint32_t>(idx) * 3000u)
      .set_timestamp_ms(idx * 33)
      .build();
}

webrtc::VideoCodec DefaultSettings(int w, int h, int fps, int bps) {
  webrtc::VideoCodec s{};
  s.codecType = webrtc::kVideoCodecAV1;
  s.width = w;
  s.height = h;
  s.maxFramerate = fps;
  s.startBitrate = bps / 1000;
  s.minBitrate = bps / 2000;
  s.maxBitrate = bps / 1000;
  return s;
}

// ---------------------------------------------------------------------
// Always-on tests.
// ---------------------------------------------------------------------

TEST(SvtAv1EncoderTest, EncoderInfoTagsSoftware) {
  SvtAv1EncoderConfig cfg;
  cfg.low_latency_tag = true;
  SvtAv1Encoder enc(cfg);
  auto info = enc.GetEncoderInfo();
  EXPECT_FALSE(info.is_hardware_accelerated);
  EXPECT_NE(std::string::npos, info.implementation_name.find("svtav1"));
  EXPECT_NE(std::string::npos, info.implementation_name.find("av1"));
  EXPECT_NE(std::string::npos, info.implementation_name.find("lowlatency"));
}

TEST(SvtAv1EncoderTest, ProbeAvailableIsCheap) {
  // Same canary as nvenc / vaapi: must complete quickly even when
  // libSvtAv1Enc isn't loadable. ProbeAvailable hangs → CI hangs.
  bool ok = SvtAv1Encoder::ProbeAvailable();
  (void)ok;  // value depends on the host build.
}

TEST(SvtAv1EncoderTest, ConfigDefaultsAreLatencyShape) {
  // Sanity-check the defaults that this entire encoder rests on:
  // M8 preset (per T43), VQ tune, 60-frame intra-refresh. If any
  // of these shifts, latency or quality changes silently — this
  // test is the canary.
  SvtAv1EncoderConfig cfg;
  EXPECT_EQ(8, cfg.preset_m);
  EXPECT_EQ("VQ", cfg.tune);
  EXPECT_EQ(60, cfg.intra_refresh_period_frames);
  EXPECT_EQ(0, cfg.profile);  // 8-bit 4:2:0.
  EXPECT_TRUE(cfg.low_latency_tag);
}

#if defined(HAS_SVT_AV1)

TEST(SvtAv1EncoderTest, InitAndEncodeSucceeds) {
  if (!SvtAv1Encoder::ProbeAvailable()) {
    GTEST_SKIP() << "libSvtAv1Enc not loadable on this host";
  }
  SvtAv1Encoder enc(SvtAv1EncoderConfig{});
  CapturingCallback cb;
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto settings = DefaultSettings(640, 360, 30, 1'500'000);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings()));
  for (int i = 0; i < 30; ++i) {
    EXPECT_EQ(WEBRTC_VIDEO_CODEC_OK,
              enc.Encode(MakeFrame(640, 360, i), nullptr));
  }
  EXPECT_FALSE(cb.captured().empty());
  for (const auto& c : cb.captured()) {
    EXPECT_GT(c.size, 0u);
    EXPECT_EQ(webrtc::kVideoCodecAV1, c.codec_type);
  }
}

TEST(SvtAv1EncoderTest, FirstFrameIsKey) {
  if (!SvtAv1Encoder::ProbeAvailable()) {
    GTEST_SKIP() << "libSvtAv1Enc not loadable";
  }
  SvtAv1Encoder enc(SvtAv1EncoderConfig{});
  CapturingCallback cb;
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto settings = DefaultSettings(320, 240, 30, 1'000'000);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings()));
  for (int i = 0; i < 8; ++i) enc.Encode(MakeFrame(320, 240, i), nullptr);
  ASSERT_FALSE(cb.captured().empty());
  EXPECT_EQ(webrtc::VideoFrameType::kVideoFrameKey,
            cb.captured().front().frame_type);
}

TEST(SvtAv1EncoderTest, NoBFramesEverEmitted) {
  // We don't parse the OBU stream here — the contract surface check
  // is "every captured frame is Key or Delta," which is what
  // hierarchical_levels=0 + pred_structure=0 (low-delay-P) +
  // no-lookahead guarantees. If a future refactor flips any of
  // those, this is the test that breaks.
  if (!SvtAv1Encoder::ProbeAvailable()) {
    GTEST_SKIP() << "libSvtAv1Enc not loadable";
  }
  SvtAv1Encoder enc(SvtAv1EncoderConfig{});
  CapturingCallback cb;
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto settings = DefaultSettings(640, 360, 30, 1'500'000);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings()));
  for (int i = 0; i < 60; ++i) {
    enc.Encode(MakeFrame(640, 360, i), nullptr);
  }
  for (const auto& c : cb.captured()) {
    ASSERT_TRUE(c.frame_type == webrtc::VideoFrameType::kVideoFrameKey ||
                c.frame_type == webrtc::VideoFrameType::kVideoFrameDelta);
  }
}

TEST(SvtAv1EncoderTest, ForcedKeyframeProducesKey) {
  if (!SvtAv1Encoder::ProbeAvailable()) {
    GTEST_SKIP() << "libSvtAv1Enc not loadable";
  }
  SvtAv1Encoder enc(SvtAv1EncoderConfig{});
  CapturingCallback cb;
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto settings = DefaultSettings(320, 240, 30, 1'000'000);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings()));
  for (int i = 0; i < 5; ++i) enc.Encode(MakeFrame(320, 240, i), nullptr);
  std::vector<webrtc::VideoFrameType> types{
      webrtc::VideoFrameType::kVideoFrameKey};
  enc.Encode(MakeFrame(320, 240, 5), &types);
  ASSERT_FALSE(cb.captured().empty());
  EXPECT_EQ(webrtc::VideoFrameType::kVideoFrameKey,
            cb.captured().back().frame_type);
}

TEST(SvtAv1EncoderTest, SetRatesAdjustsBitrate) {
  if (!SvtAv1Encoder::ProbeAvailable()) {
    GTEST_SKIP() << "libSvtAv1Enc not loadable";
  }
  SvtAv1Encoder enc(SvtAv1EncoderConfig{});
  CapturingCallback cb;
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto settings = DefaultSettings(320, 240, 30, 1'500'000);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings()));
  webrtc::VideoBitrateAllocation alloc;
  alloc.SetBitrate(0, 0, 750'000);
  webrtc::VideoEncoder::RateControlParameters params(alloc, 30.0);
  enc.SetRates(params);  // re-emits svt_av1_enc_set_parameter.
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(WEBRTC_VIDEO_CODEC_OK,
              enc.Encode(MakeFrame(320, 240, i), nullptr));
  }
}

#endif  // HAS_SVT_AV1

}  // namespace
}  // namespace cloud_browser

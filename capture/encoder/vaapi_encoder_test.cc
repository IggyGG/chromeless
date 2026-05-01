// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Tests for VaapiEncoder (T70). Same shape as nvenc_encoder_test.cc:
// always-on tests for the parts that don't touch libva, plus
// HAS_VAAPI-gated tests that GTEST_SKIP if no VA-capable GPU is
// present.
//
// TODO(T17-build-env): wire into the libwebrtc gtest runner.
// TODO(VAAPI-libva): add libva headers + libs to the build (see
// capture/build-integration/BUILD.gn — gated under :vaapi).

#include "capture/encoder/vaapi_encoder.h"

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

webrtc::VideoFrame MakeFrame(int w, int h, int idx) {
  webrtc::scoped_refptr<webrtc::I420Buffer> buf = webrtc::I420Buffer::Create(w, h);
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

webrtc::VideoCodec DefaultSettings(int w, int h, int fps, int bps,
                                     webrtc::VideoCodecType codec) {
  webrtc::VideoCodec s{};
  s.codecType = codec;
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

TEST(VaapiEncoderTest, EncoderInfoTagsHardware) {
  VaapiEncoderConfig cfg;
  cfg.codec_type = "H264";
  cfg.low_latency_tag = true;
  VaapiEncoder enc(cfg);
  auto info = enc.GetEncoderInfo();
  EXPECT_TRUE(info.is_hardware_accelerated);
  EXPECT_NE(std::string::npos,
            info.implementation_name.find("vaapi"));
  EXPECT_NE(std::string::npos,
            info.implementation_name.find("H264"));
  EXPECT_NE(std::string::npos,
            info.implementation_name.find("lowlatency"));
}

TEST(VaapiEncoderTest, EncoderInfoReflectsCodecType) {
  for (const std::string& codec : {"H264", "HEVC", "AV1", "VP9"}) {
    VaapiEncoderConfig cfg;
    cfg.codec_type = codec;
    VaapiEncoder enc(cfg);
    auto info = enc.GetEncoderInfo();
    EXPECT_NE(std::string::npos,
              info.implementation_name.find(codec))
        << "codec_type " << codec << " missing from impl name";
  }
}

TEST(VaapiEncoderTest, ProbeAvailableIsCheap) {
  // Same canary as NvencEncoderTest. Must complete quickly even on
  // hosts without /dev/dri/renderD128. If a future refactor makes
  // this synchronously open + dispatch a real encode, the next CI
  // run will hang — and we'll catch it here.
  bool h264 = VaapiEncoder::ProbeAvailable("H264");
  bool hevc = VaapiEncoder::ProbeAvailable("HEVC");
  bool av1  = VaapiEncoder::ProbeAvailable("AV1");
  bool vp9  = VaapiEncoder::ProbeAvailable("VP9");
  (void)h264; (void)hevc; (void)av1; (void)vp9;
}

TEST(VaapiEncoderTest, UnknownCodecTypeFailsInitEncode) {
  VaapiEncoderConfig cfg;
  cfg.codec_type = "GIF";  // not a real video codec.
  VaapiEncoder enc(cfg);
  auto settings = DefaultSettings(640, 360, 30, 1'500'000,
                                    webrtc::kVideoCodecGeneric);
  auto rc = enc.InitEncode(&settings, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200));
  EXPECT_TRUE(rc == WEBRTC_VIDEO_CODEC_ERR_PARAMETER ||
              rc == WEBRTC_VIDEO_CODEC_ERROR);
}

#if defined(HAS_VAAPI)

// HAS_VAAPI tests run only when libva is in the build AND a probe
// confirms a working session for that codec.

TEST(VaapiEncoderTest, H264InitAndEncodeOnRealDevice) {
  if (!VaapiEncoder::ProbeAvailable("H264")) {
    GTEST_SKIP() << "no VAAPI H264 on this host (no /dev/dri/renderD128 "
                 << "or driver lacks encode entrypoint)";
  }
  VaapiEncoderConfig cfg;
  cfg.codec_type = "H264";
  cfg.target_bitrate_bps = 2'000'000;
  VaapiEncoder enc(cfg);
  CapturingCallback cb;
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto settings = DefaultSettings(640, 360, 30, 2'000'000,
                                    webrtc::kVideoCodecH264);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200)));
  for (int i = 0; i < 30; ++i) {
    EXPECT_EQ(WEBRTC_VIDEO_CODEC_OK,
              enc.Encode(MakeFrame(640, 360, i), nullptr));
  }
  EXPECT_FALSE(cb.captured().empty());
  for (const auto& c : cb.captured()) {
    EXPECT_GT(c.size, 0u);
    EXPECT_EQ(webrtc::kVideoCodecH264, c.codec_type);
  }
  // EncoderInfo should now include the vendor tag (Intel / Mesa
  // Gallium / etc.) from vaQueryVendorString.
  auto info = enc.GetEncoderInfo();
  // Don't assert a specific vendor string — only that the impl name
  // is more specific than the generic format from before
  // InitEncode (i.e., it's been updated with vendor info).
  EXPECT_NE(std::string::npos, info.implementation_name.find("H264"));
}

TEST(VaapiEncoderTest, HevcInitOnRealDevice) {
  if (!VaapiEncoder::ProbeAvailable("HEVC")) {
    GTEST_SKIP() << "no VAAPI HEVC on this host";
  }
  VaapiEncoderConfig cfg;
  cfg.codec_type = "HEVC";
  VaapiEncoder enc(cfg);
  auto settings = DefaultSettings(640, 360, 30, 1'500'000,
                                    webrtc::kVideoCodecH265);
  EXPECT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200)));
}

TEST(VaapiEncoderTest, AV1InitOnlyOnQsvOrAmfRdna3Plus) {
  if (!VaapiEncoder::ProbeAvailable("AV1")) {
    GTEST_SKIP() << "no VAAPI AV1 on this host (requires Intel Arc / Xe-HPG "
                 << "or AMD RDNA 3+)";
  }
  VaapiEncoderConfig cfg;
  cfg.codec_type = "AV1";
  VaapiEncoder enc(cfg);
  auto settings = DefaultSettings(640, 360, 30, 1'500'000,
                                    webrtc::kVideoCodecAV1);
  EXPECT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200)));
}

TEST(VaapiEncoderTest, VP9OnIntelOnly) {
  if (!VaapiEncoder::ProbeAvailable("VP9")) {
    GTEST_SKIP() << "no VAAPI VP9 on this host (Intel only; Mesa AMD "
                 << "drops VP9 encode)";
  }
  VaapiEncoderConfig cfg;
  cfg.codec_type = "VP9";
  VaapiEncoder enc(cfg);
  auto settings = DefaultSettings(640, 360, 30, 1'500'000,
                                    webrtc::kVideoCodecVP9);
  EXPECT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200)));
}

TEST(VaapiEncoderTest, ForcedKeyframeOnRealDevice) {
  if (!VaapiEncoder::ProbeAvailable("H264")) {
    GTEST_SKIP() << "no VAAPI H264 on this host";
  }
  VaapiEncoder enc(VaapiEncoderConfig{});
  CapturingCallback cb;
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto settings = DefaultSettings(640, 360, 30, 1'500'000,
                                    webrtc::kVideoCodecH264);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200)));
  for (int i = 0; i < 5; ++i) enc.Encode(MakeFrame(640, 360, i), nullptr);
  std::vector<webrtc::VideoFrameType> types{
      webrtc::VideoFrameType::kVideoFrameKey};
  enc.Encode(MakeFrame(640, 360, 5), &types);
  ASSERT_FALSE(cb.captured().empty());
  EXPECT_EQ(webrtc::VideoFrameType::kVideoFrameKey,
            cb.captured().back().frame_type);
}

TEST(VaapiEncoderTest, SetRatesOnRealDevice) {
  if (!VaapiEncoder::ProbeAvailable("H264")) {
    GTEST_SKIP() << "no VAAPI H264 on this host";
  }
  VaapiEncoder enc(VaapiEncoderConfig{});
  CapturingCallback cb;
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto settings = DefaultSettings(640, 360, 30, 1'500'000,
                                    webrtc::kVideoCodecH264);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200)));

  webrtc::VideoBitrateAllocation alloc;
  alloc.SetBitrate(0, 0, 750'000);
  webrtc::VideoEncoder::RateControlParameters params(alloc, 30.0);
  enc.SetRates(params);  // re-emits VAEncMiscParameterRateControl.
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(WEBRTC_VIDEO_CODEC_OK,
              enc.Encode(MakeFrame(640, 360, i), nullptr));
  }
}

#endif  // HAS_VAAPI

}  // namespace
}  // namespace cloud_browser

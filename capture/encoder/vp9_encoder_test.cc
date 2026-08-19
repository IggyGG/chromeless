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
      c.end_of_picture = codec_specific_info->end_of_picture;
      if (codec_specific_info->codecType == webrtc::kVideoCodecVP9) {
        const webrtc::CodecSpecificInfoVP9& vp9 =
            codec_specific_info->codecSpecific.VP9;
        c.vp9_flexible_mode = vp9.flexible_mode;
        c.vp9_temporal_idx = vp9.temporal_idx;
        c.vp9_num_spatial_layers = vp9.num_spatial_layers;
        c.vp9_first_active_layer = vp9.first_active_layer;
        c.vp9_first_frame_in_picture = vp9.first_frame_in_picture;
        c.vp9_spatial_layer_resolution_present =
            vp9.spatial_layer_resolution_present;
        c.vp9_ss_data_available = vp9.ss_data_available;
        c.vp9_inter_pic_predicted = vp9.inter_pic_predicted;
        c.vp9_width0 = vp9.width[0];
        c.vp9_height0 = vp9.height[0];
      }
    }
    captured_.push_back(c);
    return Result(Result::OK);
  }

  struct Capture {
    size_t size = 0;
    webrtc::VideoFrameType frame_type = webrtc::VideoFrameType::kEmptyFrame;
    webrtc::VideoCodecType codec_type = webrtc::kVideoCodecGeneric;
    bool end_of_picture = false;
    bool vp9_flexible_mode = true;
    int vp9_temporal_idx = 0;
    size_t vp9_num_spatial_layers = 0;
    size_t vp9_first_active_layer = 0;
    bool vp9_first_frame_in_picture = false;
    bool vp9_spatial_layer_resolution_present = false;
    bool vp9_ss_data_available = false;
    bool vp9_inter_pic_predicted = false;
    size_t vp9_width0 = 0;
    size_t vp9_height0 = 0;
  };
  const std::vector<Capture>& captured() const { return captured_; }

 private:
  std::vector<Capture> captured_;
};

// Build a single I420 VideoFrame filled with a mid-grey + colored
// rectangle so successive frames have measurable inter-frame deltas.
webrtc::VideoFrame MakeFrame(int width, int height, int frame_idx) {
  webrtc::scoped_refptr<webrtc::I420Buffer> buf =
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
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200)));
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
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200)));
  for (int i = 0; i < 30; ++i) {
    auto frame = MakeFrame(640, 360, i);
    EXPECT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.Encode(frame, /*frame_types=*/nullptr));
  }
  EXPECT_GE(cb.captured().size(), 1u);
  // First frame should be a keyframe (libvpx emits an IDR on first
  // packet even with kf_mode=DISABLED).
  EXPECT_EQ(webrtc::VideoFrameType::kVideoFrameKey,
            cb.captured().front().frame_type);
  const auto& first = cb.captured().front();
  EXPECT_TRUE(first.end_of_picture);
  EXPECT_FALSE(first.vp9_flexible_mode);
  EXPECT_EQ(webrtc::kNoTemporalIdx, first.vp9_temporal_idx);
  EXPECT_EQ(1u, first.vp9_num_spatial_layers);
  EXPECT_EQ(0u, first.vp9_first_active_layer);
  EXPECT_TRUE(first.vp9_first_frame_in_picture);
  EXPECT_TRUE(first.vp9_ss_data_available);
  EXPECT_TRUE(first.vp9_spatial_layer_resolution_present);
  EXPECT_EQ(640u, first.vp9_width0);
  EXPECT_EQ(360u, first.vp9_height0);
  for (const auto& c : cb.captured()) {
    EXPECT_EQ(webrtc::kVideoCodecVP9, c.codec_type);
    EXPECT_GT(c.size, 0u);
    EXPECT_TRUE(c.end_of_picture);
    EXPECT_FALSE(c.vp9_flexible_mode);
    EXPECT_EQ(webrtc::kNoTemporalIdx, c.vp9_temporal_idx);
    EXPECT_EQ(1u, c.vp9_num_spatial_layers);
    EXPECT_TRUE(c.vp9_first_frame_in_picture);
  }
}

// Mid-session resolution change — the exact cycle libwebrtc drives when the
// captured surface is resized (client-driven viewport resize).
//
// The contract is NOT ours to choose; it is fixed by VideoStreamEncoder::
// ReconfigureEncoder (third_party/webrtc/video/video_stream_encoder.cc at
// branch-heads/7727, :1398-1416): a width/height delta makes
// RequiresEncoderReset return true, which drives, in this order,
//
//     ReleaseEncoder()  ->  InitEncode(new size)  ->  RegisterEncodeCompleteCallback()
//
// Two properties of this encoder depend on that ordering and are easy to
// break by "tidying" either method:
//
//   * Release() nulls callback_. That is only safe because libwebrtc
//     re-registers after every successful InitEncode. If InitEncode is ever
//     changed to preserve callback_, or Release stops nulling it, this test
//     still passes -- but if someone "fixes" Release to keep the callback AND
//     a caller re-inits without re-registering, Encode's !callback_ guard
//     (vp9_encoder.cc) silently drops every frame instead of crashing.
//   * InitEncode does NOT call Release() first. Safe only because
//     ReleaseEncoder always precedes it. Calling InitEncode twice with no
//     Release in between would leak the vpx codec ctx and the vpx_image.
//
// So: replay the real sequence, and assert the encoder actually produces
// output at the NEW geometry. A crash here means mid-session resize crashes
// the guest; empty output means resize silently freezes the stream.
TEST(Vp9EncoderTest, ReinitAtNewResolutionProducesFramesAtNewGeometry) {
  Vp9EncoderConfig cfg;
  cfg.target_bitrate_bps = 1'500'000;
  cfg.framerate = 30;
  Vp9Encoder enc(cfg);
  CapturingCallback cb;
  const webrtc::VideoEncoder::Settings kSettings(
      webrtc::VideoEncoder::Capabilities(false), 1, 1200);

  // ── Session 1: 640x360 ──
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto small = DefaultSettings(640, 360, 30, 1'500'000);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.InitEncode(&small, kSettings));
  for (int i = 0; i < 10; ++i) {
    auto frame = MakeFrame(640, 360, i);
    ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.Encode(frame, nullptr));
  }
  ASSERT_GE(cb.captured().size(), 1u);
  EXPECT_EQ(640u, cb.captured().front().vp9_width0);
  EXPECT_EQ(360u, cb.captured().front().vp9_height0);

  // ── The resize: libwebrtc's exact order. Note Release() nulls callback_,
  //    so the re-register below is load-bearing, not ceremony. ──
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.Release());
  auto large = DefaultSettings(1280, 720, 30, 1'500'000);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.InitEncode(&large, kSettings));
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));

  // ── Session 2: 1280x720. Output must resume at the NEW geometry. ──
  const size_t before = cb.captured().size();
  for (int i = 0; i < 10; ++i) {
    auto frame = MakeFrame(1280, 720, i);
    ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.Encode(frame, nullptr));
  }
  ASSERT_GT(cb.captured().size(), before)
      << "no output after re-init — resize freezes the stream";
  const auto& first_after = cb.captured()[before];
  EXPECT_EQ(webrtc::VideoFrameType::kVideoFrameKey, first_after.frame_type)
      << "first frame after re-init must be a keyframe or the receiver "
         "cannot decode the new resolution";
  EXPECT_EQ(1280u, first_after.vp9_width0);
  EXPECT_EQ(720u, first_after.vp9_height0);

  // Release twice: the second is a no-op, matching ReleaseEncoder's own
  // encoder_initialized_ guard. Must not double-free the vpx ctx/image.
  EXPECT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.Release());
  EXPECT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.Release());
}

TEST(Vp9EncoderTest, ForcedKeyframeFlagsAreHonored) {
  Vp9Encoder enc(Vp9EncoderConfig{});
  CapturingCallback cb;
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto settings = DefaultSettings(640, 360, 30, 1'500'000);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200)));
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
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200)));
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
            enc.InitEncode(&settings, webrtc::VideoEncoder::Settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200)));
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

// ── The crash the external-reinit test above CANNOT catch ────────────────
//
// See the twin in h264_encoder_test.cc for the full story. Short version:
// the test above re-registers the callback by hand (libwebrtc's order), so
// it never exercises Encode()'s SELF-reinit path. That path calls
// Release() — which nulls callback_ — after the entry guard has already
// been passed, then dereferences it. Null deref, browser process dead, on
// the first frame at a new geometry.
//
// Latent until Cb.setViewport made the capturer's resolution mutable; the
// capturer pinned min==max at 1280x720, so no frame ever changed size.
TEST(Vp9EncoderTest, SelfReinitOnFrameSizeChangeKeepsCallbackRegistered) {
  Vp9Encoder enc(Vp9EncoderConfig{});
  CapturingCallback cb;
  const webrtc::VideoEncoder::Settings kSettings(
      webrtc::VideoEncoder::Capabilities(false), 1, 1200);

  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.RegisterEncodeCompleteCallback(&cb));
  auto small = DefaultSettings(640, 360, 30, 1'500'000);
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.InitEncode(&small, kSettings));
  for (int i = 0; i < 10; ++i) {
    ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.Encode(MakeFrame(640, 360, i),
                                                nullptr));
  }
  const size_t before = cb.captured().size();
  ASSERT_GT(before, 0u);

  // Larger frame, no external Release/InitEncode, no re-registration.
  for (int i = 0; i < 10; ++i) {
    ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.Encode(MakeFrame(1280, 720, i),
                                                nullptr));
  }

  ASSERT_GT(cb.captured().size(), before)
      << "self-reinit produced no output — the callback was lost across "
         "the internal Release(), so every post-resize frame is dropped "
         "(and before the fix, this line was preceded by a SIGSEGV)";
  const auto& first_after = cb.captured()[before];
  EXPECT_EQ(webrtc::VideoFrameType::kVideoFrameKey, first_after.frame_type)
      << "first frame after self-reinit must be a keyframe";
  EXPECT_EQ(1280u, first_after.vp9_width0);
  EXPECT_EQ(720u, first_after.vp9_height0);

  // And back down: shrinking must work too (the user can drag smaller).
  const size_t before_shrink = cb.captured().size();
  for (int i = 0; i < 10; ++i) {
    ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, enc.Encode(MakeFrame(640, 360, i),
                                                nullptr));
  }
  ASSERT_GT(cb.captured().size(), before_shrink)
      << "self-reinit on shrink produced no output";
  EXPECT_EQ(640u, cb.captured()[before_shrink].vp9_width0);
}

}  // namespace
}  // namespace cloud_browser

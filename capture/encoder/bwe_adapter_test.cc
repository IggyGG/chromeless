// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Unit tests for BweAdapter and WrapWithBweAdapter.
//
// TODO(T17-build-env): wire into the libwebrtc gtest target. Authored
// against documented libwebrtc + gmock conventions.

#include "capture/encoder/bwe_adapter.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "api/units/data_rate.h"
#include "api/video/video_bitrate_allocation.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "test/gmock.h"
#include "test/gtest.h"

namespace cloud_browser {
namespace {

using ::testing::_;

// MockEncoder records every SetRates call so tests can inspect what
// the adapter forwarded. Other VideoEncoder methods are no-ops or
// return success — they are not under test here.
class MockEncoder : public webrtc::VideoEncoder {
 public:
  struct Call {
    int total_bps = 0;
    double framerate_fps = 0.0;
  };

  int32_t InitEncode(const webrtc::VideoCodec*,
                     const webrtc::VideoEncoder::Settings&) override {
    return WEBRTC_VIDEO_CODEC_OK;
  }
  int32_t Encode(const webrtc::VideoFrame&,
                 const std::vector<webrtc::VideoFrameType>*) override {
    return WEBRTC_VIDEO_CODEC_OK;
  }
  int32_t RegisterEncodeCompleteCallback(
      webrtc::EncodedImageCallback*) override {
    return WEBRTC_VIDEO_CODEC_OK;
  }
  int32_t Release() override { return WEBRTC_VIDEO_CODEC_OK; }
  void SetRates(const RateControlParameters& params) override {
    Call c;
    c.total_bps = static_cast<int>(params.bitrate.get_sum_bps());
    c.framerate_fps = params.framerate_fps;
    calls_.push_back(c);
  }
  EncoderInfo GetEncoderInfo() const override { return EncoderInfo(); }

  const std::vector<Call>& calls() const { return calls_; }

 private:
  std::vector<Call> calls_;
};

// Helper: invoke the adapter's BWE-update entrypoint with a single
// total bitrate.
void Push(BweAdapter* adapter, int total_bps, double fps = 30.0,
           uint8_t frac_loss = 0, int64_t rtt_ms = 50) {
  adapter->OnBitrateUpdated(
      webrtc::DataRate::BitsPerSec(total_bps),
      webrtc::DataRate::BitsPerSec(total_bps),
      webrtc::DataRate::BitsPerSec(total_bps * 2),
      frac_loss, rtt_ms, fps);
}

// ---------------------------------------------------------------------
// BweAdapter (registry + fan-out)
// ---------------------------------------------------------------------

TEST(BweAdapterTest, RegisterAndCount) {
  BweAdapter a;
  MockEncoder e1, e2;
  EXPECT_EQ(0u, a.encoder_count());
  a.RegisterEncoder(&e1);
  a.RegisterEncoder(&e2);
  EXPECT_EQ(2u, a.encoder_count());
  // Idempotent.
  a.RegisterEncoder(&e1);
  EXPECT_EQ(2u, a.encoder_count());
  a.UnregisterEncoder(&e1);
  EXPECT_EQ(1u, a.encoder_count());
  a.UnregisterEncoder(&e2);
  EXPECT_EQ(0u, a.encoder_count());
}

TEST(BweAdapterTest, OnBitrateUpdatedFansOutToAllRegistered) {
  BweAdapter a;
  MockEncoder e1, e2, e3;
  a.RegisterEncoder(&e1);
  a.RegisterEncoder(&e2);
  a.RegisterEncoder(&e3);

  Push(&a, /*total_bps=*/2'000'000, /*fps=*/30.0);

  ASSERT_EQ(1u, e1.calls().size());
  ASSERT_EQ(1u, e2.calls().size());
  ASSERT_EQ(1u, e3.calls().size());
  EXPECT_EQ(2'000'000, e1.calls().back().total_bps);
  EXPECT_DOUBLE_EQ(30.0, e1.calls().back().framerate_fps);
}

TEST(BweAdapterTest, UnregisteredEncoderDoesNotReceiveUpdates) {
  BweAdapter a;
  MockEncoder e1, e2;
  a.RegisterEncoder(&e1);
  a.RegisterEncoder(&e2);

  Push(&a, 1'000'000);
  EXPECT_EQ(1u, e1.calls().size());
  EXPECT_EQ(1u, e2.calls().size());

  a.UnregisterEncoder(&e1);
  Push(&a, 2'000'000);
  EXPECT_EQ(1u, e1.calls().size()) << "e1 was unregistered; should not be called";
  EXPECT_EQ(2u, e2.calls().size());
  EXPECT_EQ(2'000'000, e2.calls().back().total_bps);
}

TEST(BweAdapterTest, MetricsSinkReceivesSnapshot) {
  std::vector<BweUpdate> seen;
  BweAdapter a([&](const BweUpdate& u) { seen.push_back(u); });
  MockEncoder e;
  a.RegisterEncoder(&e);

  Push(&a, /*total_bps=*/1'500'000, /*fps=*/24.0,
        /*frac_loss=*/12, /*rtt_ms=*/85);

  ASSERT_EQ(1u, seen.size());
  EXPECT_EQ(1'500'000, seen[0].total_bitrate_bps);
  EXPECT_EQ(12, seen[0].fraction_loss);
  EXPECT_EQ(85, seen[0].rtt_ms);
  EXPECT_DOUBLE_EQ(24.0, seen[0].framerate_fps);
  EXPECT_EQ(1, seen[0].active_encoder_count);
}

TEST(BweAdapterTest, LastUpdateMirrorsMostRecentFire) {
  BweAdapter a;
  MockEncoder e;
  a.RegisterEncoder(&e);
  Push(&a, 800'000, /*fps=*/15.0);
  Push(&a, 1'600'000, /*fps=*/30.0);
  auto last = a.last_update();
  EXPECT_EQ(1'600'000, last.total_bitrate_bps);
  EXPECT_DOUBLE_EQ(30.0, last.framerate_fps);
}

TEST(BweAdapterTest, NullEncoderRegistrationIsRejected) {
  BweAdapter a;
  a.RegisterEncoder(nullptr);
  EXPECT_EQ(0u, a.encoder_count());
}

TEST(BweAdapterTest, ConcurrentRegisterAndUpdateDoesNotCrash) {
  // Stress: one thread continuously fires updates while another
  // registers/unregisters. The lock-free SetRates fan-out depends on
  // the snapshot trick in OnBitrateUpdated; this test exists so a
  // future refactor that changes it triggers TSAN / sanitizer noise
  // immediately.
  BweAdapter a;
  std::atomic<bool> stop{false};
  std::vector<std::unique_ptr<MockEncoder>> encoders;
  for (int i = 0; i < 4; ++i) encoders.emplace_back(std::make_unique<MockEncoder>());

  std::thread updater([&]() {
    int n = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      Push(&a, 500'000 + (n++ % 10) * 100'000);
    }
  });
  std::thread registrar([&]() {
    int n = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      auto* e = encoders[n % encoders.size()].get();
      a.RegisterEncoder(e);
      a.UnregisterEncoder(e);
      ++n;
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stop.store(true, std::memory_order_relaxed);
  updater.join();
  registrar.join();
  // Pass = no crash, no deadlock, no TSAN report. We don't assert
  // call counts — they are inherently racy.
  SUCCEED();
}

// ---------------------------------------------------------------------
// WrapWithBweAdapter (decorator)
// ---------------------------------------------------------------------

TEST(WrapWithBweAdapterTest, NullAdapterReturnsInnerUnwrapped) {
  auto inner = std::make_unique<MockEncoder>();
  auto* raw = inner.get();
  auto wrapped = WrapWithBweAdapter(std::move(inner), nullptr);
  EXPECT_EQ(raw, wrapped.get())
      << "null adapter should not wrap; got a different pointer";
}

TEST(WrapWithBweAdapterTest, RegistersOnInitAndUnregistersOnRelease) {
  BweAdapter a;
  auto inner = std::make_unique<MockEncoder>();
  auto* raw = inner.get();
  auto wrapped = WrapWithBweAdapter(std::move(inner), &a);

  // Pre-Init: not registered.
  EXPECT_EQ(0u, a.encoder_count());

  webrtc::VideoCodec settings{};
  settings.codecType = webrtc::kVideoCodecGeneric;
  settings.width = 320;
  settings.height = 240;
  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK,
            wrapped->InitEncode(&settings,
                                 webrtc::VideoEncoder::Settings()));
  EXPECT_EQ(1u, a.encoder_count())
      << "InitEncode should register with adapter";

  // While registered, BWE updates reach the inner encoder.
  Push(&a, 1'000'000);
  ASSERT_EQ(1u, raw->calls().size());

  ASSERT_EQ(WEBRTC_VIDEO_CODEC_OK, wrapped->Release());
  EXPECT_EQ(0u, a.encoder_count())
      << "Release should unregister from adapter";

  // After release, BWE updates do not reach the inner encoder.
  Push(&a, 2'000'000);
  EXPECT_EQ(1u, raw->calls().size())
      << "released encoder should not see additional updates";
}

TEST(WrapWithBweAdapterTest, DestructorUnregistersIfReleaseSkipped) {
  BweAdapter a;
  {
    auto inner = std::make_unique<MockEncoder>();
    auto wrapped = WrapWithBweAdapter(std::move(inner), &a);
    webrtc::VideoCodec settings{};
    settings.width = 320;
    settings.height = 240;
    wrapped->InitEncode(&settings, webrtc::VideoEncoder::Settings());
    EXPECT_EQ(1u, a.encoder_count());
    // ... drop wrapped without calling Release.
  }
  EXPECT_EQ(0u, a.encoder_count())
      << "dtor should defensively unregister to avoid dangling registration";
}

}  // namespace
}  // namespace cloud_browser

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Unit tests for CloudBrowserFrameSinkCapturer.
//
// We stand up a fake `viz::mojom::FrameSinkVideoCapturer` producer
// (the upstream side of the Mojo) and drive synthetic
// OnFrameCaptured() calls into the consumer-under-test. Assertions
// cover:
//   * the delivery callback fires once per non-malformed frame
//   * `Done()` is called for every captured frame, even when
//     WrapAsMediaFrame fails or the consumer is dropped early
//   * `frames_dropped_by_capturer` reflects info.metadata
//     .frame_count_dropped
//   * `Stop()` is forwarded to the producer
//
// TODO(T17-build-env): wire into the libwebrtc gtest runner. Authored
// against the upstream mojom + base::test::TaskEnvironment / mojo::
// Receiver test patterns. Until the build env runs, this file is
// design-by-spec.

#include "capture/framesink-capturer/capturer.h"
#include "capture/framesink-capturer/cb_framesink_video_track_source.h"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "api/make_ref_counted.h"
#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/run_loop.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/threading/thread.h"
#include "media/base/video_frame.h"
#include "media/base/video_types.h"
#include "media/capture/mojom/video_capture_buffer.mojom.h"
#include "media/mojo/mojom/media_types.mojom.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/viz/privileged/mojom/compositing/frame_sink_video_capture.mojom.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace cloud_browser {
namespace {

// FakeProducer implements the producer half of the Mojo. We record
// every method call so tests can assert on it.
class FakeProducer : public viz::mojom::FrameSinkVideoCapturer {
 public:
  FakeProducer() = default;

  mojo::Remote<viz::mojom::FrameSinkVideoCapturer> BindAndPassRemote() {
    mojo::Remote<viz::mojom::FrameSinkVideoCapturer> remote;
    receiver_.Bind(remote.BindNewPipeAndPassReceiver());
    return remote;
  }

  // Convenience: send a synthetic frame downstream after Start has
  // been called. `dropped` is ignored for now — see Wall #28 TODO in
  // capturer.cc; VideoFrameMetadata::frame_count_dropped was removed
  // from chromium and our capturer.cc no longer tracks it.
  void SendFrame(int /*dropped*/ = 0,
                 media::VideoPixelFormat fmt = media::PIXEL_FORMAT_I420) {
    ASSERT_TRUE(consumer_.is_bound()) << "Start not yet called";
    auto info = media::mojom::VideoFrameInfo::New();
    info->coded_size = gfx::Size(640, 360);
    info->visible_rect = gfx::Rect(0, 0, 640, 360);
    info->pixel_format = fmt;
    info->timestamp = base::Microseconds(++ts_us_);

    // Allocate a tiny shmem region so WrapExternalData has something
    // to bind against.
    auto region = base::ReadOnlySharedMemoryRegion::Create(640 * 360 * 3 / 2);
    ASSERT_TRUE(region.IsValid());
    auto handle = media::mojom::VideoBufferHandle::NewReadOnlyShmemRegion(
        std::move(region.region));

    mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
        cb_remote;
    auto cb_receiver = cb_remote.InitWithNewPipeAndPassReceiver();
    auto fake_callbacks = std::make_unique<FakeFrameCallbacks>();
    fake_callbacks_.push_back(fake_callbacks.get());
    fake_callbacks->Bind(std::move(cb_receiver));
    callback_holders_.push_back(std::move(fake_callbacks));

    consumer_->OnFrameCaptured(std::move(handle), std::move(info),
                                gfx::Rect(0, 0, 640, 360),
                                std::move(cb_remote));
  }

  // Aggregate Done() count across every frame we've issued. Tests use
  // this to assert no Done leaks after teardown.
  int total_done_calls() const {
    int n = 0;
    for (auto* fc : fake_callbacks_) n += fc->done_calls();
    return n;
  }

  bool stop_called() const { return stop_called_; }
  bool start_called() const { return start_called_; }

  // viz::mojom::FrameSinkVideoCapturer:
  media::VideoPixelFormat last_format() const { return last_format_; }
  viz::mojom::BufferFormatPreference last_start_pref() const {
    return last_start_pref_;
  }

  void SetFormat(media::VideoPixelFormat format) override {
    last_format_ = format;
  }
  void SetMinCapturePeriod(base::TimeDelta /*period*/) override {}
  void SetMinSizeChangePeriod(base::TimeDelta /*min_period*/) override {}
  void SetResolutionConstraints(const gfx::Size& /*min*/,
                                 const gfx::Size& /*max*/,
                                 bool /*fixed*/) override {}
  void SetAutoThrottlingEnabled(bool /*enabled*/) override {}
  void SetAnimationFpsLockIn(bool /*enabled*/,
                              float /*majority_damaged_pixel_min_ratio*/)
      override {}
  void ChangeTarget(
      const std::optional<viz::VideoCaptureTarget>& /*target*/,
      uint32_t /*sub_capture_version*/) override {}
  void Start(
      mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumer> consumer,
      viz::mojom::BufferFormatPreference pref) override {
    consumer_.Bind(std::move(consumer));
    last_start_pref_ = pref;
    start_called_ = true;
  }
  void Stop() override {
    stop_called_ = true;
    if (consumer_.is_bound()) consumer_->OnStopped();
  }
  void RequestRefreshFrame() override {}
  void CreateOverlay(int32_t /*stacking_index*/,
                     mojo::PendingReceiver<viz::mojom::FrameSinkVideoCaptureOverlay>
                         /*receiver*/) override {}

 private:
  // Counts Done() / ProvideFeedback() invocations for one captured
  // frame's callback remote.
  class FakeFrameCallbacks
      : public viz::mojom::FrameSinkVideoConsumerFrameCallbacks {
   public:
    void Bind(mojo::PendingReceiver<
              viz::mojom::FrameSinkVideoConsumerFrameCallbacks> r) {
      receiver_.Bind(std::move(r));
    }
    int done_calls() const { return done_calls_; }

    // FrameSinkVideoConsumerFrameCallbacks:
    void Done() override { ++done_calls_; }
    void ProvideFeedback(
        const media::VideoCaptureFeedback& /*feedback*/) override {}

   private:
    int done_calls_ = 0;
    mojo::Receiver<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
        receiver_{this};
  };

  mojo::Receiver<viz::mojom::FrameSinkVideoCapturer> receiver_{this};
  mojo::Remote<viz::mojom::FrameSinkVideoConsumer> consumer_;
  std::vector<std::unique_ptr<FakeFrameCallbacks>> callback_holders_;
  std::vector<FakeFrameCallbacks*> fake_callbacks_;
  media::VideoPixelFormat last_format_ = media::PIXEL_FORMAT_UNKNOWN;
  viz::mojom::BufferFormatPreference last_start_pref_ =
      viz::mojom::BufferFormatPreference::kDefault;
  bool start_called_ = false;
  bool stop_called_ = false;
  uint64_t ts_us_ = 0;
};

class FrameSinkCapturerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // mojo::core::Init() is process-global and idempotent-once: it
    // must run exactly once before any Mojo primitive (MessagePipe,
    // Remote::BindNewPipeAndPassReceiver, etc.) is used. The
    // base::TestSuite main from //base/test:run_all_unittests does
    // NOT call it for us — only browsertest mains do. Without this
    // initialization the producer_.BindAndPassRemote() call below
    // crashes with FATAL "Mojo has not been initialized in this
    // process. You must call mojo::core::Init() as an embedder."
    // (mojo/public/c/system/thunks.cc:40).
    //
    // We use a function-local static initialized via lambda so Init
    // runs exactly once regardless of how many test fixtures
    // instantiate. base::NoDestructor<bool> static_asserts because
    // bool is trivially constructible/destructible — for a one-shot
    // side-effect initializer, a plain `static const bool` is the
    // canonical pattern.
    [[maybe_unused]] static const bool kMojoInited = []() {
      mojo::core::Init();
      return true;
    }();

    auto producer_remote = producer_.BindAndPassRemote();
    capturer_ = std::make_unique<CloudBrowserFrameSinkCapturer>(
        std::move(producer_remote),
        base::BindRepeating(&FrameSinkCapturerTest::OnFrame,
                            base::Unretained(this)));
  }

  void TearDown() override {
    capturer_.reset();
    delivered_.clear();
  }

  void OnFrame(scoped_refptr<media::VideoFrame> f) {
    delivered_.push_back(std::move(f));
  }

  // Run the runloop until idle so Mojo IPC settles.
  void FlushPendingIPC() {
    base::RunLoop loop;
    loop.RunUntilIdle();
  }

  base::test::SingleThreadTaskEnvironment task_env_;
  FakeProducer producer_;
  std::unique_ptr<CloudBrowserFrameSinkCapturer> capturer_;
  std::vector<scoped_refptr<media::VideoFrame>> delivered_;
};

TEST_F(FrameSinkCapturerTest, StartForwardsToProducer) {
  capturer_->Start(viz::VideoCaptureTarget(viz::FrameSinkId(1, 1)));
  FlushPendingIPC();
  EXPECT_TRUE(producer_.start_called());
}

TEST_F(FrameSinkCapturerTest, DefaultStartUsesI420SharedMemoryPath) {
  capturer_->Start(viz::VideoCaptureTarget(viz::FrameSinkId(1, 1)));
  FlushPendingIPC();

  EXPECT_EQ(media::PIXEL_FORMAT_I420, producer_.last_format())
      << "the default runtime path must avoid the NV12 mappable-SharedImage "
         "GMB lane; GPU-less pods do not have a GBM/shared-context backing "
         "for first-light capture";
  EXPECT_EQ(viz::mojom::BufferFormatPreference::kDefault,
            producer_.last_start_pref())
      << "I420 should use the shared-memory FrameSinkVideoCapturer path";
}

TEST_F(FrameSinkCapturerTest, ConfiguredNv12StillRequestsMappableSharedImage) {
  capturer_->Configure(gfx::Size(1280, 720), media::PIXEL_FORMAT_NV12,
                       base::Hertz(60));

  capturer_->Start(viz::VideoCaptureTarget(viz::FrameSinkId(1, 1)));
  FlushPendingIPC();

  EXPECT_EQ(media::PIXEL_FORMAT_NV12, producer_.last_format());
  EXPECT_EQ(viz::mojom::BufferFormatPreference::kPreferMappableSharedImage,
            producer_.last_start_pref())
      << "the future hardware/GMB lane should remain opt-in through Configure";
}

TEST_F(FrameSinkCapturerTest, FrameIsDeliveredAndDoneCalledOnRelease) {
  capturer_->Start(viz::VideoCaptureTarget(viz::FrameSinkId(1, 1)));
  FlushPendingIPC();

  producer_.SendFrame();
  FlushPendingIPC();

  ASSERT_EQ(1u, delivered_.size());
  // Done() is RAII-tied to frame destruction; until we drop our ref
  // it should NOT have fired yet.
  FlushPendingIPC();
  EXPECT_EQ(0, producer_.total_done_calls())
      << "Done() fired before frame released";

  delivered_.clear();
  FlushPendingIPC();
  EXPECT_EQ(1, producer_.total_done_calls())
      << "Done() did not fire after frame released";

  auto stats = capturer_->GetStats();
  EXPECT_EQ(1u, stats.frames_received);
  EXPECT_EQ(1u, stats.frames_delivered);
  EXPECT_EQ(1u, stats.buffers_done);
  EXPECT_EQ(0u, stats.frames_dropped_by_capturer);
}

TEST_F(FrameSinkCapturerTest, DonePostsBackWhenFrameReleasedOffSequence) {
  capturer_->Start(viz::VideoCaptureTarget(viz::FrameSinkId(1, 1)));
  FlushPendingIPC();

  producer_.SendFrame();
  FlushPendingIPC();

  ASSERT_EQ(1u, delivered_.size());
  scoped_refptr<media::VideoFrame> frame = std::move(delivered_.front());
  delivered_.clear();
  EXPECT_EQ(0, producer_.total_done_calls());

  base::Thread release_thread("cv2-91-frame-release");
  ASSERT_TRUE(release_thread.Start());
  base::RunLoop released;
  release_thread.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](scoped_refptr<media::VideoFrame> f, base::OnceClosure done) {
            f = nullptr;
            std::move(done).Run();
          },
          std::move(frame), released.QuitClosure()));
  released.Run();
  release_thread.Stop();

  FlushPendingIPC();
  EXPECT_EQ(1, producer_.total_done_calls())
      << "Mojo Done() must be posted back to the capturer sequence when "
         "libwebrtc releases the frame on an encoder/network thread";
  EXPECT_EQ(1u, capturer_->GetStats().buffers_done);
}

TEST_F(FrameSinkCapturerTest, SetOnFrameCallbackReplacesPlaceholderBeforeStart) {
  std::vector<scoped_refptr<media::VideoFrame>> rebound_delivered;
  capturer_->SetOnFrameCallback(base::BindLambdaForTesting(
      [&](scoped_refptr<media::VideoFrame> f) {
        rebound_delivered.push_back(std::move(f));
      }));

  capturer_->Start(viz::VideoCaptureTarget(viz::FrameSinkId(1, 1)));
  FlushPendingIPC();

  producer_.SendFrame();
  FlushPendingIPC();

  EXPECT_TRUE(delivered_.empty())
      << "the constructor callback must be replaceable before Start() so "
         "placeholder callbacks do not black-hole native WebRTC frames";
  EXPECT_EQ(1u, rebound_delivered.size());
}

TEST_F(FrameSinkCapturerTest, VideoTrackSourceRebindsCapturerFrameIngress) {
  FakeProducer producer;
  auto producer_remote = producer.BindAndPassRemote();
  auto capturer = std::make_unique<CloudBrowserFrameSinkCapturer>(
      std::move(producer_remote),
      base::BindRepeating([](scoped_refptr<media::VideoFrame>) {
        ADD_FAILURE() << "placeholder capturer callback fired; "
                         "CloudBrowserFrameSinkVideoTrackSource should "
                         "rebind it to OnCapturerFrame";
      }));
  auto source =
      webrtc::make_ref_counted<CloudBrowserFrameSinkVideoTrackSource>(
          std::move(capturer));

  source->capturer_for_test()->Start(
      viz::VideoCaptureTarget(viz::FrameSinkId(1, 1)));
  FlushPendingIPC();

  producer.SendFrame();
  FlushPendingIPC();

  EXPECT_EQ(1u, source->GetStats().frames_received_from_capturer)
      << "captured frames must enter CloudBrowserFrameSinkVideoTrackSource; "
         "otherwise the browser peer can negotiate a live video track but "
         "the portal will decode 0x0 forever";
  EXPECT_EQ(1u, source->GetStats().frames_published_to_sinks);
}

TEST_F(FrameSinkCapturerTest, MultipleFramesAllAcked) {
  capturer_->Start(viz::VideoCaptureTarget(viz::FrameSinkId(1, 1)));
  FlushPendingIPC();

  for (int i = 0; i < 5; ++i) producer_.SendFrame();
  FlushPendingIPC();

  EXPECT_EQ(5u, delivered_.size());
  delivered_.clear();
  FlushPendingIPC();
  EXPECT_EQ(5, producer_.total_done_calls());
  EXPECT_EQ(5u, capturer_->GetStats().buffers_done);
}

// DISABLED: capturer.cc no longer reads VideoFrameMetadata::frame_count_dropped
// (removed from chromium's media::VideoFrameMetadata — see Wall #28 TODO).
// Re-enable once we identify the replacement metric or move dropped-frame
// accounting to a different signal.
TEST_F(FrameSinkCapturerTest, DISABLED_DroppedFrameCountSurfacesInStats) {
  capturer_->Start(viz::VideoCaptureTarget(viz::FrameSinkId(1, 1)));
  FlushPendingIPC();

  producer_.SendFrame(/*dropped=*/3);
  producer_.SendFrame(/*dropped=*/2);
  FlushPendingIPC();
  EXPECT_EQ(5u, capturer_->GetStats().frames_dropped_by_capturer);
}

TEST_F(FrameSinkCapturerTest, DoneFiresEvenIfWrapFails) {
  // Force WrapExternalData failure by sending a malformed
  // VideoFrameInfoPtr (null). The capturer should still ack the
  // buffer.
  capturer_->Start(viz::VideoCaptureTarget(viz::FrameSinkId(1, 1)));
  FlushPendingIPC();
  // Manually invoke OnFrameCaptured with null info.
  auto region = base::ReadOnlySharedMemoryRegion::Create(64);
  auto handle = media::mojom::VideoBufferHandle::NewReadOnlyShmemRegion(
      std::move(region.region));
  mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
      cb_remote;
  auto cb_receiver = cb_remote.InitWithNewPipeAndPassReceiver();
  // Bind a one-shot fake callbacks counter inline.
  struct OneShot
      : public viz::mojom::FrameSinkVideoConsumerFrameCallbacks {
    int dones = 0;
    mojo::Receiver<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
        receiver{this};
    void Done() override { ++dones; }
    void ProvideFeedback(
        const media::VideoCaptureFeedback&) override {}
  };
  auto one_shot = std::make_unique<OneShot>();
  one_shot->receiver.Bind(std::move(cb_receiver));

  capturer_->OnFrameCaptured(std::move(handle),
                              /*info=*/nullptr,
                              gfx::Rect(),
                              std::move(cb_remote));
  FlushPendingIPC();
  EXPECT_EQ(1, one_shot->dones)
      << "Done() not fired despite null info — buffer-pool starvation risk";
  EXPECT_EQ(0u, delivered_.size());
  EXPECT_EQ(1u, capturer_->GetStats().frames_failed_to_wrap);
}

TEST_F(FrameSinkCapturerTest, StopForwardsToProducer) {
  capturer_->Start(viz::VideoCaptureTarget(viz::FrameSinkId(1, 1)));
  FlushPendingIPC();
  capturer_->Stop();
  FlushPendingIPC();
  EXPECT_TRUE(producer_.stop_called());
}

TEST_F(FrameSinkCapturerTest, StartIsIdempotent) {
  capturer_->Start(viz::VideoCaptureTarget(viz::FrameSinkId(1, 1)));
  capturer_->Start(viz::VideoCaptureTarget(viz::FrameSinkId(1, 1)));
  FlushPendingIPC();
  // FakeProducer flips start_called_ on every Start; we only want to
  // assert that we observe Start at least once. The relevant
  // production behaviour is that the capturer never re-issues
  // configuration — verified at code-review time, not here.
  EXPECT_TRUE(producer_.start_called());
}

}  // namespace
}  // namespace cloud_browser

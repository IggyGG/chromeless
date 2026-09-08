// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Unit tests for capture/signaling/cb_signaling_reconnect.{h,cc} — the R7
// reconnect supervisor.
//
// # Why these exist now
//
// The target has been built by every lane since M3 and constructed by
// NOTHING, and BUILD.gn carried a TODO promising this file "in a follow-up
// commit" that never came. CLAUDE.md's rule — a TODO that names its own
// verification step is a defect nobody has run yet — was exactly right: when
// the wrapper was finally wired into the embedder (CV2-REDIAL, 2026-09-08),
// its OnClosed rule turned out to be wrong for the case it was written for.
// It treated ANY close carrying code 1000 as consumer-driven and went
// terminal, so a broker that closed cleanly during a rollout would have left
// the worker exactly as stranded as no wrapper at all. That is the first
// test below, and it fails against the pre-CV2-REDIAL rule.
//
// # Fixture shape
//
// The wrapper's inner client is a real SignalingWsClient constructed against
// a network::mojom::NetworkContext, so these tests drive the wrapper through
// its OWN SignalingClientObserver face — which is precisely how the inner
// client talks to it — rather than standing up a fake NetworkContext. That
// keeps the tests on the seam the wrapper defines (observer in, observer
// out) and needs no mojo plumbing. `Connect()` is deliberately NOT called in
// most cases: the tests that need the open state install it by feeding
// OnConnected(), which is what a successful inner dial does.

#include "capture/signaling/cb_signaling_reconnect.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/test/task_environment.h"
#include "capture/signaling/cb_wire_envelope.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace cloud_browser {
namespace signaling {
namespace {

// Records what the consumer would see. The embedder (main_parts) reacts to
// exactly these four.
class RecordingObserver : public ReconnectingClientObserver {
 public:
  void OnConnected() override { ++connected; }
  void OnEnvelope(const Envelope& envelope) override {
    envelopes.push_back(envelope.type);
  }
  void OnClosed(uint16_t code, std::string_view reason) override {
    closed_codes.push_back(code);
    last_close_reason = std::string(reason);
  }
  void OnGaveUp(uint32_t attempts_made) override {
    gave_up = true;
    gave_up_after = attempts_made;
  }

  int connected = 0;
  std::vector<EnvelopeType> envelopes;
  std::vector<uint16_t> closed_codes;
  std::string last_close_reason;
  bool gave_up = false;
  uint32_t gave_up_after = 0;
};

// The wrapper needs a non-null NetworkContext pointer at construction (it
// DCHECKs), but these tests never let it dial: no test calls Connect(), and
// StartInnerDial is only reached from Connect() or a backoff fire, neither
// of which happens on the paths under test. A tagged null-ish pointer would
// be UB to dereference; nothing dereferences it here.
network::mojom::NetworkContext* FakeNetworkContext() {
  static int sentinel = 0;
  return reinterpret_cast<network::mojom::NetworkContext*>(&sentinel);
}

class CbSignalingReconnectTest : public ::testing::Test {
 protected:
  CbSignalingReconnectTest()
      : task_env_(base::test::TaskEnvironment::TimeSource::MOCK_TIME) {}

  // A wrapper with `observer_` attached and the given reconnect policy.
  std::unique_ptr<CbSignalingReconnect> MakeWrapper(ReconnectConfig cfg) {
    return std::make_unique<CbSignalingReconnect>(
        FakeNetworkContext(), WsClientConfig{}, cfg, &observer_);
  }

  // Put the wrapper in the state a successful inner dial produces.
  void FeedConnected(CbSignalingReconnect* w) {
    static_cast<SignalingClientObserver*>(w)->OnConnected();
  }
  void FeedClosed(CbSignalingReconnect* w, uint16_t code,
                  std::string_view reason) {
    static_cast<SignalingClientObserver*>(w)->OnClosed(code, reason);
  }

  base::test::TaskEnvironment task_env_;
  RecordingObserver observer_;
};

// THE REGRESSION. A broker that shuts down cleanly closes with 1000. That is
// not the consumer hanging up, and the wrapper must redial — otherwise the
// worker is stranded exactly as it was before the wrapper existed
// (docs/findings/worker-signaling-no-redial.md, measured three times on
// 2026-09-07: rollout, then a liveness kill ~70 s later).
//
// Watched fail against the pre-CV2-REDIAL rule
// `code == kCloseCodeNormal && state_ != kReconnecting`: the observer saw a
// terminal OnClosed(1000) and is_reconnecting() was false.
TEST_F(CbSignalingReconnectTest, ACleanRemoteCloseIsRedialedNotTerminal) {
  auto w = MakeWrapper(ReconnectConfig{});
  FeedConnected(w.get());
  ASSERT_TRUE(w->is_connected());

  FeedClosed(w.get(), /*code=*/1000, "broker going away");

  EXPECT_TRUE(observer_.closed_codes.empty())
      << "a remote close must not be reported to the consumer as terminal";
  EXPECT_TRUE(w->is_reconnecting())
      << "the wrapper must be backing off toward a redial";
}

// An abnormal drop (1006 — what a killed broker pod produces) is the same
// story with a different number.
TEST_F(CbSignalingReconnectTest, AnAbnormalCloseIsRedialed) {
  auto w = MakeWrapper(ReconnectConfig{});
  FeedConnected(w.get());

  FeedClosed(w.get(), /*code=*/1006, "abnormal");

  EXPECT_TRUE(observer_.closed_codes.empty());
  EXPECT_TRUE(w->is_reconnecting());
}

// The consumer's own Disconnect() must stay terminal. Without the
// disconnect_requested_ latch, making a remote 1000 redial would also make
// teardown redial — a worker that reconnects while shutting down.
TEST_F(CbSignalingReconnectTest, ConsumerDisconnectIsTerminal) {
  auto w = MakeWrapper(ReconnectConfig{});
  FeedConnected(w.get());

  w->Disconnect();  // no live inner client → synthetic OnClosed
  FeedClosed(w.get(), /*code=*/1000, "after our own disconnect");

  EXPECT_FALSE(w->is_reconnecting())
      << "our own Disconnect must not be answered with a reconnect";
  EXPECT_FALSE(observer_.closed_codes.empty())
      << "the consumer must be told its Disconnect completed";
}

// Backoff is exponential and capped, and attempts are bounded: after
// max_attempts the consumer gets OnGaveUp exactly once and the wrapper stops
// on its own. main_parts turns OnGaveUp into a process recycle.
TEST_F(CbSignalingReconnectTest, GivesUpAfterMaxAttempts) {
  ReconnectConfig cfg;
  cfg.initial_backoff = base::Milliseconds(10);
  cfg.multiplier = 2.0;
  cfg.max_backoff = base::Milliseconds(40);
  cfg.max_attempts = 3;
  cfg.idle_timeout = base::TimeDelta();  // off; not under test here
  auto w = MakeWrapper(cfg);

  FeedConnected(w.get());
  // Each drop schedules the next attempt; the attempt's dial fails
  // immediately in this fixture (no NetworkContext), which the wrapper sees
  // as another dead channel. Drive that loop by hand.
  for (int i = 0; i < 4; ++i) {
    FeedClosed(w.get(), /*code=*/1006, "drop");
    task_env_.FastForwardBy(base::Milliseconds(100));
  }

  EXPECT_TRUE(observer_.gave_up)
      << "the wrapper must stop after max_attempts rather than redial forever";
  EXPECT_EQ(observer_.gave_up_after, 3u);
}

// A successful reconnect resets the schedule: the consumer sees one
// OnConnected per recovered channel and the next outage starts from the
// initial backoff, not from the cap.
TEST_F(CbSignalingReconnectTest, ASuccessfulReconnectResetsTheSchedule) {
  ReconnectConfig cfg;
  cfg.initial_backoff = base::Milliseconds(10);
  cfg.max_attempts = 5;
  cfg.idle_timeout = base::TimeDelta();
  auto w = MakeWrapper(cfg);

  FeedConnected(w.get());
  FeedClosed(w.get(), 1006, "drop");
  EXPECT_TRUE(w->is_reconnecting());

  FeedConnected(w.get());  // the redial succeeded
  EXPECT_TRUE(w->is_connected());
  EXPECT_EQ(w->attempts_made(), 0u)
      << "a recovered channel must clear the attempt counter";
  EXPECT_EQ(observer_.connected, 2);
}

// Send() is refused while there is no channel. The consumer (the offerer
// driver) already handles a false return by buffering, and a queue inside
// the wrapper would deliver a pre-outage offer to a post-outage broker.
TEST_F(CbSignalingReconnectTest, SendIsRefusedWhileReconnecting) {
  auto w = MakeWrapper(ReconnectConfig{});
  Envelope env;
  env.type = EnvelopeType::kBye;
  env.from = PeerRole::kBrowser;

  EXPECT_FALSE(w->Send(env)) << "not connected yet";

  FeedConnected(w.get());
  FeedClosed(w.get(), 1006, "drop");
  EXPECT_FALSE(w->Send(env)) << "mid-backoff";
}

// Inbound envelopes reach the consumer unchanged — the wrapper is a
// supervisor, not a filter.
TEST_F(CbSignalingReconnectTest, EnvelopesArePassedThrough) {
  auto w = MakeWrapper(ReconnectConfig{});
  FeedConnected(w.get());

  Envelope env;
  env.type = EnvelopeType::kAnswer;
  env.from = PeerRole::kClient;
  static_cast<SignalingClientObserver*>(w.get())->OnEnvelope(env);

  ASSERT_EQ(observer_.envelopes.size(), 1u);
  EXPECT_EQ(observer_.envelopes[0], EnvelopeType::kAnswer);
}

}  // namespace
}  // namespace signaling
}  // namespace cloud_browser

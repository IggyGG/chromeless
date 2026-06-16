// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Unit coverage for CbInputDispatch's v1 envelope decoder.

#include "cloud-browser/capture/build-integration/cb_input_dispatch.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace cloud_browser {
namespace {

TEST(CbInputDispatchTest, AcceptsSpecEpochMsTimestamp) {
  InputEnvelope envelope;
  std::string reason;

  const bool ok = CbInputDispatch::ParseEnvelopeForTesting(
      R"({
        "v": 1,
        "type": "mouse_button",
        "t": 1730290000123,
        "seq": 4217,
        "data": {"button": 0, "action": "down", "x": 10, "y": 20}
      })",
      &envelope, &reason);

  ASSERT_TRUE(ok) << reason;
  EXPECT_EQ(1, envelope.version);
  EXPECT_EQ("mouse_button", envelope.type);
  EXPECT_EQ(1730290000123LL, envelope.t);
  EXPECT_EQ(4217LL, envelope.seq);
  EXPECT_TRUE(envelope.data.FindInt("x").has_value());
}

TEST(CbInputDispatchTest, RejectsFractionalTimestamp) {
  InputEnvelope envelope;
  std::string reason;

  const bool ok = CbInputDispatch::ParseEnvelopeForTesting(
      R"({
        "v": 1,
        "type": "mouse_move",
        "t": 1730290000123.5,
        "seq": 1,
        "data": {"x": 10, "y": 20}
      })",
      &envelope, &reason);

  EXPECT_FALSE(ok);
  EXPECT_EQ("missing or non-integer 't'", reason);
}

TEST(CbInputDispatchTest, RejectsUnsafeIntegerTimestamp) {
  InputEnvelope envelope;
  std::string reason;

  const bool ok = CbInputDispatch::ParseEnvelopeForTesting(
      R"({
        "v": 1,
        "type": "mouse_move",
        "t": 9007199254740992,
        "seq": 1,
        "data": {"x": 10, "y": 20}
      })",
      &envelope, &reason);

  EXPECT_FALSE(ok);
  EXPECT_EQ("missing or non-integer 't'", reason);
}

}  // namespace
}  // namespace cloud_browser

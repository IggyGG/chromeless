// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Unit coverage for the four CbLastPointerState transitions documented
// in cb_last_pointer.h:
//   1. default-constructed — at.is_null(), in_widget=true (the "no
//      events yet" reading);
//   2. Update — sets coords / buttons / at, sets in_widget=true;
//   3. MarkLeft — keeps coords / buttons / at, flips in_widget=false;
//   4. leave then Update — in_widget back to true, coords refreshed.
//
// Plus the two envelope helpers' null-safety + happy-path behaviour.

#include "cloud-browser/capture/build-integration/cb_last_pointer.h"

#include "base/time/time.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace cloud_browser {
namespace {

TEST(CbLastPointerStateTest, DefaultConstructedHasNoHistoryButInWidget) {
  CbLastPointerState state;
  const CbLastPointerSnapshot& snap = state.last_pointer();
  EXPECT_TRUE(snap.at.is_null());
  // Default-in-widget so the "post-leave" state (at set, in_widget
  // false) is distinguishable from the "never seen a forward yet"
  // state (at unset, in_widget true).
  EXPECT_TRUE(snap.in_widget);
  EXPECT_EQ(0.f, snap.x);
  EXPECT_EQ(0.f, snap.y);
  EXPECT_EQ(0u, snap.buttons_blink);
}

TEST(CbLastPointerStateTest, UpdateSetsFieldsAndEstablishesPresence) {
  CbLastPointerState state;
  const base::TimeTicks now = base::TimeTicks::Now();
  state.Update(100.5f, 200.25f, 0x01u, now);
  const CbLastPointerSnapshot& snap = state.last_pointer();
  EXPECT_EQ(100.5f, snap.x);
  EXPECT_EQ(200.25f, snap.y);
  EXPECT_EQ(0x01u, snap.buttons_blink);
  EXPECT_EQ(now, snap.at);
  EXPECT_TRUE(snap.in_widget);
}

TEST(CbLastPointerStateTest, MarkLeftKeepsCoordsAndFlipsInWidget) {
  CbLastPointerState state;
  const base::TimeTicks now = base::TimeTicks::Now();
  state.Update(42.f, 84.f, 0x02u, now);
  state.MarkLeft();
  const CbLastPointerSnapshot& snap = state.last_pointer();
  // Freeze-on-leave invariant — paint consumers rely on this.
  EXPECT_EQ(42.f, snap.x);
  EXPECT_EQ(84.f, snap.y);
  EXPECT_EQ(0x02u, snap.buttons_blink);
  EXPECT_EQ(now, snap.at);
  EXPECT_FALSE(snap.in_widget);
}

TEST(CbLastPointerStateTest, UpdateAfterLeaveReEstablishesPresence) {
  CbLastPointerState state;
  state.Update(10.f, 20.f, 0u, base::TimeTicks::Now());
  state.MarkLeft();
  ASSERT_FALSE(state.last_pointer().in_widget);

  // A client that sends mouse_leave then mouse_move (skipping the
  // optional mouse_enter envelope) should still end up correct.
  const base::TimeTicks later = base::TimeTicks::Now();
  state.Update(50.f, 60.f, 0x01u, later);
  const CbLastPointerSnapshot& snap = state.last_pointer();
  EXPECT_EQ(50.f, snap.x);
  EXPECT_EQ(60.f, snap.y);
  EXPECT_EQ(0x01u, snap.buttons_blink);
  EXPECT_EQ(later, snap.at);
  EXPECT_TRUE(snap.in_widget);
}

TEST(CbLastPointerStateTest, MarkEnteredIsIdempotentAndPreservesCoords) {
  CbLastPointerState state;
  const base::TimeTicks now = base::TimeTicks::Now();
  state.Update(7.f, 11.f, 0x04u, now);
  state.MarkLeft();
  state.MarkEntered();
  const CbLastPointerSnapshot& snap = state.last_pointer();
  EXPECT_TRUE(snap.in_widget);
  EXPECT_EQ(7.f, snap.x);
  EXPECT_EQ(11.f, snap.y);
  EXPECT_EQ(0x04u, snap.buttons_blink);
  EXPECT_EQ(now, snap.at);

  // Idempotent — second MarkEntered() changes nothing.
  state.MarkEntered();
  EXPECT_TRUE(state.last_pointer().in_widget);
}

TEST(CbLastPointerStateTest, MarkLeftIsIdempotent) {
  CbLastPointerState state;
  state.Update(1.f, 2.f, 0u, base::TimeTicks::Now());
  state.MarkLeft();
  ASSERT_FALSE(state.last_pointer().in_widget);
  state.MarkLeft();
  EXPECT_FALSE(state.last_pointer().in_widget);
}

TEST(HandlePointerLeaveEnvelopeTest, NullStateReturnsFalse) {
  base::DictValue data;
  EXPECT_FALSE(HandlePointerLeaveEnvelope(data, nullptr));
}

TEST(HandlePointerLeaveEnvelopeTest, FlipsInWidgetAndReturnsTrue) {
  CbLastPointerState state;
  state.Update(5.f, 6.f, 0u, base::TimeTicks::Now());
  base::DictValue data;
  EXPECT_TRUE(HandlePointerLeaveEnvelope(data, &state));
  EXPECT_FALSE(state.last_pointer().in_widget);
  // Coords preserved per the freeze-on-leave invariant.
  EXPECT_EQ(5.f, state.last_pointer().x);
  EXPECT_EQ(6.f, state.last_pointer().y);
}

TEST(HandlePointerEnterEnvelopeTest, NullStateReturnsFalse) {
  base::DictValue data;
  EXPECT_FALSE(HandlePointerEnterEnvelope(data, nullptr));
}

TEST(HandlePointerEnterEnvelopeTest, FlipsInWidgetTrueAndReturnsTrue) {
  CbLastPointerState state;
  state.Update(9.f, 13.f, 0u, base::TimeTicks::Now());
  state.MarkLeft();
  ASSERT_FALSE(state.last_pointer().in_widget);
  base::DictValue data;
  EXPECT_TRUE(HandlePointerEnterEnvelope(data, &state));
  EXPECT_TRUE(state.last_pointer().in_widget);
}

}  // namespace
}  // namespace cloud_browser

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "capture/cursor/cb_cursor_emit_policy.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace cloud_browser {
namespace cursor {

namespace {

// Wall #1: rounding uses std::lround on a (value / quantum) ratio then
// remultiplies by quantum. For quantum=1 we short-circuit to a plain
// lround so the common-case path doesn't pay a divide-then-multiply.
// std::lround returns a long; we narrow to int32_t for the snapshot
// field. Position values that overflow int32_t are nonsense for cursor
// coordinates (the M4 R3 snapshot's source is widget-space DIPs of a
// 1280x720 FSVC) — narrowing would clamp implementation-defined, but
// in practice this never fires. A CHECK would be wrong (the policy is
// on the hot UI-thread path); a static_cast is fine.
//
// Wall #2: negative values round toward zero in C / std::lround
// rounds away from zero. The watcher legacy emitter never sees
// negative cursor coords (widget-space, off-screen positions don't
// trigger SetCursor in chromium's hit-test path), so the difference
// is academic — but documenting the choice so a future reader of the
// dedupe-equality bug-report knows where the asymmetry came from.

}  // namespace

EmitPolicy::EmitPolicy(PolicyConfig config) : config_(std::move(config)) {}

EmitPolicy::~EmitPolicy() = default;

std::pair<int32_t, int32_t> EmitPolicy::RoundPosition(float x, float y) const {
  // Treat <= 0 as "round to nearest DIP" — quantum of 0 would divide
  // by zero, and negative quantum makes no sense. The header already
  // documents this behaviour; the .cc enforces it.
  const int q =
      (config_.position_quantum_px > 0) ? config_.position_quantum_px : 1;

  if (q == 1) {
    return {static_cast<int32_t>(std::lround(x)),
            static_cast<int32_t>(std::lround(y))};
  }

  const auto round_to_q = [q](float v) {
    const float ratio = v / static_cast<float>(q);
    const long rounded = std::lround(ratio);
    return static_cast<int32_t>(rounded * static_cast<long>(q));
  };
  return {round_to_q(x), round_to_q(y)};
}

bool EmitPolicy::IsDuplicate(const JoinedCursorState& joined) const {
  if (!last_.has_value()) {
    return false;
  }
  const auto [rx, ry] = RoundPosition(joined.x, joined.y);
  const PolicySnapshot& l = *last_;
  return l.type == joined.type &&
         l.visible == joined.visible &&
         l.buttons_blink == joined.buttons_blink &&
         l.x_rounded == rx &&
         l.y_rounded == ry;
}

EmitDecision EmitPolicy::Decide(const JoinedCursorState& joined,
                                base::TimeTicks now) {
  if (now.is_null()) {
    now = base::TimeTicks::Now();
  }

  // Dedupe runs FIRST and unconditionally. An identical-state record
  // is suppressed regardless of the debounce window. Mirrors the
  // legacy capture/cursor-watcher/main.go:487 ordering (equalState
  // returns early before any wire work happens) — same semantics for
  // the wire consumer.
  if (IsDuplicate(joined)) {
    return EmitDecision{
        /*emit=*/false,
        /*reason_for_suppress=*/std::string(kSuppressReasonDedupe),
    };
  }

  // Debounce only meaningful when configured > 0 AND we have a prior
  // emit to measure against. The first emit on a fresh policy
  // instance always passes through immediately.
  if (config_.min_emit_gap > base::TimeDelta() && last_.has_value()) {
    const base::TimeDelta gap = now - last_->emitted_at;
    if (gap < config_.min_emit_gap) {
      // Retain the joined state — the next R3 cycle (which may carry
      // the same or a newer state) will re-evaluate. v1 has no flush
      // timer; see header TODO(M5-R4-pending-flush-timer).
      pending_ = joined;
      return EmitDecision{
          /*emit=*/false,
          /*reason_for_suppress=*/std::string(kSuppressReasonDebounce),
      };
    }
  }

  // About to return .emit=true — caller will allocate a seq + emit
  // the wire bytes + call OnEmitted. Clear pending_ now so a
  // late-arriving identical Decide doesn't double-emit.
  pending_.reset();
  return EmitDecision{/*emit=*/true, /*reason_for_suppress=*/std::nullopt};
}

int64_t EmitPolicy::AllocSeq() {
  // Post-increment: returns 0 on first call, then 1, 2, … — matches
  // capture/cursor-watcher/main.go:499's atomic.AddInt64(&w.seq, 1) - 1
  // shape so the wire consumer (client/src/cursor.ts) sees the same
  // first-emit-is-0 sequence start across the streamer-page → native
  // cutover.
  return next_seq_++;
}

void EmitPolicy::OnEmitted(const JoinedCursorState& joined,
                           base::TimeTicks now) {
  if (now.is_null()) {
    now = base::TimeTicks::Now();
  }
  const auto [rx, ry] = RoundPosition(joined.x, joined.y);

  PolicySnapshot snap;
  snap.type = joined.type;
  snap.visible = joined.visible;
  snap.x_rounded = rx;
  snap.y_rounded = ry;
  snap.buttons_blink = joined.buttons_blink;
  snap.emitted_at = now;
  // The wire seq this emit consumed via AllocSeq(). next_seq_ has
  // already been incremented past it; snapshot.seq is next_seq_ - 1.
  //
  // Edge case: a caller could in principle call OnEmitted without
  // ever calling AllocSeq (next_seq_ still 0). v1 records seq = -1
  // in that case (post-decrement underflow) — but per the header
  // contract this is a programming error and we tolerate the weird
  // tracing rather than CHECK on the UI thread. If the team wants
  // strict, switch to a DCHECK_GT(next_seq_, 0) here.
  snap.seq = next_seq_ - 1;
  snap.has_pointer_history = joined.has_pointer_history;

  last_ = snap;

  if (config_.replay_buffer_capacity > 0) {
    replay_.push_back(snap);
    while (replay_.size() > config_.replay_buffer_capacity) {
      replay_.pop_front();
    }
  }
}

std::vector<PolicySnapshot> EmitPolicy::ReplayBuffer() const {
  return std::vector<PolicySnapshot>(replay_.begin(), replay_.end());
}

void EmitPolicy::Reset() {
  next_seq_ = 0;
  last_.reset();
  pending_.reset();
  replay_.clear();
}

}  // namespace cursor
}  // namespace cloud_browser

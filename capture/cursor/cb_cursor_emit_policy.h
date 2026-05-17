// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Cursor-channel v1 emission policy for the cb-chromium native cursor
// egress path.
//
// Module M5 R4 of the ChromelessV2 native-peer migration (CV2-22).
// Sits between M5 R3 (CbCursorXyJoin, which produces a
// JoinedCursorState every time the renderer asks for a different
// cursor and stamps the last-known pointer x,y onto it) and the M5 R6
// DC emitter (which writes the formatted envelope onto the cursor
// DataChannel that the M3 R5 DC host owns).
//
// What R4 owns (everything between "R3 says the cursor state just
// changed" and "R6 writes bytes onto the DC"):
//
//   1. STATE DEDUPE. R3 fires on every CursorChangeCallback edge
//      AND on visibility transitions AND on every position-only
//      update path the plumbing wires through it. Many of those
//      edges carry observably identical state (sub-DIP pointer
//      jitter, redundant SetCursor of the same type, ShowCursor of
//      an already-visible cursor). Mirrors the legacy
//      capture/cursor-watcher/main.go:485 maybeEmit + equalState
//      gate: same wire consumer, same dedupe semantics, so the JS
//      renderer in client/src/cursor.ts keeps round-tripping
//      unchanged across the streamer-page → native cutover.
//
//   2. TIME-BASED DEBOUNCE. A drag with the cursor flipping
//      between kPointer and kHand on hover boundaries can fire R3
//      faster than the DC can sensibly carry; the policy collapses
//      bursts to one emit per `min_emit_gap` window. Default is
//      zero (no debounce) to match the legacy watcher's
//      change-driven behaviour; tune at M5 R6 wire-up if SigNoz
//      shows DC backpressure.
//
//   3. PER-CHANNEL MONOTONIC SEQ. The wire envelope's `seq` field
//      MUST be strictly increasing per channel
//      (docs/protocols/cursor-channel.md). R2's EnvelopeAssembler
//      keeps its own seq counter, but that counter increments per
//      *assembled* envelope — once dedupe / debounce are active,
//      assembled-but-suppressed envelopes would make R2's seq
//      diverge from the wire seq. R4 owns the wire seq: AllocSeq()
//      hands the next value out to the caller that's actually
//      writing bytes onto the DC.
//
//   4. REPLAY ON RECONNECT. The cursor wire shape is fully self-
//      describing — the consumer needs the most recent (type,
//      visible, x, y) tuple to render correctly; no incremental
//      updates that require earlier frames. When a peer reconnects
//      its cursor DC, R6 calls ReplayBuffer() to get the last
//      decided-emit snapshot(s) and replays them so the renderer
//      doesn't sit on a stale cursor for an unbounded time.
//      Capacity is 1 by default; configurable so a future
//      multi-cursor channel (custom-image cache warm-up, R5) can
//      replay more.
//
// What R4 does NOT own:
//
//   * Wire envelope formatting / JSON encoding. That's M5 R2
//     (capture/cursor/cb_cursor_envelope.{h,cc}). R4's Decide()
//     returns yes/no + a reason string; the caller hands the joined
//     state to R2's EnvelopeAssembler::AssembleAndEncode when the
//     decision is yes.
//   * DC write / DC lifecycle. That's M5 R6 (consumes M3 R5's DC
//     host). R6 calls Decide() → if yes, R6 allocates the seq via
//     AllocSeq(), formats the envelope, writes to DC, and on
//     successful send calls OnEmitted() so the policy's dedupe /
//     replay state advances.
//   * Pointer-position observation. R3's xy-join already reads M4
//     R3's CbLastPointerSnapshot at every cursor-change edge. A
//     separate aura mouse-move pre-target observer for
//     position-only updates remains out-of-scope until SigNoz shows
//     the cursor lagging mouse motion during drags; if/when wired,
//     the observer SHOULD feed back through R3 (so the join stays
//     the single source of truth) rather than into R4 directly.
//     See TODO(M5-R4-position-only-observer) below.
//
// # R3 → R4 → R6 flow (no timer)
//
// v1 is intentionally pull-driven: R4 has no internal timer, no
// background thread, no PostDelayedTask. The flow on every R3 edge:
//
//   R3::OnCursorChange(type, visible)
//     → JoinedCursorState joined = build_joined(...)
//     → r6_callback.Run(joined)            // R6 is R3's emit-cb
//
//   R6::OnJoined(joined)
//     → EmitDecision d = policy_.Decide(joined);
//     → if (!d.emit) {                     // dedupe or debounce
//         // suppressed; policy retains joined as pending_ if it
//         // was a debounce hold, so the next change cycle re-evaluates
//         return;
//       }
//     → int64_t seq = policy_.AllocSeq();
//     → std::string json = envelope_assembler_.AssembleAndEncode(
//                            joined.type, joined.visible, /*now=*/0, seq);
//     → dc_host_.Send(json);
//     → policy_.OnEmitted(joined, base::TimeTicks::Now());
//
// Why no timer in v1: every interesting state change re-enters R3,
// which re-enters R6's OnJoined, which calls Decide() again — that's
// the same edge a timer would fire on, so a timer would be redundant
// for the steady-state path. The one case a timer WOULD matter is "we
// debounced an emit, then no further state change happens for a long
// time, and the consumer never sees the suppressed state" — that's
// pinned by the dedupe rule (the held state IS the current state, and
// the next legitimate change will re-emit) plus the
// replay-on-reconnect path (a fresh peer always gets the most recent
// decided emit). If a future tuning surfaces a missed-flush bug, the
// fix is to install a PostDelayedTask in R6 keyed off the
// pending-suppress decision; the policy stays pull-driven.
//
// # Seq divergence note (R2 seq vs R4 seq)
//
// R2's EnvelopeAssembler keeps a seq counter for "envelopes I
// assembled". R4 keeps a seq counter for "envelopes that hit the
// wire". Pre-dedupe these are equal; once dedupe / debounce are
// active, R2's seq runs ahead of R4's. The wire MUST carry R4's seq
// — the consumer asserts strict monotonicity on what it sees, and
// gaps from R2-assembled-then-R4-suppressed would falsely look like
// dropped DC frames. M5 R6's contract is "if you're going to use R2
// to encode AND R4 to gate, you MUST inject R4's seq into the encode
// call rather than letting R2 stamp its internal one". The encoder
// signature in M5 R2 already accepts an explicit seq for exactly
// this seam (its `now_epoch_ms` sentinel pattern was the model).
// TODO(M5-R4-r2-seq-injection): the encode entry point is currently
// EnvelopeAssembler::AssembleAndEncode(type, visible, now_epoch_ms);
// R6 will need a 4-arg variant that also threads `seq` through. Land
// that overload in the same change that wires R6.
//
// # Threading
//
// All entry points run on BrowserThread::UI alongside R3's emit
// callback. The policy holds no locks; the pending_ + last_ + replay_
// fields are touched on the same sequence R3 fires on. If a future
// caller wants to drive Decide() off-sequence (it shouldn't — R6 is
// also UI-thread), wrap in a SequenceChecker + bound callback rather
// than adding a lock here.
//
// # Wall #-style notes for the reviewer
//
//   * Pure policy (mirrors R2's :cb_cursor_envelope shape) — only
//     chromium deps are //base (TimeTicks / TimeDelta) and
//     //ui/base/cursor/mojom for the enum.
//   * Depends on M5 R3's :cursor_xy_join source_set for the
//     JoinedCursorState struct definition; that transitively pulls
//     in :embedder + :input_dispatch_mouse, which is heavier than
//     R4 strictly needs. If the build cone bites, JoinedCursorState
//     can be moved to a header-only seam in a follow-up — but for
//     v1 the simple dep matches how R3 depends on R1 + M4 R3.

#ifndef CAPTURE_CURSOR_CB_CURSOR_EMIT_POLICY_H_
#define CAPTURE_CURSOR_CB_CURSOR_EMIT_POLICY_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/time/time.h"
#include "capture/build-integration/cb_cursor_xy_join.h"
#include "ui/base/cursor/mojom/cursor_type.mojom-shared.h"

namespace cloud_browser {
namespace cursor {

// Knobs that govern dedupe / debounce / replay behaviour. Constructed
// once at peer-connection setup, immutable for the lifetime of the
// EmitPolicy instance. Defaults are chosen to reproduce the legacy
// capture/cursor-watcher/ behaviour (dedupe-only, no debounce, one
// replay slot) so the cutover is observable-equivalent at first
// wire-up.
struct PolicyConfig {
  // Minimum gap between two successive wire emits on the same
  // channel. Decide() returns .emit=false with reason="debounce" when
  // a candidate emit falls inside this window relative to the last
  // emitted state's emitted_at timestamp.
  //
  // Default base::TimeDelta() (zero) — debounce disabled, matches
  // capture/cursor-watcher/main.go which has none. Tune from M5 R6
  // operator config once SigNoz shows DC backpressure under drag.
  //
  // TODO(M5-R4-debounce-default): revisit after first end-to-end
  // soak; cursor channel volume should be well under the DC's
  // OnBufferedAmountLow threshold even at 60 Hz so a default of 0
  // is safe, but worth measuring.
  base::TimeDelta min_emit_gap;

  // Sub-DIP position deltas are noise — collapse to nearest N DIPs
  // before the dedupe equality check so a 0.4-DIP jitter doesn't
  // trigger a re-emit when nothing the user can see has changed.
  // Set to 1 for integer-DIP rounding (default); set to 0 to disable
  // rounding (sub-DIP positions then dedupe on bitwise equality,
  // which is almost never useful but kept as an escape hatch).
  //
  // Must be >= 0. Negative values are treated as 1 (see RoundPosition
  // in the .cc).
  int position_quantum_px = 1;

  // Cap on the replay ring buffer. v1 cursor wire is fully self-
  // describing, so the most recent emit is enough to bring a fresh
  // consumer up to date — default 1. Set to 0 to disable replay
  // entirely (the R6 reconnect path then has to manufacture a cold
  // initial state); set higher only when a follow-up channel
  // (R5 custom-image cache warm-up) needs the history.
  std::size_t replay_buffer_capacity = 1;
};

// Reason returned by Decide() when an emit is suppressed. Stable
// string-valued so tracing / SigNoz can group on it without a header
// shared with the consumer.
inline constexpr char kSuppressReasonDedupe[]   = "dedupe";
inline constexpr char kSuppressReasonDebounce[] = "debounce";

// Result of Decide(). When .emit is true, .reason_for_suppress is
// std::nullopt; when .emit is false, .reason_for_suppress is set to
// one of the kSuppressReason* constants above. Trivially copyable.
struct EmitDecision {
  bool emit = false;
  std::optional<std::string> reason_for_suppress;
};

// Snapshot of an actually-emitted state. The policy keeps the most
// recent snapshot for dedupe and the last N for replay-on-reconnect.
// Pure data, no methods.
struct PolicySnapshot {
  ui::mojom::CursorType type{};
  bool visible = true;

  // Rounded to PolicyConfig.position_quantum_px so dedupe equality
  // matches what the wire actually carried. The pre-rounding floats
  // are NOT retained — if the consumer needs sub-DIP for some future
  // use, store them separately upstream.
  int32_t x_rounded = 0;
  int32_t y_rounded = 0;

  // blink::WebInputEvent::Modifiers-format button-down bits from the
  // R3 join, mirrored from the JoinedCursorState. Included in the
  // dedupe key so a button-down-while-still inside the same hover
  // region re-emits.
  uint32_t buttons_blink = 0;

  // base::TimeTicks::Now() at the moment R6 confirmed the emit went
  // out (OnEmitted). Used as the reference point for the next
  // Decide() call's debounce gap.
  base::TimeTicks emitted_at;

  // The wire seq this snapshot was sent with. Strictly increasing
  // per EmitPolicy instance; allocated by AllocSeq().
  int64_t seq = 0;

  // Whether the R3 join knew of any pointer history when this
  // snapshot was emitted. Mirrored from JoinedCursorState so a
  // replay consumer can decide whether to render at (0,0) or skip
  // the position dimension.
  bool has_pointer_history = false;
};

// Single-channel emission policy. One instance per peer's cursor DC
// (or per per-peer plumbing struct that owns the DC + the joined
// callback hookup). Stateful, single-threaded.
//
// Lifetime: owned by the per-peer plumbing class that wires R3's
// SetEmitCallback to the R6 DC emitter. Outlives no chromium objects;
// destroyed when the peer connection tears down.
class EmitPolicy {
 public:
  explicit EmitPolicy(PolicyConfig config = {});

  EmitPolicy(const EmitPolicy&) = delete;
  EmitPolicy& operator=(const EmitPolicy&) = delete;

  ~EmitPolicy();

  // Evaluate a joined state from R3. Returns:
  //   .emit==true  → caller SHOULD allocate a seq (AllocSeq()),
  //                  format the envelope (M5 R2), write to DC (M5 R6),
  //                  and on successful send call OnEmitted(joined).
  //   .emit==false → suppressed. .reason_for_suppress is one of
  //                  kSuppressReasonDedupe / kSuppressReasonDebounce.
  //                  When the reason is debounce, the policy retains
  //                  the joined state in pending_ so the next change
  //                  cycle re-evaluates; when dedupe, no state is
  //                  retained (the duplicate is just dropped).
  //
  // |now| is callable-injected so tests can pin time without standing
  // up base::ScopedMockTimeMessageLoopTaskRunner. Production passes
  // base::TimeTicks() (default-constructed sentinel) and the policy
  // reads base::TimeTicks::Now() internally. Sentinel matches M5 R2's
  // now_epoch_ms convention.
  EmitDecision Decide(const JoinedCursorState& joined,
                      base::TimeTicks now = base::TimeTicks());

  // Reserve and consume the next monotonic per-channel seq. R6 calls
  // this exactly once per decided emit, between Decide() returning
  // .emit=true and the DC.Send() call.
  //
  // Returns values 0, 1, 2, … in order. Matches
  // capture/cursor-watcher/main.go:499 semantics (the legacy watcher
  // uses atomic.AddInt64(&w.seq, 1) - 1 for the same first-emit-is-0
  // shape that the wire consumer already handles).
  //
  // Per-channel: separate EmitPolicy instances have independent seq
  // streams. Cross-peer seq alignment is not a wire requirement.
  int64_t AllocSeq();

  // Called by R6 after the wire bytes for a decided emit have been
  // handed off to the DC. Records the actual emitted state into
  // last_ (for the next dedupe check) and pushes a PolicySnapshot
  // into the replay ring.
  //
  // |joined| MUST be the same joined state that produced the most
  // recent Decide(.emit=true). |now| is callable-injected as for
  // Decide(); pass base::TimeTicks() in production.
  //
  // It is a programming error to call OnEmitted without a preceding
  // Decide(.emit=true); v1 silently records the state anyway (we
  // don't want to CHECK on a DC race), but tracing will look weird.
  void OnEmitted(const JoinedCursorState& joined,
                 base::TimeTicks now = base::TimeTicks());

  // Returns the replay ring buffer in oldest-first order. R6 calls
  // this on a fresh peer DC bind so the new consumer learns the
  // last-known cursor state without having to wait for the next
  // renderer-driven change.
  //
  // Empty when no emit has yet happened or when
  // PolicyConfig.replay_buffer_capacity is 0.
  std::vector<PolicySnapshot> ReplayBuffer() const;

  // Drops the replay buffer, clears last_ / pending_, and resets
  // next_seq_ to 0. Called by R6 on peer-connection teardown.
  //
  // A subsequent reconnect starts cold from seq=0 with no replay;
  // clients MUST NOT assume seq continuity across DC teardowns.
  // Matches the legacy watcher.Run, which resets on every cdp dial
  // cycle.
  void Reset();

  // Test accessors — used by cb_cursor_emit_policy_test.cc to assert
  // dedupe / debounce / replay invariants without standing up real
  // R3 + R6 plumbing. Production code MUST NOT use these.
  int64_t next_seq_for_testing() const { return next_seq_; }
  bool has_pending_for_testing() const { return pending_.has_value(); }
  bool has_last_for_testing() const { return last_.has_value(); }
  std::size_t replay_size_for_testing() const { return replay_.size(); }

 private:
  // Is |joined| observationally equivalent to last_ under the policy's
  // dedupe rules? Position is rounded to config_.position_quantum_px
  // before comparison; type, visible, buttons_blink compare exactly.
  // Returns false when last_ is unset (first emit is never a dup).
  bool IsDuplicate(const JoinedCursorState& joined) const;

  // Round (x, y) to the configured quantum. Helper to keep
  // IsDuplicate and OnEmitted consistent on the rounding rule.
  std::pair<int32_t, int32_t> RoundPosition(float x, float y) const;

  const PolicyConfig config_;

  // Strictly increasing, starts at 0. Allocated by AllocSeq();
  // recorded into the snapshot OnEmitted writes.
  int64_t next_seq_ = 0;

  // Most-recently emitted state. nullopt until first OnEmitted; once
  // set, drives dedupe and the debounce gap reference point. Cleared
  // by Reset().
  std::optional<PolicySnapshot> last_;

  // Held-back joined state from a debounce-suppressed Decide. v1
  // does NOT spin a flush timer — the next R3 cycle re-enters
  // Decide() and will either re-suppress (if still inside the gap)
  // or proceed (gap expired). Cleared on the next .emit=true
  // decision and on Reset().
  //
  // TODO(M5-R4-pending-flush-timer): if SigNoz shows missed final-
  // frame cursor state after a burst (held-back state never re-
  // delivered because the renderer stopped changing the cursor),
  // promote the flush to a PostDelayedTask in R6 keyed off this
  // field. Policy stays pull-driven; R6 owns the wall clock.
  std::optional<JoinedCursorState> pending_;

  // Replay ring buffer. Capacity-bounded; oldest entries evicted
  // first when push exceeds config_.replay_buffer_capacity. deque
  // (not vector) so push_back/pop_front are O(1) and the iteration
  // order is obvious to a reader.
  std::deque<PolicySnapshot> replay_;
};

}  // namespace cursor
}  // namespace cloud_browser

#endif  // CAPTURE_CURSOR_CB_CURSOR_EMIT_POLICY_H_

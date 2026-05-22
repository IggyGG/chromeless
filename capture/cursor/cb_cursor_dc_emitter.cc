// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "capture/cursor/cb_cursor_dc_emitter.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"

namespace cloud_browser {
namespace cursor {

namespace {

// Wall #1: the cursor-channel v1 wire seq MUST come from M5 R4's
// AllocSeq(), NOT M5 R2's internal seq_ counter. R2's seq advances
// per *assembled* envelope and would diverge from R4's wire seq the
// moment dedupe / debounce starts suppressing emits (see R4 header
// "Seq divergence note"). R6 explicitly threads R4's seq into the
// encode call via the V1EnvelopeView's seq field so the wire bytes
// carry the right one.
//
// Wall #2: the encode path is currently
//   assembler->AssembleAndEncode(type, visible, now_epoch_ms)
// which stamps R2's internal seq. R4's header has
// TODO(M5-R4-r2-seq-injection): "the encode entry point is currently
// AssembleAndEncode(type, visible, now_epoch_ms); R6 will need a
// 4-arg variant that also threads `seq` through. Land that overload
// in the same change that wires R6."
//
// This DRAFT lands the wiring with the workaround: R6 calls
// Assemble() to build a V1EnvelopeView, overwrites the .seq field,
// then calls the static EncodeJson(view) — the static encoder
// already takes the view by const-ref and emits whatever seq is in
// it. The R4-required overload lives as a TODO(M5-R6-…) marker
// below; landing it is a separate diff because it touches R2's
// header (out-of-scope for an R6 draft).
//
// Wall #3: replay-on-reconnect reuses the recorded snapshot's seq
// (NOT a fresh AllocSeq) — the policy already incremented next_seq_
// when the snapshot was first emitted. Allocating a new seq would
// make the wire carry the same payload under two different seqs;
// the cursor consumer's monotonic-seq invariant tolerates that, but
// it's wasteful and breaks the "this snapshot was sent with this
// seq" mental model the policy maintains.

constexpr signaling::CbDcLabel kLabel = signaling::CbDcLabel::kCursor;

// Build a V1EnvelopeView from a JoinedCursorState + an explicit wire
// seq, then JSON-encode. Returns nullopt on encoder failure (see R2
// EncodeJson docs — well-formed input should never fail; we honour
// the optional return so the caller doesn't double-encode an error
// case).
std::optional<std::string> EncodeWithSeq(EnvelopeAssembler* assembler,
                                         const JoinedCursorState& joined,
                                         int64_t seq) {
  // Use Assemble (not AssembleAndEncode) so we can stamp the seq
  // before the JSON write. Wall #2 above explains why; the R2 seq
  // overload is the proper fix.
  V1EnvelopeView view = assembler->Assemble(joined.type, joined.visible,
                                            /*now_epoch_ms=*/0);
  view.seq = seq;

  // Stamp the position from the joined state (R2's Assemble defaults
  // x/y to the latched position which R6 has no path to populate
  // until M5 R4 wires the position-only observer through R3). Until
  // then, the join's float coords ARE the authoritative position;
  // narrow to int for the wire (the protocol's x/y fields are int).
  // TODO(M5-R6-position-via-r2-latched): once R4 / R3 wire
  // SetLatchedPosition on the assembler, drop this override and
  // trust Assemble's defaults.
  view.x = static_cast<int>(joined.x);
  view.y = static_cast<int>(joined.y);

  return EnvelopeAssembler::EncodeJson(view);
}

}  // namespace

class CbCursorDcEmitter::CursorChannelObserver
    : public webrtc::DataChannelObserver {
 public:
  explicit CursorChannelObserver(CbCursorDcEmitter* emitter)
      : emitter_(emitter) {}

  CursorChannelObserver(const CursorChannelObserver&) = delete;
  CursorChannelObserver& operator=(const CursorChannelObserver&) = delete;

  ~CursorChannelObserver() override = default;

  void OnStateChange() override {
    if (!emitter_) {
      return;
    }
    const bool open = emitter_->dc_host_->IsOpen(kLabel);
    emitter_->OnChannelStateChanged(
        kLabel, open ? webrtc::DataChannelInterface::DataState::kOpen
                     : webrtc::DataChannelInterface::DataState::kClosed);
  }

  void OnMessage(const webrtc::DataBuffer& /*buffer*/) override {
    // The cursor channel is browser -> client only in v1. Inbound frames
    // on this label are client drift; drop them quietly to match the
    // host's unbound-channel behaviour.
  }

  void OnBufferedAmountChange(uint64_t /*sent_data_size*/) override {}

  bool IsOkToCallOnTheNetworkThread() override { return false; }

 private:
  raw_ptr<CbCursorDcEmitter> emitter_;
};

CbCursorDcEmitter::CbCursorDcEmitter(
    CbCursorXyJoin* xy_join,
    EmitPolicy* emit_policy,
    EnvelopeAssembler* envelope_assembler,
    signaling::CbDataChannelHost* dc_host,
    scoped_refptr<base::SequencedTaskRunner> ui_task_runner)
    : xy_join_(xy_join),
      emit_policy_(emit_policy),
      envelope_assembler_(envelope_assembler),
      dc_host_(dc_host),
      ui_task_runner_(std::move(ui_task_runner)) {
  CHECK(xy_join_);
  CHECK(emit_policy_);
  CHECK(envelope_assembler_);
  CHECK(dc_host_);
  CHECK(ui_task_runner_);
  DETACH_FROM_SEQUENCE(ui_sequence_);
}

CbCursorDcEmitter::~CbCursorDcEmitter() {
  // Stop is idempotent + UI-thread; the dtor is also UI-thread per
  // the lifetime contract.
  Stop();
}

void CbCursorDcEmitter::BindAndStart() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_);
  if (started_) {
    // Idempotent — replace the R3 slot with ourselves again and
    // re-bind the host observer slot. No-op semantics from the
    // outside, defensive against double-bind from a confused
    // embedder bring-up path.
    // TODO(M5-R6-bind-idempotent-test): unit-test BindAndStart twice
    // and assert no observable difference (no leaked callback, no
    // double subscription).
    return;
  }
  started_ = true;

  // R3's emit slot — single-occupancy by R3's contract. The bound
  // callback will fire synchronously on the UI thread (R3 header).
  xy_join_->SetEmitCallback(base::BindRepeating(&CbCursorDcEmitter::OnJoined,
                                                weak_factory_.GetWeakPtr()));

  // R6 needs cursor-channel open/close but the host-level observer slot
  // is reserved for session-wide consumers. Bind a tiny per-channel
  // DataChannelObserver shim through CbDataChannelHost's normal
  // observer fan-out; it converts OnStateChange into the labelled
  // CbDataChannelHostObserver callback this class already implements.
  cursor_channel_observer_ = std::make_unique<CursorChannelObserver>(this);
  dc_host_->BindObserver(kLabel, cursor_channel_observer_.get());
  if (dc_host_->IsOpen(kLabel)) {
    OnChannelStateChanged(kLabel,
                          webrtc::DataChannelInterface::DataState::kOpen);
  }
  LOG(INFO) << "CV2-83: CbCursorDcEmitter bound to cursor DataChannel "
               "(per-channel state observer active)";
}

void CbCursorDcEmitter::Stop() {
  // Note: Stop is also called from ~CbCursorDcEmitter; we don't
  // assert sequence here because the dtor on a partially-bound R6
  // (e.g. ctor succeeded but BindAndStart never ran) must still be
  // safe from any thread. The check is below, guarded by started_.
  if (!started_) {
    return;
  }
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_);
  started_ = false;

  // Detach from R3 first — a callback in flight at this point is
  // already on the UI thread and we're on the UI thread, so this is
  // race-free.
  xy_join_->SetEmitCallback(JoinedCursorCallback());

  dc_host_->BindObserver(kLabel, nullptr);
  cursor_channel_observer_.reset();

  // Invalidate weak pointers so any UI-task-runner hop posted
  // before Stop but not yet dispatched is dropped on dispatch.
  weak_factory_.InvalidateWeakPtrs();
}

EmitterStats CbCursorDcEmitter::Stats() const {
  // Pull-only snapshot. Torn-write-safe on aligned uint64_t per
  // Wall # in the header. No lock; no fence.
  return stats_;
}

void CbCursorDcEmitter::OnChannelStateChanged(
    signaling::CbDcLabel label,
    webrtc::DataChannelInterface::DataState state) {
  // Fires on libwebrtc signaling thread. Hop to the UI thread so
  // emitter state mutation stays on a single sequence.
  ui_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&CbCursorDcEmitter::OnChannelStateChanged_Ui,
                                weak_factory_.GetWeakPtr(), label, state));
}

void CbCursorDcEmitter::OnChannelStateChanged_Ui(
    signaling::CbDcLabel label,
    webrtc::DataChannelInterface::DataState state) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_);
  if (label != kLabel) {
    // Not our channel. Host's host-level observer surface fans out
    // ALL channels' state changes; the per-channel trampoline (when
    // it lands; see TODO above) only fires for kCursor. We accept
    // both modes defensively.
    return;
  }

  using State = webrtc::DataChannelInterface::DataState;
  switch (state) {
    case State::kOpen: {
      const bool reopen = cursor_dc_ever_opened_ && !cursor_dc_open_;
      cursor_dc_open_ = true;
      cursor_dc_ever_opened_ = true;

      // Drain a single pending-while-closed edge, if any. Always do
      // this BEFORE replay so the most-recent state lands last on
      // the wire (consumers render the last-seen seq).
      if (pending_while_closed_.has_value()) {
        JoinedCursorState pending = std::move(*pending_while_closed_);
        pending_while_closed_.reset();
        OnJoined(pending);
      }

      // Replay only on a real kClosed → kOpen transition (not on
      // the initial kConnecting → kOpen latch, since the policy's
      // ReplayBuffer is empty there). Use was_closed AND a non-
      // empty replay buffer; this combination uniquely identifies
      // reconnect.
      if (reopen) {
        ReplayOnOpen();
      }
      break;
    }
    case State::kClosing:
    case State::kClosed:
      cursor_dc_open_ = false;
      break;
    case State::kConnecting:
      // Pre-open — nothing to do. OnJoined edges that arrive while
      // kConnecting get retained in pending_while_closed_.
      break;
  }
}

void CbCursorDcEmitter::OnJoinedForTesting(const JoinedCursorState& joined) {
  OnJoined(joined);
}

void CbCursorDcEmitter::OnJoined(const JoinedCursorState& joined) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_);

  // Wall #1: even when the DC isn't open, run Decide() so the
  // policy's last_ stays in sync with reality. Skipping Decide
  // would mean the first emit after reconnect re-fires duplicate
  // payloads the consumer already had. R4's contract is "Decide()
  // is the only path that mutates the policy's view of recent
  // state"; bypassing it under any condition breaks that contract.
  //
  // Wall #2: when the DC isn't open we still need a place to park
  // the most-recent state so the kOpen-transition replay catches it.
  // Decide() does NOT do that — its pending_ slot only fills on a
  // debounce hold, not on a "nowhere to send" condition.
  // pending_while_closed_ is the R6-owned slot that handles the
  // DC-closed case; R4's pending_ handles the debounce-hold case;
  // the two slots compose cleanly because at most one can be
  // populated for a given joined edge (a kClosed DC never produces
  // a debounce hold — Decide() can't get to the debounce check
  // because we shortcut after it).
  //
  // Wall #3 — note on the actual control flow below: we DO call
  // Decide unconditionally, then branch on cursor_dc_open_. The
  // dedupe / debounce decision is still correct because the policy
  // doesn't know whether we sent the bytes — OnEmitted is what
  // tells the policy "this snapshot hit the wire", and we
  // selectively call OnEmitted only on real DC send success.

  const EmitDecision decision = emit_policy_->Decide(joined);

  if (!decision.emit) {
    // Suppressed — bump the right counter and bail. We do NOT
    // touch pending_while_closed_ here even if the DC is closed;
    // the policy has already decided this state isn't worth
    // emitting (either a duplicate or inside a debounce window).
    if (decision.reason_for_suppress.has_value()) {
      const std::string& reason = *decision.reason_for_suppress;
      if (reason == kSuppressReasonDedupe) {
        ++stats_.suppress_dedupe;
      } else if (reason == kSuppressReasonDebounce) {
        ++stats_.suppress_debounce;
      }
      // Unknown reason strings are deliberately not counted —
      // R4's header pins the two constants and any drift should
      // surface as zero-counter weirdness in stats, not as a
      // silent miscount.
    }
    return;
  }

  if (!cursor_dc_open_) {
    // DC isn't ready yet. Park the joined state so the open-time
    // drain replays it. R4's Decide() returned .emit=true so this
    // state would have hit the wire; we just couldn't deliver it.
    // We do NOT call OnEmitted here — last_ stays on the previous
    // emitted state so the post-reconnect first emit retries
    // cleanly.
    pending_while_closed_ = joined;
    ++stats_.suppress_dc_closed;
    return;
  }

  // Decided emit, DC is open. Allocate the wire seq, encode, send.
  const int64_t seq = emit_policy_->AllocSeq();
  EncodeAndSendAsync(joined, seq, /*record_emitted=*/true);
}

void CbCursorDcEmitter::EncodeAndSendAsync(const JoinedCursorState& joined,
                                           int64_t seq,
                                           bool record_emitted) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_);

  std::optional<std::string> encoded =
      EncodeWithSeq(envelope_assembler_, joined, seq);
  if (!encoded.has_value()) {
    // R2's EncodeJson should never fail on well-formed input — see
    // its header. Counting as a send error so a regression here
    // shows up in stats even if the cause is encode-side.
    ++stats_.send_errors;
    LOG(WARNING) << "CbCursorDcEmitter: EncodeJson returned nullopt "
                    "for seq="
                 << seq;
    return;
  }

  // Cursor edges are delivered synchronously on the UI thread from
  // Aura's SetCursor path. Do not BlockingCall into libwebrtc from
  // that posted-task context; queue the send onto the signaling thread
  // and handle the queued/not-queued result back on UI.
  dc_host_->SendAsync(
      kLabel, std::move(*encoded), ui_task_runner_,
      base::BindOnce(&CbCursorDcEmitter::OnSendComplete,
                     weak_factory_.GetWeakPtr(), joined, seq, record_emitted));
}

void CbCursorDcEmitter::OnSendComplete(JoinedCursorState joined,
                                       int64_t seq,
                                       bool record_emitted,
                                       bool ok,
                                       std::string message) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_);

  if (!ok) {
    ++stats_.send_errors;
    LOG(WARNING) << "CbCursorDcEmitter: DC Send failed for seq=" << seq
                 << " err=" << message;
    return;
  }

  ++stats_.emits;
  if (record_emitted) {
    emit_policy_->OnEmitted(joined);
  }
}

void CbCursorDcEmitter::ReplayOnOpen() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_);

  const std::vector<PolicySnapshot> snaps = emit_policy_->ReplayBuffer();
  if (snaps.empty()) {
    return;
  }
  ++stats_.dc_replays;

  // Build a minimal JoinedCursorState from each snapshot and
  // re-encode with the recorded seq (NOT a fresh AllocSeq — see
  // Wall #3 in the .cc top-of-file comment). x/y are int32_t in
  // the snapshot (post-rounding) but the join carries float; cast
  // back via static_cast — the rounding has already happened on
  // ingress, so this is a representation conversion only.
  for (const PolicySnapshot& snap : snaps) {
    JoinedCursorState joined;
    joined.type = snap.type;
    joined.visible = snap.visible;
    joined.x = static_cast<float>(snap.x_rounded);
    joined.y = static_cast<float>(snap.y_rounded);
    joined.buttons_blink = snap.buttons_blink;
    joined.has_pointer_history = snap.has_pointer_history;
    // last_motion_at / joined_at are intentionally left default —
    // the snapshot's recorded emitted_at refers to a prior session
    // and would be misleading on the replayed wire. The cursor
    // protocol's `t` field is filled by R2 with the current
    // wall-clock at Assemble time (now_epoch_ms=0 sentinel), which
    // is the correct semantics for a replay: "this is the latest
    // known state as of now".

    EncodeAndSendAsync(joined, snap.seq, /*record_emitted=*/false);
    // We DO NOT call OnEmitted on the policy for replays — the
    // snapshot is already in last_/replay_, recording it again
    // would be a no-op for last_ (same state) but would push a
    // duplicate into the replay deque.
  }
}

}  // namespace cursor
}  // namespace cloud_browser

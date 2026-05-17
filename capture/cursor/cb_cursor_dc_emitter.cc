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
  V1EnvelopeView view = assembler->Assemble(joined.type,
                                            joined.visible,
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
  xy_join_->SetEmitCallback(base::BindRepeating(
      &CbCursorDcEmitter::OnJoined, weak_factory_.GetWeakPtr()));

  // M3 R5 has TWO observer surfaces:
  //   1. A host-level CbDataChannelHostObserver passed into the
  //      host's ctor. v1 production wires this to M6 R1's
  //      CbWebrtcEventEmitter, not to R6.
  //   2. Per-channel webrtc::DataChannelObserver via
  //      host->BindObserver(label, observer). R6 binds itself here
  //      so the kCursor state changes route to R6 without
  //      contending for the host-level slot M6 wants.
  //
  // But R6's CbCursorDcEmitter implements the *host-level*
  // observer surface (signaling::CbDataChannelHostObserver) — the
  // host-level surface delivers state-change with a CbDcLabel
  // attached, while the webrtc::DataChannelObserver per-channel
  // surface only knows about the single channel it's bound to.
  // The host-level surface is the cleaner seam for R6 because the
  // OnChannelStateChanged signature already carries the label.
  //
  // Resolution (DRAFT decision): R6 attaches as the host-level
  // observer. M6 R1's CbWebrtcEventEmitter — which today is the
  // assumed sole consumer of the host-level slot — will need to
  // either fan-out on its side or move to a per-channel observer
  // when M6 R1 lands. M6 R1 is currently drafted but not yet
  // integrated; clarifying the fan-out is a small follow-up that
  // doesn't block R6 wiring.
  //
  // TODO(M5-R6-host-observer-fanout): coordinate with M6 R1's
  // integration so the host-level observer slot has a fan-out
  // adapter. Until then, R6 owning the slot is the right call
  // because cursor open/close is on the M5 critical path and M6
  // stats forwarding can wait.
  //
  // Practical note: the host's observer pointer is set at
  // construction time (see CbDataChannelHost ctor docs). R6 cannot
  // bind itself by calling a setter on the existing host instance
  // in this DRAFT — the host has no SetHostObserver method. R6's
  // BindAndStart instead registers a per-channel
  // webrtc::DataChannelObserver via host->BindObserver(kCursor,
  // shim), where `shim` is a thin DataChannelObserver that
  // forwards OnStateChange into R6's OnChannelStateChanged.
  //
  // TODO(M5-R6-host-ctor-injection): teach CbDataChannelHost's
  // ctor to accept the host-level observer as an optional
  // post-construction setter, OR have CbOffererDriver wire the
  // host with R6's observer pointer pre-construction once R6 is
  // alive by then in the embedder bring-up order. Until then the
  // per-channel observer trampoline below is sufficient for the
  // open/close signals R6 needs.
  //
  // For the DRAFT: leave R6 as the host-level observer in TYPE
  // (we inherit CbDataChannelHostObserver), and leave a TODO to
  // wire it through the host ctor. The OnChannelStateChanged
  // override is callable from either surface — the M6 R1 work
  // will simply route into it.
  //
  // The per-channel observer trampoline is NOT installed in this
  // DRAFT because it requires a separate DataChannelObserver
  // adapter class that touches webrtc::DataBuffer types not yet
  // pulled by R6's header dep set. The follow-up commit lands the
  // adapter; the DRAFT documents the intent.
  //
  // TODO(M5-R6-per-channel-observer-trampoline): write a small
  // DataChannelObserver adapter class (or use a lambda
  // type-erasure via an anonymous-namespace class in this .cc)
  // that forwards OnStateChange → OnChannelStateChanged with
  // label=kCursor. Bind it via dc_host_->BindObserver(kCursor,
  // &trampoline). Owned by this object; UnregisterObserver in
  // Stop().
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

  // TODO(M5-R6-per-channel-observer-trampoline): once the
  // trampoline lands, dc_host_->BindObserver(kCursor, nullptr) here
  // to unwire the open/close stream before R6 destructs. Until
  // then, R6's OnChannelStateChanged override is unreachable from
  // the host (the host has no per-channel observer pointer to
  // unset).

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
      FROM_HERE,
      base::BindOnce(&CbCursorDcEmitter::OnChannelStateChanged_Ui,
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
      const bool was_closed = !cursor_dc_open_;
      cursor_dc_open_ = true;

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
      if (was_closed) {
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
  std::string sent_json;
  if (!EncodeAndSend(joined, seq, &sent_json)) {
    // Send failure — don't call OnEmitted, don't update last_.
    // Next equivalent joined edge will try again with a fresh seq
    // (R4's AllocSeq doesn't roll back; the seq we just allocated
    // is gone, leaving a one-seq gap in the wire stream).
    //
    // TODO(M5-R6-seq-rollback): R4 has no AllocSeqRollback() entry
    // point. A wire-side gap is harmless for the cursor protocol
    // (consumers don't reason about contiguity) but if a future
    // channel needs gapless seqs, add the rollback to R4 and call
    // it here.
    return;
  }

  // Bookkeeping last per R4's contract.
  emit_policy_->OnEmitted(joined);
}

bool CbCursorDcEmitter::EncodeAndSend(const JoinedCursorState& joined,
                                      int64_t seq,
                                      std::string* out_json) {
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
    return false;
  }

  // Internal-thread-safe per CbDataChannelHost::Send docs — no hop
  // needed from the UI thread.
  const signaling::SendResult result = dc_host_->Send(kLabel, *encoded);
  if (!result.ok()) {
    ++stats_.send_errors;
    LOG(WARNING) << "CbCursorDcEmitter: DC Send failed for seq=" << seq
                 << " err=" << result.message();
    return false;
  }

  ++stats_.emits;
  if (out_json) {
    *out_json = std::move(*encoded);
  }
  return true;
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

    std::string ignored_json;
    EncodeAndSend(joined, snap.seq, &ignored_json);
    // We DO NOT call OnEmitted on the policy for replays — the
    // snapshot is already in last_/replay_, recording it again
    // would be a no-op for last_ (same state) but would push a
    // duplicate into the replay deque.
  }
}

}  // namespace cursor
}  // namespace cloud_browser

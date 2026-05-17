// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbCursorDcEmitter — M5 R6 (CV2-24) — DataChannel binder for the
// cb-chromium native cursor egress path.
//
// Stitches together the four upstream R# pieces M5 has produced and
// drops their output onto the M3 R5 DataChannel host's "cursor"
// channel (CbDcLabel::kCursor):
//
//   * M5 R3 (CbCursorXyJoin) — fires a JoinedCursorState on every
//                              cursor-change edge.
//   * M5 R4 (cursor::EmitPolicy) — dedupes / debounces the joined
//                              edge, allocates the per-channel wire
//                              seq, records the emitted snapshot for
//                              the reconnect replay path.
//   * M5 R2 (cursor::EnvelopeAssembler) — encodes (type, visible,
//                              x, y, seq) into the cursor-channel v1
//                              JSON envelope.
//   * M3 R5 (CbDataChannelHost) — accepts the encoded string on
//                              CbDcLabel::kCursor; the SCTP DC carries
//                              it to the portal answerer.
//
// R6 is the integration layer; it owns the embedder-side glue and
// nothing else.
//
// # Wiring diagram (steady state, single peer)
//
//   chromium renderer ─────────────────────────────────────────────┐
//                                                                  │
//   ┌──────────────────────────┐  SetCursor(type,vis)               │
//   │ M5 R1 CbCursorClient     │◀─── aura::client::CursorClient ────┘
//   └────────────┬─────────────┘
//                │ CursorChangeCallback(type,vis)
//                ▼
//   ┌──────────────────────────┐  reads M4 R3 snapshot
//   │ M5 R3 CbCursorXyJoin     │
//   └────────────┬─────────────┘
//                │ JoinedCursorState
//                ▼
//   ┌──────────────────────────┐         ┌─────────────────────────┐
//   │ M5 R6 CbCursorDcEmitter  │◀───────▶│ M5 R4 EmitPolicy        │
//   │                          │ Decide  │   dedupe / debounce     │
//   │  OnJoined()              │ Alloc-  │   seq + replay buffer   │
//   │   1. Decide(joined)      │  Seq    └─────────────────────────┘
//   │   2. if !emit return     │
//   │   3. seq = AllocSeq()    │         ┌─────────────────────────┐
//   │   4. json = encode(..,   │────────▶│ M5 R2 EnvelopeAssembler │
//   │            seq)          │ encode  │   AssembleAndEncode     │
//   │   5. dc_host->Send(      │◀────────│         (with seq)      │
//   │            kCursor, json)│  json   └─────────────────────────┘
//   │   6. OnEmitted(joined)   │
//   └────────────┬─────────────┘         ┌─────────────────────────┐
//                │ Send(kCursor, json)   │ M3 R5 CbDataChannelHost │
//                └──────────────────────▶│   kCursor channel       │
//                                        └────────────┬────────────┘
//                                                     │ SCTP / DTLS
//                                                     ▼
//                                              portal answerer
//                                              client/src/cursor.ts
//
// The double-arrow between the emitter and EmitPolicy is the four-
// step bookkeeping R4's header specifies: Decide → AllocSeq → write →
// OnEmitted. R6 owns the ordering — if the DC Send returns an error
// we DO NOT call OnEmitted, so the next joined edge with the same
// payload re-tries cleanly (R4 will not dedupe against a never-
// emitted snapshot because last_ stays untouched).
//
// # CbDataChannelHostObserver pause/resume
//
// R6 also implements CbDataChannelHostObserver and registers itself
// as the host-level observer (via the host's ctor; the host accepts a
// single host-level observer pointer). It uses two state-change
// signals:
//
//   1. OnChannelStateChanged(kCursor, kOpen) → flips
//      cursor_dc_open_ = true, drains any pending JoinedCursorState
//      that arrived while the DC was kConnecting, and replays the
//      EmitPolicy's ReplayBuffer() onto the wire so a freshly
//      reconnected peer sees the most-recent cursor state without
//      waiting for the next renderer-driven change.
//
//   2. OnChannelStateChanged(kCursor, kClosing | kClosed) → flips
//      cursor_dc_open_ = false. Subsequent OnJoined edges still run
//      Decide() (so the EmitPolicy's last_ tracks reality), but Send
//      is short-circuited. The first edge while-closed is retained
//      as pending_; if the channel comes back kOpen we replay it as
//      part of the open-time replay path. If the peer never
//      reconnects this is no-op — the embedder tears the whole
//      session down.
//
// R6 deliberately does NOT use OnAllChannelsOpen(). The host fires
// that beacon when input + stats + cursor + clipboard + file-upload
// have all hit kOpen at least once, but the cursor channel can be
// kOpen well before clipboard / file-upload (M6 R2/R3 are
// optional in v1). Gating cursor emit on AllChannelsOpen() would
// stall the cursor stream behind unrelated channels.
//
// # Threading
//
// All public entry points run on BrowserThread::UI:
//   * OnJoined() fires from R3's CursorChangeCallback dispatch,
//     which fires from aura::client::CursorClient::SetCursor, which
//     fires from the renderer's input-event response on the UI
//     thread.
//   * BindAndStart() / Stop() are called by the embedder's per-peer
//     plumbing constructor / destructor — same UI thread.
//
// Host observer callbacks (OnChannelStateChanged + OnAllChannelsOpen)
// fire on the libwebrtc signaling thread. R6 hops them back onto the
// UI thread with the embedder-provided ui_task_runner_ so the
// emitter's mutable state (cursor_dc_open_, pending_) is touched on
// a single sequence. The hop is mandatory — the host's docs say
// observer fan-out happens on the signaling-task-runner sequence
// and the embedder must not assume that's the UI thread.
//
// The CbDataChannelHost::Send call itself is internally thread-safe
// (its header documents the auto-hop to the signaling thread), so
// R6 can call Send() directly from the UI thread without an
// intermediate PostTask.
//
// # Lifetime
//
// R6 holds raw pointers to all four collaborators (R3 join, R4
// policy, R2 assembler, M3 R5 host). Caller (per-peer plumbing
// struct) owns them all and MUST outlive R6. Stop() unwires the
// R3 callback + the host observer slot before R6 destructs so a
// late callback from either does not land on freed memory.
//
// # Non-goals
//
//   * Wire-envelope shape (R2 owns the JSON layout).
//   * Per-edge emission policy decisions (R4 owns dedupe / debounce
//     / seq / replay).
//   * Pointer position observation (R3 + M4 R3 own the xy source).
//   * Custom-image cursor bytes (R5 will populate
//     custom_image_b64 on the V1EnvelopeView; R6's encode path
//     forwards whatever the assembler produces).
//   * Live E2E test runner — the R6 follow-up will add a browser
//     test that drives a fake CursorClient + a TestDataChannel and
//     asserts the wire bytes round-trip. The DRAFT lands the glue;
//     the test follows.
//
// # Cross-references
//
//   * M3 R5 — capture/signaling/cb_dc_host.{h,cc}
//   * M5 R2 — capture/cursor/cb_cursor_envelope.{h,cc}
//   * M5 R3 — capture/build-integration/cb_cursor_xy_join.{h,cc}
//   * M5 R4 — capture/cursor/cb_cursor_emit_policy.{h,cc}
//   * Legacy reference (deleted in M7):
//       capture/cursor-watcher/main.go (the Go sidecar this path
//       replaces; same wire shape, same dedupe rule, same client
//       renderer at client/src/cursor.ts).

#ifndef CAPTURE_CURSOR_CB_CURSOR_DC_EMITTER_H_
#define CAPTURE_CURSOR_CB_CURSOR_DC_EMITTER_H_

#include <cstdint>
#include <optional>
#include <string>

#include "api/data_channel_interface.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "capture/build-integration/cb_cursor_xy_join.h"
#include "capture/cursor/cb_cursor_emit_policy.h"
#include "capture/cursor/cb_cursor_envelope.h"
#include "capture/signaling/cb_dc_host.h"

namespace cloud_browser {
namespace cursor {

// Counters R6 surfaces for SigNoz / M6 R1's CbStatsRelay. Pull-only
// snapshot — R6 doesn't push these anywhere; the embedder polls when
// it wants to forward them onto the stats channel. Counters monotonic
// over the emitter's lifetime; Stop() does NOT clear them so a final
// snapshot after teardown still reflects the session.
//
// TODO(M5-R6-stats-emit): the M6 R1 emitter wants these as
// "cursor.emit", "cursor.suppress.dedupe", "cursor.suppress.debounce",
// "cursor.send_err", "cursor.dc_replay" attrs. Wire the forward when
// M6 R1's API for cursor-specific stats stabilises — until then the
// stats relay polls this struct via Stats() on its own cadence.
struct EmitterStats {
  // Successful DC writes (host->Send returned kNone).
  uint64_t emits = 0;

  // Decide() returned .emit=false for one of these reasons.
  uint64_t suppress_dedupe = 0;
  uint64_t suppress_debounce = 0;

  // Joined edges suppressed because the cursor DC was not yet kOpen
  // (or had transitioned to kClosing/kClosed). The most recent such
  // edge is retained in pending_ for the next open-time replay.
  uint64_t suppress_dc_closed = 0;

  // CbDataChannelHost::Send returned non-kNone. R6 does NOT call
  // OnEmitted in this case so the next equivalent joined edge will
  // attempt a fresh send.
  uint64_t send_errors = 0;

  // Number of times the open-time replay loop fired (kClosed → kOpen
  // transition with a non-empty ReplayBuffer()). Each replay can
  // emit replay_buffer_capacity records; the counter increments once
  // per transition, not once per replayed record.
  uint64_t dc_replays = 0;
};

// CbCursorDcEmitter
//
// Binds the M5 R3/R4/R2 chain onto the kCursor channel of an
// M3 R5 CbDataChannelHost. One instance per peer connection; lives as
// long as its CbDataChannelHost does.
//
// Sequence (see file-level comment for the full diagram):
//
//   auto emitter = std::make_unique<CbCursorDcEmitter>(
//       /*xy_join=*/&join,
//       /*emit_policy=*/&policy,
//       /*envelope_assembler=*/&assembler,
//       /*dc_host=*/&host,
//       /*ui_task_runner=*/ui_task_runner);
//   emitter->BindAndStart();
//   // ... session runs ...
//   emitter->Stop();
class CbCursorDcEmitter : public signaling::CbDataChannelHostObserver {
 public:
  // |xy_join|              — M5 R3 join. R6 registers itself via
  //                          SetEmitCallback() in BindAndStart.
  // |emit_policy|          — M5 R4 policy instance. R6 owns the
  //                          four-step Decide → AllocSeq → encode →
  //                          OnEmitted dance per emit.
  // |envelope_assembler|   — M5 R2 encoder. R6 uses the
  //                          AssembleAndEncode entry point (see
  //                          TODO(M5-R4-r2-seq-injection) for the
  //                          seq-injection overload R4 specifies).
  // |dc_host|              — M3 R5 host. R6 writes on
  //                          CbDcLabel::kCursor and registers itself
  //                          as the bound observer for kCursor as a
  //                          state-change shim (the v1 kCursor
  //                          contract has no inbound traffic, but
  //                          binding lets R6 see open/close on the
  //                          channel without piggy-backing on the
  //                          host-level observer slot M6 R1 wants).
  // |ui_task_runner|       — UI thread runner. Host observer
  //                          callbacks (signaling thread) hop onto
  //                          this runner before touching emitter
  //                          state.
  //
  // Lifetime: all four pointers are caller-owned and MUST outlive
  // this object. None may be null. The task runner is also caller-
  // owned; typically content::GetUIThreadTaskRunner({}).
  CbCursorDcEmitter(
      CbCursorXyJoin* xy_join,
      EmitPolicy* emit_policy,
      EnvelopeAssembler* envelope_assembler,
      signaling::CbDataChannelHost* dc_host,
      scoped_refptr<base::SequencedTaskRunner> ui_task_runner);

  CbCursorDcEmitter(const CbCursorDcEmitter&) = delete;
  CbCursorDcEmitter& operator=(const CbCursorDcEmitter&) = delete;

  ~CbCursorDcEmitter() override;

  // Register the R3 emit-callback + the host-level observer slot.
  // Idempotent — calling twice replaces with itself. After this
  // returns, joined edges from R3 will flow through R6.
  //
  // Pre-conditions:
  //   * dc_host_->CreateOutboundChannels() has been called (the
  //     kCursor slot must exist). R6 checks IsOpen(kCursor) on the
  //     first joined edge; pre-open edges are retained as pending_.
  //   * xy_join_ is alive and has no other emit-callback registered
  //     (R3's slot is single-occupancy; v1 sets R6 as the sole
  //     consumer).
  //
  // Thread: caller's thread (typically UI). Calling from another
  // thread is undefined.
  void BindAndStart();

  // Unwire the R3 callback + the host observer slot. Safe to call
  // multiple times; safe to call without a prior BindAndStart (no-
  // op). After Stop returns, no further OnJoined / OnChannelState-
  // Changed callbacks reach this object.
  //
  // Called by dtor; exposed so the embedder can tear R6 down
  // explicitly before the host (R7 reconnect path) without dropping
  // the host first.
  void Stop();

  // Pull-only stats snapshot. See EmitterStats above. Thread-safe
  // (atomic-int reads on aligned uint64_t are torn-write-safe on
  // every chromium-supported arch; the embedder reads on whatever
  // thread the M6 R1 stats poll runs on).
  EmitterStats Stats() const;

  // CbDataChannelHostObserver:
  // Hops onto ui_task_runner_ then calls OnChannelStateChanged_Ui.
  void OnChannelStateChanged(
      signaling::CbDcLabel label,
      webrtc::DataChannelInterface::DataState state) override;

  // Test seam — invoked by the unit test to drive a synthetic
  // joined edge without standing up real R1 + R3. Production code
  // MUST NOT call this; production flows through xy_join_'s
  // emit-callback slot R6 registered in BindAndStart.
  void OnJoinedForTesting(const JoinedCursorState& joined);

 private:
  // R3 emit-callback handler. Fires synchronously on the UI thread
  // from R1's CursorChangeCallback dispatch. Runs the four-step
  // Decide → AllocSeq → encode → Send → OnEmitted dance.
  void OnJoined(const JoinedCursorState& joined);

  // UI-thread continuation of OnChannelStateChanged. Updates
  // cursor_dc_open_, drains pending_ on kOpen, replays the
  // EmitPolicy's ReplayBuffer() onto the wire on the kClosed →
  // kOpen edge.
  void OnChannelStateChanged_Ui(
      signaling::CbDcLabel label,
      webrtc::DataChannelInterface::DataState state);

  // Encode |joined| via the R2 assembler with the supplied wire seq
  // and write the result to the kCursor channel. Returns true on
  // successful host->Send. Sets *out_json (when non-null) to the
  // encoded bytes for tracing.
  //
  // Increments stats_.emits or stats_.send_errors. Does NOT call
  // OnEmitted on the policy — caller (OnJoined) owns the policy
  // bookkeeping order.
  bool EncodeAndSend(const JoinedCursorState& joined,
                     int64_t seq,
                     std::string* out_json);

  // Replay the EmitPolicy's ReplayBuffer() onto the wire after a
  // kClosed → kOpen transition. Each replayed snapshot reuses its
  // recorded seq (NOT a freshly-allocated one) so a consumer that
  // already saw the same seq before the reconnect dedupes on its
  // own side; consumers that missed the original see the latest
  // state. Increments stats_.dc_replays once.
  //
  // No-op when ReplayBuffer() is empty (no prior emit) or when the
  // policy is configured with replay_buffer_capacity = 0.
  void ReplayOnOpen();

  // Collaborators — all caller-owned, all must outlive `this`.
  const raw_ptr<CbCursorXyJoin> xy_join_;
  const raw_ptr<EmitPolicy> emit_policy_;
  const raw_ptr<EnvelopeAssembler> envelope_assembler_;
  const raw_ptr<signaling::CbDataChannelHost> dc_host_;
  const scoped_refptr<base::SequencedTaskRunner> ui_task_runner_;

  // True when the kCursor channel has hit kOpen and has not yet
  // transitioned to kClosing/kClosed. Touched on the UI thread only.
  bool cursor_dc_open_ = false;

  // Whether BindAndStart has run + Stop has not. Lets dtor + Stop
  // be idempotent.
  bool started_ = false;

  // Most recent joined edge suppressed because the cursor DC wasn't
  // open. Drained on the next kOpen transition (replayed through the
  // full Decide → encode → Send path; the policy will likely emit it
  // since last_ on the policy still tracks pre-disconnect state).
  //
  // Single slot is sufficient: while the DC is closed, every fresh
  // joined edge supersedes the previous one — only the most-recent
  // state matters for a self-describing wire shape (the cursor
  // protocol carries no incremental updates). Multiple buffered
  // states across a long disconnect would just emit identical
  // dedupe-eligible payloads after Decide(); R6's pending_ already
  // collapses that to one.
  std::optional<JoinedCursorState> pending_while_closed_;

  // Pull-only stats. uint64_t aligned reads are torn-write-safe;
  // writes happen only on the UI thread so no fencing is required.
  // TODO(M5-R6-stats-thread): if a future caller decides to poll
  // Stats() off-thread under heavy DC traffic, promote to
  // std::atomic<uint64_t> per counter.
  EmitterStats stats_;

  SEQUENCE_CHECKER(ui_sequence_);

  base::WeakPtrFactory<CbCursorDcEmitter> weak_factory_{this};
};

}  // namespace cursor
}  // namespace cloud_browser

#endif  // CAPTURE_CURSOR_CB_CURSOR_DC_EMITTER_H_

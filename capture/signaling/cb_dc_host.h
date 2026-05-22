// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Native browser-process DataChannel host — M3 R5 (CV2-55).
//
// Owns the post-handshake DataChannels on the cb-chromium peer's single
// PeerConnection. The offerer driver (M3 R4) constructs the PC and runs
// the SDP+ICE dance; this host runs *inside* that dance — between
// driver.Start() and the first OnRenegotiationNeeded() — to create the
// outbound DCs so they appear in the FIRST offer SDP, then provides the
// observer-binding + per-channel Send() API that the consumer modules
// (M4 input, M5 cursor, M6 stats / clipboard / file-upload) wire onto.
//
// # Why a separate object
//
// libwebrtc's DataChannelInterface is single-observer: each channel
// has one RegisterObserver / UnregisterObserver slot. The consumer
// modules (CbInputDispatch, CbStatsRelay, CbCursorRelay, …) each
// implement webrtc::DataChannelObserver and want to be that observer.
// But the M3 host *also* needs to learn `open` / `close` state per
// channel so M6's CbWebrtcEventEmitter can fire dc.opened beacons
// (with handshake_ms on the first one), and so the embedder's
// readiness gate can know when "all DCs are open" lights up.
//
// The host solves this with a per-channel trampoline observer
// (CbDataChannelHost::ChannelObserver, private impl) that the host
// registers as the channel's *real* observer; the trampoline forwards
// OnMessage / OnBufferedAmountChange / IsOkToCallOnTheNetworkThread
// to the consumer-bound observer verbatim, and additionally updates
// host state + notifies the host-level observer on the libwebrtc
// signaling thread.
//
// # The five channels (labels are exact + case-sensitive)
//
// Locked to capture/streamer-page/streamer.js:1858-1869 because the
// portal answerer's `pc.ondatachannel` handler routes on these literal
// label strings — any drift silently lands in the portal's `default
// =>` log-and-drop arm:
//
//   "input"        — inbound text envelopes (M4). One observer:
//                    CbInputDispatch.
//   "stats"        — inbound text frames forwarded to the metrics
//                    sidecar (M6 R1). One observer: CbStatsRelay.
//   "cursor"       — one-way EMIT from the browser to the portal
//                    (M5 R2). The host emits on this channel; the
//                    consumer-bound observer (if any) only sees
//                    open/close + buffered-amount changes — there is
//                    no inbound traffic on this label in v1.
//   "clipboard"    — bidirectional (M6 R2 / not in R1). One observer
//                    when M6 R2 lands; until then, an
//                    unbound-default trampoline drops inbound.
//   "files"        — one-way RECEIVE from the portal (M6 R3 / not in
//                    R1). Same unbound-default behaviour as
//                    clipboard pre-R2.
//
// # Channel-creation ordering (must precede the first offer SDP)
//
//   1. Embedder constructs CbOffererDriver and CbDataChannelHost,
//      passing driver.pc() into the host ctor.
//   2. Embedder calls host.CreateOutboundChannels() — this issues
//      five pc->CreateDataChannelOrError() calls with `{ordered:true}`
//      and the canonical labels.
//   3. Embedder calls host.BindObserver(label, consumer) for each
//      label whose R# consumer is alive at this point (R1 timing:
//      input + stats + cursor are present, clipboard + file-upload
//      bind in M6 R2/R3).
//   4. The video transceiver from M2 is added.
//   5. The driver's first OnRenegotiationNeeded() fires; CreateOffer
//      includes the channels because step 2 happened before this.
//
// Any reordering breaks the contract — the offerer driver's docs are
// explicit that the embedder is responsible for getting all the
// `m=` lines + DC entries into the first SDP. The host enforces this
// by refusing CreateOutboundChannels() after the PC is past
// kHaveLocalOffer (asserted in DCHECK; release builds log + bail).
//
// # Threading
//
// The host is constructed + driven from the embedder's UI thread (the
// thread that drives CbOffererDriver). All consumer-facing Send()
// calls accept calls from any thread — internally they hop onto the
// libwebrtc signaling thread via rtc::Thread::BlockingCall before
// touching dc->Send(), because DataChannelInterface::Send is only safe
// on the signaling thread. ChannelObserver::OnMessage,
// OnBufferedAmountChange, and OnStateChange arrive on the libwebrtc
// signaling thread and are forwarded synchronously to the consumer.
//
// # Lifetime
//
// The host does NOT own the PC — that's the offerer driver's
// scoped_refptr. The host holds its OWN scoped_refptr to each
// DataChannelInterface returned by CreateDataChannelOrError so the
// channels outlive the PC's natural teardown order long enough for
// the host to UnregisterObserver before drop. The consumer observers
// are NOT owned — caller (the embedder) keeps them alive past the
// host's destruction; host dtor UnregisterObservers every channel
// before dropping its refs.
//
// # Cross-references
//
//   * M1  — capture/build-integration/cloud_browser_pcf.{h,cc}
//   * M3 R1 — capture/signaling/cb_wire_envelope.{h,cc}    (route key types)
//   * M3 R2 — capture/signaling/cb_signaling_ws_client.{h,cc}
//   * M3 R3 — capture/signaling/cb_ice_config.{h,cc}
//   * M3 R4 — capture/signaling/cb_offerer_driver.{h,cc}   (owns pc)
//   * M4 R1 — capture/build-integration/cb_input_dispatch.{h,cc}
//   * M5 R1/R2 — capture/build-integration/cb_cursor_client.{h,cc}
//   * M6 R1 — capture/build-integration/cb_stats_relay.{h,cc}
//             (and CbWebrtcEventEmitter for dc.opened beacons)
//   * Legacy reference (deleted in M7):
//       capture/streamer-page/streamer.js:1858-1869
//
// # Non-goals
//
//   * Inbound message decode — that's the consumer module's job
//     (CbInputDispatch owns envelope decode for "input"; CbStatsRelay
//     just forwards "stats" raw bodies; the M6 R2/R3 consumers will
//     own clipboard/file-upload decode).
//   * Outbound message *construction* — same: M5 R2 owns the cursor
//     JSON envelope shape, M6 R2 owns the clipboard envelope, etc.
//     The host's Send() takes an already-built std::string text
//     payload (or webrtc::CopyOnWriteBuffer for binary).
//   * SCTP reliability / retransmit tuning — `{ordered: true}` is the
//     v1 contract; if a future channel needs unordered/lossy, the
//     factory grows a new entrypoint, the existing five do not change.
//   * Renegotiation. Same as M3 R4 — v1 has no support; if a future
//     R# adds a new channel mid-session, it lands on a new offer.

#ifndef CAPTURE_SIGNALING_CB_DC_HOST_H_
#define CAPTURE_SIGNALING_CB_DC_HOST_H_

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "api/data_channel_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/scoped_refptr.h"
#include "base/containers/flat_set.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/thread.h"

namespace cloud_browser::signaling {

// The five canonical DC labels. Kept in offer-SDP creation order so
// label_id (size_t) doubles as the SDP track index — matches what
// streamer.js historically produced. Adding a new label is a wire-
// contract change: bump the index, update portal answerer, never
// reorder existing entries.
enum class CbDcLabel : uint8_t {
  kInput = 0,
  kStats = 1,
  kCursor = 2,
  kClipboard = 3,
  kFiles = 4,
};

// Total number of DCs the host creates. The array<,kNumChannels>
// fields below all use this as their fixed size — adding a new
// channel grows the enum + this constant in lockstep.
inline constexpr size_t kNumChannels = 5;

// Default subset of labels opened by CreateOutboundChannels() when the
// caller doesn't pass an explicit set. Mirrors the historical v1
// behavior — open all five channels — so existing callers (the brief
// confirms there are none in production today, but tests and future
// code paths get the same shape) see no behavioral change.
//
// Wave 1.5 (CV2-77 cb_dc_host adoption) will pass a narrower set that
// omits kFiles (and possibly kClipboard) until the M6 R2/R3
// consumers ship. That call site is the first production caller; the
// default here keeps the no-arg invocation identical to the prior
// hard-coded loop.
//
// Returned by const-ref to a function-local static so the constant has
// a stable address + no static-init-order trap (base::flat_set has a
// non-trivial constructor — declaring as a namespace-scope global
// const is unsafe across TU init order).
const base::flat_set<CbDcLabel>& DefaultOutboundLabels();

// Exact label string emitted on the wire. Locked against
// streamer.js:1858 + :1866-1868. The portal answerer routes on these
// strings; drift here = silent feature break.
const char* LabelToString(CbDcLabel label);

// Reverse mapping. Returns std::nullopt for unknown labels. Used by
// the host's OnDataChannel-from-driver path (currently a no-op:
// answerer-created channels are not part of the v1 protocol, but the
// path exists so any portal-side regression that *does* open a remote
// channel surfaces as an explicit log rather than silent acceptance).
std::optional<CbDcLabel> LabelFromString(std::string_view s);

// Optional host-level observer. Fires on the libwebrtc signaling thread
// the host was constructed with.
//
// The primary client is M6 R1's CbWebrtcEventEmitter — it wires
// EmitDcOpened() in OnChannelStateChanged(label, kOpen), and the
// FIRST kOpen across all channels carries the handshake_ms attr.
//
// Lifetime: caller-owned; must outlive the host. Pass nullptr in
// tests that don't exercise the lifecycle beacon path.
class CbDataChannelHostObserver {
 public:
  virtual ~CbDataChannelHostObserver() = default;

  // Per-channel state transitions. State values pass through
  // verbatim — observer reads them with the standard
  // webrtc::DataChannelInterface::DataState comparisons.
  // TODO(M3-R5-state-coalesce): if M6 R1's emitter needs to dedup
  // bouncy state transitions (kConnecting → kOpen → kClosing →
  // kOpen on flaky networks), do that on the emitter side, not
  // here — the host's job is faithful forwarding, not policy.
  virtual void OnChannelStateChanged(
      CbDcLabel label,
      webrtc::DataChannelInterface::DataState state) {}

  // All five channels have reached kOpen at least once during this
  // session. Latched: a second open of any channel after a partial-
  // close does not re-fire. The embedder's readiness gate uses this
  // to flip "session is live" semantics, and M6 R1 reads it to gate
  // the first stats-relay forward.
  virtual void OnAllChannelsOpen() {}
};

// Send result. RTCError is the libwebrtc-native error type; the host
// returns kInvalidState when the channel isn't bound to a created
// DataChannel yet (CreateOutboundChannels() not called), kInvalidRange
// when the requested label is out of bounds (can't happen with the
// enum — guarded as defence-in-depth), and the libwebrtc-native error
// returned by DataChannelInterface::Send for any send-time failure.
using SendResult = webrtc::RTCError;
using SendAsyncCallback =
    base::OnceCallback<void(bool ok, std::string message)>;

// CbDataChannelHost
//
// Construct one per PeerConnection. The host registers itself as the
// observer on every channel it creates; consumer observers are
// FORWARDED through (see ChannelObserver impl in the .cc).
//
// Sequence (see file-level comment for the full embedder
// interleaving):
//
//   auto host = std::make_unique<CbDataChannelHost>(
//       driver.pc(), m6_event_emitter.get(),
//       signaling_thread);
//   host->CreateOutboundChannels();           // before first offer
//   host->BindObserver(CbDcLabel::kInput,  input_dispatch.get());
//   host->BindObserver(CbDcLabel::kStats,  stats_relay.get());
//   host->BindObserver(CbDcLabel::kCursor, cursor_client_or_null);
//   // M2 video transceiver added by embedder here.
//   driver.Start();                           // triggers offer SDP
//
// After Start(): outbound emit via host->Send(kCursor, json_text);
// inbound dispatch is driven by libwebrtc invoking the consumer's
// observer through the host trampoline.
class CbDataChannelHost {
 public:
  // |pc|: the offerer driver's PeerConnection (driver.pc()). The host
  //     takes a scoped_refptr to keep it alive across host lifetime
  //     even if the driver enters kFailed mid-session — the host's
  //     UnregisterObserver / dc ref drop must outlive the PC's
  //     teardown to avoid use-after-free in the libwebrtc signaling
  //     thread's late callbacks.
  // |host_observer|: optional CbDataChannelHostObserver — typically
  //     points at M6 R1's CbWebrtcEventEmitter. nullptr is fine.
  // |signaling_thread|: the libwebrtc signaling thread that owns the
  //     PeerConnection proxy. The host marshals all PC/DC mutations
  //     through this thread with BlockingCall so embedder/UI callers
  //     never touch DataChannelInterface from the wrong sequence.
  //     Must outlive the host.
  CbDataChannelHost(webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
                    CbDataChannelHostObserver* host_observer,
                    webrtc::Thread* signaling_thread);

  CbDataChannelHost(const CbDataChannelHost&) = delete;
  CbDataChannelHost& operator=(const CbDataChannelHost&) = delete;

  ~CbDataChannelHost();

  // Create outbound channels on the PC for the labels in |labels|.
  // Must be called exactly once, BEFORE the offerer driver fires
  // CreateOffer. Returns kNone on success; on failure returns the
  // first DataChannelOrError error and the host stays in a half-
  // constructed state (channels created so far are kept; the embedder
  // must tear the entire session down — partial-DC sessions are not a
  // supported mode in v1).
  //
  // |labels| selects which of the five canonical labels get opened.
  // Default (DefaultOutboundLabels()) is all five — preserves the
  // historical v1 behavior. Wave 1.5 will pass a narrower set so the
  // M6 R2/R3 channels (clipboard / file-upload) stay closed until
  // their consumer modules ship. Labels NOT in |labels| are left as
  // empty slots in slots_ — IsOpen() returns false, Send() returns
  // kInvalidState (channel not created), BindObserver() still
  // succeeds slot-side but no traffic ever flows because the
  // libwebrtc DC doesn't exist.
  //
  // Thread: caller's thread; internally hops onto signaling_thread_
  // for the actual CreateDataChannelOrError calls because libwebrtc
  // requires PC mutation on the signaling thread. Returns
  // synchronously — the embedder's call site BLOCKS on the post.
  // TODO(M3-R5-async-create): if benchmarks show the synchronous
  // wait is too long (>10ms for 5 channels), split into a
  // CreateOutboundChannelsAsync() with a completion callback. R1
  // measurement says <1ms typical, so synchronous is fine for now.
  webrtc::RTCError CreateOutboundChannels(
      const base::flat_set<CbDcLabel>& labels = DefaultOutboundLabels());

  // Bind |observer| as the inbound observer for |label|. Replaces
  // any prior binding (test seam — production binds once).
  // Thread-safe; takes obs_lock_ to swap the slot.
  //
  // The host's per-channel ChannelObserver trampoline forwards
  // OnMessage / OnBufferedAmountChange / IsOkToCallOnTheNetworkThread
  // to whatever observer is bound at the moment libwebrtc invokes the
  // trampoline. OnStateChange is forwarded to BOTH the bound observer
  // and the host_observer.
  //
  // |observer| MUST outlive the host (or be unbound via BindObserver
  // (label, nullptr) before destruction). Caller-owned.
  //
  // TODO(M3-R5-bind-multi): if M6 R2+ ever needs multiple observers
  // on the same channel (e.g. clipboard's metrics tap + actual decode
  // sink), grow this into AddObserver / RemoveObserver with an
  // internal fan-out vector. R1 design is single-observer because
  // every existing consumer wants single-observer semantics and the
  // multi-observer case has no concrete user yet.
  void BindObserver(CbDcLabel label, webrtc::DataChannelObserver* observer);

  // True iff CreateOutboundChannels() succeeded AND the channel for
  // |label| reached kOpen at least once. Used by the embedder's
  // readiness gate. Latched on first-open per the
  // OnAllChannelsOpen() contract.
  bool IsOpen(CbDcLabel label) const;

  // True iff every channel has hit kOpen at least once. Latched.
  bool AllChannelsOpen() const;

  // Text-frame emit on |label|. Hops onto signaling_thread_ if the
  // caller isn't already on it, then invokes dc->Send(buf,
  // is_binary=false). Returns kNone on a successful POST to the
  // libwebrtc Send queue (NOT a successful network deliver — the
  // SCTP stack does the rest fire-and-forget); returns kInvalidState
  // if the channel isn't kOpen, or the libwebrtc error from
  // DataChannelInterface::Send otherwise.
  //
  // TODO(M3-R5-backpressure): expose BufferedAmount() / a
  // GetBufferedAmount(label) accessor for M5 R3+ to back off cursor
  // emit when the SCTP buffer balloons. R1 ships fire-and-forget;
  // M5 R3 will add the gate.
  SendResult Send(CbDcLabel label, std::string_view text);

  // Async text-frame emit. Posts the actual DataChannel send onto
  // signaling_thread_ and returns immediately to the caller. |callback|
  // is invoked on |reply_runner| with the same queued/not-queued
  // semantics as Send(). Use this from UI hot paths that may run
  // inside Chromium posted-task scopes that disallow blocking waits.
  void SendAsync(CbDcLabel label,
                 std::string text,
                 scoped_refptr<base::SequencedTaskRunner> reply_runner,
                 SendAsyncCallback callback);

  // Binary-frame emit on |label|. Same threading + error model as
  // text Send; calls dc->Send(buf, is_binary=true). The binary path
  // is reserved for future use (M6 R3 file-upload's reverse-direction
  // ACKs may want it); none of the v1 channels uses it.
  SendResult SendBinary(CbDcLabel label, webrtc::CopyOnWriteBuffer buffer);

  // Drop all channel refs + unregister observers. Called by the
  // dtor; exposed publicly so the embedder can tear the host down
  // explicitly before the PC (R7 reconnect path; not used in v1).
  // After Shutdown(), every public method except IsOpen / dtor
  // returns kInvalidState; the host is single-shot.
  void Shutdown();

 private:
  // Per-channel trampoline observer. Lives as long as the host;
  // registered as the dc's observer immediately after channel
  // creation. Forwards OnMessage / OnBufferedAmountChange /
  // IsOkToCallOnTheNetworkThread to whatever consumer is bound;
  // OnStateChange updates host state + fans out to host_observer.
  class ChannelObserver;

  // Backing storage. Index = static_cast<size_t>(CbDcLabel).
  struct ChannelSlot {
    webrtc::scoped_refptr<webrtc::DataChannelInterface> dc;
    std::unique_ptr<ChannelObserver> trampoline;
    bool ever_opened = false;  // latched on first kOpen
  };

  // Hop helper — RAII-style: invokes |fn| synchronously if we're
  // already on signaling_thread_, else BlockingCall + wait.
  void RunOnSignalingSync(base::OnceClosure fn);

  // ChannelObserver callbacks — invoked on signaling thread.
  void OnChannelStateChanged_Signaling(CbDcLabel label);
  void OnChannelMessage_Signaling(CbDcLabel label,
                                  const webrtc::DataBuffer& buffer);
  void OnChannelBufferedAmountChange_Signaling(CbDcLabel label,
                                               uint64_t sent_data_size);

  // Member ordering is destruction-order-significant: |slots_| must
  // destruct BEFORE |pc_| so trampoline UnregisterObserver lands on
  // a still-alive DataChannel that's still attached to a still-alive
  // PC. The dtor runs Shutdown() first, which is belt-and-braces.
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
  raw_ptr<CbDataChannelHostObserver> host_observer_;
  raw_ptr<webrtc::Thread> signaling_thread_;

  // obs_lock_ guards observer slot mutation in BindObserver +
  // observer slot read in the ChannelObserver trampoline. Held only
  // for the slot swap / load — never across a consumer-observer
  // callback (which would deadlock if a consumer calls back into the
  // host's Send()).
  mutable base::Lock obs_lock_;
  std::array<raw_ptr<webrtc::DataChannelObserver>, kNumChannels>
      bound_observers_ = {};

  // slots_lock_ guards slots_ during CreateOutboundChannels +
  // Shutdown. Steady-state reads from the signaling thread do NOT
  // take the lock — slots_ is immutable between Create and Shutdown
  // when accessed from the signaling sequence.
  mutable base::Lock slots_lock_;
  std::array<ChannelSlot, kNumChannels> slots_;

  // Latched state. Mutated only from the signaling sequence;
  // public accessors read without locking because bool/atomic-int
  // reads are torn-write-safe on every chromium-supported arch.
  bool all_open_latched_ = false;

  // Embedder-call-ordering DCHECK. CreateOutboundChannels() must
  // only fire once; subsequent calls return kInvalidState.
  bool create_called_ = false;

  SEQUENCE_CHECKER(public_api_sequence_);

  base::WeakPtrFactory<CbDataChannelHost> weak_factory_{this};
};

}  // namespace cloud_browser::signaling

#endif  // CAPTURE_SIGNALING_CB_DC_HOST_H_

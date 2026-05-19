// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Implementation of capture/signaling/cb_dc_host.h — M3 R5 (CV2-55).

#include "capture/signaling/cb_dc_host.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/bind_post_task.h"

namespace cloud_browser::signaling {

namespace {

// Canonical wire labels. Locked to capture/streamer-page/streamer.js
// :1858 + :1866-1868. Index alignment with CbDcLabel enum is
// load-bearing — see kLabelStrings access in LabelToString().
constexpr std::array<const char*, kNumChannels> kLabelStrings = {
    "input", "stats", "cursor", "clipboard", "files",
};

}  // namespace

const char* LabelToString(CbDcLabel label) {
  const auto idx = static_cast<size_t>(label);
  CHECK_LT(idx, kNumChannels);
  return kLabelStrings[idx];
}

std::optional<CbDcLabel> LabelFromString(std::string_view s) {
  for (size_t i = 0; i < kNumChannels; ++i) {
    if (s == kLabelStrings[i]) {
      return static_cast<CbDcLabel>(i);
    }
  }
  return std::nullopt;
}

const base::flat_set<CbDcLabel>& DefaultOutboundLabels() {
  // Function-local static — initialized on first call, no static-init
  // order trap. Contains every canonical label so the no-arg
  // CreateOutboundChannels() default matches the historical v1
  // hard-coded-loop behavior. Wave 1.5 (CV2-77 adoption) will pass a
  // narrower set; this default remains the safe fallback for tests
  // and any future caller that wants the full five-channel shape.
  static const base::NoDestructor<base::flat_set<CbDcLabel>> kSet(
      base::flat_set<CbDcLabel>{
          CbDcLabel::kInput,
          CbDcLabel::kStats,
          CbDcLabel::kCursor,
          CbDcLabel::kClipboard,
          CbDcLabel::kFiles,
      });
  return *kSet;
}

// ---------------------------------------------------------------------
// ChannelObserver — per-channel trampoline.
//
// One instance per CbDcLabel. Held by ChannelSlot::trampoline. The
// host registers `this` as the DataChannelInterface's observer
// immediately after CreateDataChannelOrError; the consumer observer
// (M4 R1 dispatch, M5 R2 cursor, M6 R1 stats relay, …) is fanned out
// to via the bound_observers_ slot the host holds.
//
// Threading: every method here is invoked on libwebrtc's signaling
// thread per the DataChannelObserver contract. We grab the bound
// observer slot under obs_lock_ for the duration of the slot READ
// only — never across the consumer callback itself, so the consumer
// can call back into host->Send() without deadlocking.
// ---------------------------------------------------------------------
class CbDataChannelHost::ChannelObserver
    : public webrtc::DataChannelObserver {
 public:
  ChannelObserver(CbDataChannelHost* host, CbDcLabel label)
      : host_(host), label_(label) {}

  ChannelObserver(const ChannelObserver&) = delete;
  ChannelObserver& operator=(const ChannelObserver&) = delete;

  ~ChannelObserver() override = default;

  void OnStateChange() override {
    // Forward to bound consumer (if any) WITHOUT holding obs_lock_
    // across the call — see file-level threading note.
    webrtc::DataChannelObserver* consumer = nullptr;
    {
      base::AutoLock lock(host_->obs_lock_);
      consumer = host_->bound_observers_[static_cast<size_t>(label_)];
    }
    if (consumer) {
      consumer->OnStateChange();
    }
    // Update host-side latched state + notify host_observer. Always
    // runs on signaling thread (we are signaling thread); the helper
    // does the actual work.
    host_->OnChannelStateChanged_Signaling(label_);
  }

  void OnMessage(const webrtc::DataBuffer& buffer) override {
    webrtc::DataChannelObserver* consumer = nullptr;
    {
      base::AutoLock lock(host_->obs_lock_);
      consumer = host_->bound_observers_[static_cast<size_t>(label_)];
    }
    if (consumer) {
      consumer->OnMessage(buffer);
    } else {
      // Drop. The clipboard / file-upload paths land here in R1
      // because their consumer modules don't exist yet (M6 R2/R3).
      // Drop quietly — logging on every inbound frame would flood
      // the chromium pod log when the portal client starts sending
      // pings on those channels. The M6 modules will bind their
      // observers + take over.
      // TODO(M3-R5-unbound-metric): wire a counter so M6 R2/R3
      // landing can prove they took over (counter resets to 0 on
      // bind + stays 0 thereafter).
    }
  }

  void OnBufferedAmountChange(uint64_t sent_data_size) override {
    webrtc::DataChannelObserver* consumer = nullptr;
    {
      base::AutoLock lock(host_->obs_lock_);
      consumer = host_->bound_observers_[static_cast<size_t>(label_)];
    }
    if (consumer) {
      consumer->OnBufferedAmountChange(sent_data_size);
    }
    host_->OnChannelBufferedAmountChange_Signaling(label_, sent_data_size);
  }

  bool IsOkToCallOnTheNetworkThread() override {
    // Mirror the bound consumer's answer if any; otherwise default
    // to false (= libwebrtc will dispatch on the signaling thread).
    // Per the libwebrtc contract this is a constant per observer —
    // we forward to the consumer's choice so the consumer's
    // documentation stays load-bearing.
    webrtc::DataChannelObserver* consumer = nullptr;
    {
      base::AutoLock lock(host_->obs_lock_);
      consumer = host_->bound_observers_[static_cast<size_t>(label_)];
    }
    return consumer ? consumer->IsOkToCallOnTheNetworkThread() : false;
  }

 private:
  // Raw back-pointer — the host outlives every ChannelObserver
  // because the trampolines live INSIDE the host's slots_ array.
  // Lifetime guard via the host's UnregisterObserver-before-drop
  // discipline in Shutdown().
  const raw_ptr<CbDataChannelHost> host_;
  const CbDcLabel label_;
};

// ---------------------------------------------------------------------
// CbDataChannelHost
// ---------------------------------------------------------------------

CbDataChannelHost::CbDataChannelHost(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
    CbDataChannelHostObserver* host_observer,
    scoped_refptr<base::SequencedTaskRunner> signaling_task_runner)
    : pc_(std::move(pc)),
      host_observer_(host_observer),
      signaling_task_runner_(std::move(signaling_task_runner)) {
  DCHECK(pc_);
  DCHECK(signaling_task_runner_);
  DETACH_FROM_SEQUENCE(public_api_sequence_);
}

CbDataChannelHost::~CbDataChannelHost() {
  Shutdown();
}

void CbDataChannelHost::RunOnSignalingSync(base::OnceClosure fn) {
  if (signaling_task_runner_->RunsTasksInCurrentSequence()) {
    std::move(fn).Run();
    return;
  }
  base::WaitableEvent done(
      base::WaitableEvent::ResetPolicy::MANUAL,
      base::WaitableEvent::InitialState::NOT_SIGNALED);
  signaling_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::OnceClosure inner, base::WaitableEvent* d) {
            std::move(inner).Run();
            d->Signal();
          },
          std::move(fn), &done));
  done.Wait();
}

webrtc::RTCError CbDataChannelHost::CreateOutboundChannels(
    const base::flat_set<CbDcLabel>& labels) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(public_api_sequence_);

  if (create_called_) {
    return webrtc::RTCError(webrtc::RTCErrorType::INVALID_STATE,
                            "CreateOutboundChannels already called");
  }
  create_called_ = true;

  // PC must be present + not yet past local-offer. Cheap check —
  // libwebrtc would refuse channel creation post-local-offer with a
  // worse error anyway; we surface a clearer one. The full ordering
  // contract lives in the header file-level comment.
  if (!pc_) {
    return webrtc::RTCError(webrtc::RTCErrorType::INVALID_STATE,
                            "PC is null");
  }

  webrtc::RTCError first_error;

  // Hop onto the signaling thread for the actual mutation.
  // TODO(M3-R5-async-create): see header — synchronous is fine for
  // R1 given the 5-channel cost.
  RunOnSignalingSync(base::BindOnce(
      [](CbDataChannelHost* self, const base::flat_set<CbDcLabel>* labels,
         webrtc::RTCError* err) {
        webrtc::DataChannelInit init;
        init.ordered = true;
        // All other DataChannelInit fields default — `maxRetransmits`
        // / `maxPacketLifeTime` unset = SCTP-default reliable mode,
        // matching streamer.js `{ordered: true}` semantics. If a
        // future channel wants unordered/lossy, it grows a new init
        // here; existing five stay reliable-ordered.

        base::AutoLock lock(self->slots_lock_);
        for (size_t i = 0; i < kNumChannels; ++i) {
          const auto label = static_cast<CbDcLabel>(i);
          if (!labels->contains(label)) {
            // Caller opted out of this label — leave the slot empty.
            // IsOpen() returns false; Send() returns kInvalidState
            // (channel not created); the bound_observers_ slot can
            // still be set via BindObserver but no inbound traffic
            // will ever arrive because the libwebrtc DC doesn't
            // exist on the PC. This is the intentional Wave 1.5
            // shape: omit kFiles (and possibly kClipboard)
            // until the M6 R2/R3 consumers land.
            VLOG(1) << "[M3-R5] skipped `" << LabelToString(label)
                    << "` per caller opt-out";
            continue;
          }
          const std::string label_str = LabelToString(label);

          auto dc_or_err = self->pc_->CreateDataChannelOrError(
              label_str, &init);
          if (!dc_or_err.ok()) {
            LOG(ERROR) << "[M3-R5] CreateDataChannelOrError(" << label_str
                       << ") failed: " << dc_or_err.error().message();
            if (err->ok()) {
              *err = dc_or_err.MoveError();
            }
            continue;
          }

          auto& slot = self->slots_[i];
          slot.dc = dc_or_err.MoveValue();
          slot.trampoline =
              std::make_unique<ChannelObserver>(self, label);
          slot.dc->RegisterObserver(slot.trampoline.get());
          VLOG(1) << "[M3-R5] created + registered observer for `"
                  << label_str << "` (sctp id=" << slot.dc->id() << ")";
        }
      },
      this, &labels, &first_error));

  return first_error;
}

void CbDataChannelHost::BindObserver(
    CbDcLabel label,
    webrtc::DataChannelObserver* observer) {
  // Public API — any thread.
  base::AutoLock lock(obs_lock_);
  bound_observers_[static_cast<size_t>(label)] = observer;
  // We do NOT replay missed state-change events to a late-binding
  // observer; the consumer is expected to be bound before the
  // channels reach kOpen. If a late-bind regression class surfaces,
  // grow this into a replay-buffered bind — for now, simple slot swap.
  // TODO(M3-R5-late-bind-replay): see above.
}

bool CbDataChannelHost::IsOpen(CbDcLabel label) const {
  base::AutoLock lock(slots_lock_);
  const auto& slot = slots_[static_cast<size_t>(label)];
  return slot.dc && slot.dc->state() ==
                        webrtc::DataChannelInterface::DataState::kOpen;
}

bool CbDataChannelHost::AllChannelsOpen() const {
  // Read of bool is torn-write-safe; no lock needed.
  return all_open_latched_;
}

SendResult CbDataChannelHost::Send(CbDcLabel label,
                                   std::string_view text) {
  // Capture by value so the closure owns the bytes for the duration
  // of the hop. The string_view at the call site can vanish.
  std::string owned(text);

  webrtc::RTCError result;

  RunOnSignalingSync(base::BindOnce(
      [](CbDataChannelHost* self, CbDcLabel label, std::string body,
         webrtc::RTCError* out) {
        webrtc::scoped_refptr<webrtc::DataChannelInterface> dc;
        {
          base::AutoLock lock(self->slots_lock_);
          dc = self->slots_[static_cast<size_t>(label)].dc;
        }
        if (!dc) {
          *out = webrtc::RTCError(webrtc::RTCErrorType::INVALID_STATE,
                                  "channel not created");
          return;
        }
        if (dc->state() !=
            webrtc::DataChannelInterface::DataState::kOpen) {
          *out = webrtc::RTCError(webrtc::RTCErrorType::INVALID_STATE,
                                  "channel not open");
          return;
        }
        // DataBuffer with is_binary=false.
        webrtc::DataBuffer buf(
            webrtc::CopyOnWriteBuffer(body.data(), body.size()),
            /*binary=*/false);
        // CV2-75 fix-forward (resolves M3-R5-sendasync-fallback TODO):
        // chromium-pinned libwebrtc's DataChannelInterface::SendAsync
        // returns void (not RTCError); the on_complete callback is
        // the out-of-band signal. For fire-and-forget queueing, the
        // empty {} callback is correct, and the synchronous-return
        // contract collapses to "POSTed to queue". The M3 R5 design
        // documents the success contract as "queued, not delivered" —
        // we synthesize the OK result on the synchronous path.
        dc->SendAsync(std::move(buf), /*on_complete=*/{});
        *out = webrtc::RTCError::OK();
      },
      this, label, std::move(owned), &result));

  return result;
}

SendResult CbDataChannelHost::SendBinary(CbDcLabel label,
                                         webrtc::CopyOnWriteBuffer buffer) {
  webrtc::RTCError result;

  RunOnSignalingSync(base::BindOnce(
      [](CbDataChannelHost* self, CbDcLabel label,
         webrtc::CopyOnWriteBuffer body, webrtc::RTCError* out) {
        webrtc::scoped_refptr<webrtc::DataChannelInterface> dc;
        {
          base::AutoLock lock(self->slots_lock_);
          dc = self->slots_[static_cast<size_t>(label)].dc;
        }
        if (!dc) {
          *out = webrtc::RTCError(webrtc::RTCErrorType::INVALID_STATE,
                                  "channel not created");
          return;
        }
        if (dc->state() !=
            webrtc::DataChannelInterface::DataState::kOpen) {
          *out = webrtc::RTCError(webrtc::RTCErrorType::INVALID_STATE,
                                  "channel not open");
          return;
        }
        webrtc::DataBuffer buf(std::move(body), /*binary=*/true);
        // CV2-75 fix-forward: same SendAsync void-return resolution as
        // the text Send() above. Binary path is currently unused (v1
        // channels are text-only); kept symmetric for the future
        // binary file-upload-ack path (M6 R3 follow-up).
        dc->SendAsync(std::move(buf), /*on_complete=*/{});
        *out = webrtc::RTCError::OK();
      },
      this, label, std::move(buffer), &result));

  return result;
}

void CbDataChannelHost::Shutdown() {
  // Hop onto signaling to unregister + drop. Synchronous — once this
  // returns the host has dropped its refs + the trampolines are
  // detached from libwebrtc.
  RunOnSignalingSync(base::BindOnce(
      [](CbDataChannelHost* self) {
        base::AutoLock lock(self->slots_lock_);
        for (auto& slot : self->slots_) {
          if (slot.dc && slot.trampoline) {
            slot.dc->UnregisterObserver();
          }
          slot.trampoline.reset();
          // Close before drop so the SCTP teardown is graceful; the
          // libwebrtc DCs would clean up on ref-drop anyway but a
          // Close() makes the close-reason show up cleanly in the
          // remote (portal) onclose callback.
          if (slot.dc) {
            slot.dc->Close();
          }
          slot.dc = nullptr;
        }
      },
      this));

  // Clear bound observers under obs_lock_ — defence-in-depth so any
  // late callback that races Shutdown (shouldn't happen post-
  // Unregister, but the libwebrtc signaling thread can have queued
  // tasks in flight) sees a clean slate.
  {
    base::AutoLock lock(obs_lock_);
    for (auto& slot : bound_observers_) {
      slot = nullptr;
    }
  }
}

void CbDataChannelHost::OnChannelStateChanged_Signaling(CbDcLabel label) {
  DCHECK(signaling_task_runner_->RunsTasksInCurrentSequence());

  webrtc::DataChannelInterface::DataState state;
  {
    base::AutoLock lock(slots_lock_);
    auto& slot = slots_[static_cast<size_t>(label)];
    if (!slot.dc) {
      return;  // Raced with Shutdown; nothing to report.
    }
    state = slot.dc->state();
    if (state == webrtc::DataChannelInterface::DataState::kOpen) {
      slot.ever_opened = true;
    }
  }

  if (host_observer_) {
    host_observer_->OnChannelStateChanged(label, state);
  }

  // Latch all-open + notify exactly once.
  if (state == webrtc::DataChannelInterface::DataState::kOpen &&
      !all_open_latched_) {
    bool all = true;
    {
      base::AutoLock lock(slots_lock_);
      for (const auto& slot : slots_) {
        if (!slot.ever_opened) {
          all = false;
          break;
        }
      }
    }
    if (all) {
      all_open_latched_ = true;
      if (host_observer_) {
        host_observer_->OnAllChannelsOpen();
      }
    }
  }
}

void CbDataChannelHost::OnChannelMessage_Signaling(
    CbDcLabel /*label*/,
    const webrtc::DataBuffer& /*buffer*/) {
  // R1: nothing host-side to do for messages — the trampoline
  // already forwarded to the consumer. This method exists for
  // future host-side metrics taps (per-label inbound byte counters);
  // currently a no-op.
  // TODO(M3-R5-host-inbound-metrics): if M6 R1's CbStatsRelay
  // wants symmetric outbound/inbound counters, hook them here.
}

void CbDataChannelHost::OnChannelBufferedAmountChange_Signaling(
    CbDcLabel /*label*/,
    uint64_t /*sent_data_size*/) {
  // R1: no host-side action. M5 R3's backpressure gate will read
  // BufferedAmount() directly when it lands; that doesn't need a
  // host-side reaction here.
  // TODO(M3-R5-backpressure-callback): see header.
}

}  // namespace cloud_browser::signaling

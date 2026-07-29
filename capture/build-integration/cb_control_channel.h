// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbControlChannel — the guest's ask-a-human channel.
//
// Everything else the guest sends is telemetry or pixels; this is the one
// path where the browser has to STOP and wait for a person. A page calls
// confirm(), or requests the clipboard, or clicks <input type=file>, and
// chromium hands us a callback it will not proceed without. Without a
// channel to the viewer we have to answer on the user's behalf — which is
// exactly why confirm() currently dead-ends every page that gates on it.
//
// Bound to CbDcLabel::kControl (see capture/signaling/cb_dc_host.h).
//
// ─── Wire protocol (v1) ────────────────────────────────────────────────
//
// Envelopes match the house shape used by the input/cursor/clipboard
// channels — {v, type, t, seq, data} — so the portal's existing decode
// helpers apply unchanged. Three types:
//
//   guest -> portal, needs an answer:
//     {"v":1,"type":"ui_request","t":<epoch_ms>,"seq":<n>,
//      "data":{"id":"<uuid>","kind":"js_dialog","deadline_ms":60000, ...}}
//
//   portal -> guest, answering one:
//     {"v":1,"type":"ui_response","t":<epoch_ms>,"seq":<n>,
//      "data":{"id":"<uuid>", ...kind-specific fields...}}
//
//   guest -> portal, fire-and-forget (no answer expected):
//     {"v":1,"type":"ui_event","t":<epoch_ms>,"seq":<n>,
//      "data":{"kind":"fullscreen_changed", ...}}
//
// `id` correlates request to response. A response whose id is unknown
// (late, duplicated, or invented) is dropped with a log — never trusted,
// since the portal is a remote peer and this channel gates real decisions
// like "proceed past a certificate error".
//
// ─── THE INVARIANT THAT MATTERS ────────────────────────────────────────
//
// Every request MUST resolve exactly once, and MUST NOT depend on the
// portal to do so.
//
// content::JavaScriptDialogManager::RunJavaScriptDialog hands us a
// DialogClosedCallback and BLOCKS THE PAGE'S JS THREAD until it runs. If a
// dropped portal, a closed channel, or a user who wandered off could leave
// that callback unrun, the page wedges forever and the tab is dead. So:
//
//   * If the channel is not open when the request is minted, the default
//     is applied SYNCHRONOUSLY and nothing is sent. This is deliberately
//     the pre-existing behaviour (chromium's no-delegate path
//     auto-dismisses), so an unattended agent-driven session behaves
//     exactly as it does today.
//   * Every in-flight request carries a base::OneShotTimer. On expiry the
//     default is applied and the entry is erased.
//   * CancelAllPending() resolves every outstanding request with its
//     default. Call it on channel close, on WebContents destruction, and
//     on session teardown.
//
// The callback is therefore owned by exactly one of: the response path,
// the timeout path, or the cancel path — whichever reaches it first. All
// three erase the entry under the same UI-sequence guarantee, so there is
// no double-run window.
//
// ─── Threading ─────────────────────────────────────────────────────────
//
// libwebrtc delivers OnMessage on the signaling thread; every consumer
// (dialog manager, permission manager, download delegate) lives on the UI
// thread. Same shape as CbInputDispatch: decode + validate on the
// signaling thread, then PostTask the resolved response to the UI runner.
// All pending_ mutation happens on the UI sequence.

#ifndef CAPTURE_BUILD_INTEGRATION_CB_CONTROL_CHANNEL_H_
#define CAPTURE_BUILD_INTEGRATION_CB_CONTROL_CHANNEL_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "api/data_channel_interface.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "capture/signaling/cb_dc_host.h"

namespace cloud_browser {

// Reply payload handed back to whoever minted the request. Empty dict on
// the default/timeout path — callers must treat "no fields" as "the user
// did not answer" and fall back to their own safe default, not to a
// permissive one.
using CbControlResponseCallback =
    base::OnceCallback<void(base::Value::Dict response)>;

class CbControlChannel : public webrtc::DataChannelObserver {
 public:
  // |dc_host| must outlive this object. |ui_task_runner| is the
  // BrowserThread::UI runner (injectable for tests).
  CbControlChannel(signaling::CbDataChannelHost* dc_host,
                   scoped_refptr<base::SequencedTaskRunner> ui_task_runner);
  ~CbControlChannel() override;

  CbControlChannel(const CbControlChannel&) = delete;
  CbControlChannel& operator=(const CbControlChannel&) = delete;

  // Send a request and wait (asynchronously) for the human's answer.
  //
  // |kind| is the request family ("js_dialog", "permission",
  // "file_chooser", "cert_error"). |payload| is merged into the request's
  // `data` object. |deadline| bounds the wait; on expiry |callback| runs
  // with an empty dict.
  //
  // |callback| ALWAYS runs exactly once, on the UI sequence — including
  // when the channel is closed (it runs before this returns, synchronously)
  // or the request times out. Callers may rely on that; the dialog manager
  // does, because chromium will not let it not.
  //
  // Must be called on the UI sequence.
  void SendRequest(const std::string& kind,
                   base::Value::Dict payload,
                   base::TimeDelta deadline,
                   CbControlResponseCallback callback);

  // Fire-and-forget notice. No id, no reply, no bookkeeping. Silently
  // dropped when the channel is closed — by definition nothing depends on
  // it arriving.
  void SendEvent(const std::string& kind, base::Value::Dict payload);

  // Resolve every in-flight request with its default (an empty dict).
  // Idempotent. Call on channel close, WebContents destruction, and
  // teardown. Safe to call with nothing pending.
  void CancelAllPending();

  // True iff the underlying DC is open. Callers that can cheaply pick a
  // better default when there is no human reachable may consult this
  // first; SendRequest handles the closed case correctly regardless.
  bool IsChannelOpen() const;

  // webrtc::DataChannelObserver:
  void OnMessage(const webrtc::DataBuffer& buffer) override;
  void OnStateChange() override;
  void OnBufferedAmountChange(uint64_t sent_data_size) override;
  bool IsOkToCallOnTheNetworkThread() override;

  // Test seam: feed a raw inbound frame as if it arrived on the wire.
  void OnMessageForTesting(const std::string& json);

  // Number of requests awaiting an answer. Tests assert this reaches 0 on
  // every terminal path — a leak here is a wedged page in production.
  size_t PendingCountForTesting() const;

 private:
  struct PendingRequest {
    CbControlResponseCallback callback;
    std::unique_ptr<base::OneShotTimer> deadline_timer;
    std::string kind;  // for logging only
  };

  // Runs on the UI sequence. Erases the entry and invokes its callback.
  // No-op when |id| is unknown, which is what makes the response /
  // timeout / cancel race benign: the first one through wins, the losers
  // find nothing.
  void ResolvePending(const std::string& id, base::Value::Dict response);

  // Signaling-thread half of OnMessage: parse, then hop.
  void HandleInboundJson(const std::string& raw);

  const raw_ptr<signaling::CbDataChannelHost> dc_host_;
  const scoped_refptr<base::SequencedTaskRunner> ui_task_runner_;

  // UI-sequence only.
  std::map<std::string, PendingRequest> pending_;
  uint64_t next_seq_ = 0;
  uint64_t next_request_id_ = 0;

  SEQUENCE_CHECKER(ui_sequence_checker_);

  base::WeakPtrFactory<CbControlChannel> weak_factory_{this};
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CB_CONTROL_CHANNEL_H_

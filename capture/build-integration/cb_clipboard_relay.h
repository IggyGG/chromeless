// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbClipboardRelay — the guest half of the "clipboard" DataChannel.
//
// WHAT THIS REPLACED, AND WHY
//
// The first version of this file relayed frames byte-for-byte to a
// WebSocket "clipboard bridge" sidecar (capture/clipboard-bridge/, Go) that
// owned the OS-clipboard write and the in-page copy probe. The WebSocket
// halves were landed as DRAFTS — `PostOnIoSequence` logged "Pretend-send"
// and dropped the frame, `EnsureConnected()` was empty — and the sidecar
// itself was never composed into the standalone stack. So from 2026-05 to
// 2026-09 the client put a valid `clipboard_offer` on the wire on every
// paste and the guest discarded it. tests/interactive/run.py carried that as
// an explicit known-gap check ("client->guest clipboard is INERT
// guest-side"). Copy never worked either: nothing on the guest produced a
// cloud->client envelope.
//
// The bridge was a Phase-1 shape for a streamer PAGE that no longer exists
// (deleted in M7). The relay now lives in the browser process, which has
// direct access to the one thing the bridge existed to reach: ui::Clipboard.
// No sidecar, no sockets, no second copy of the envelope parser.
//
// PASTE (client -> cloud)
//
//   1. OnMessage on the libwebrtc signaling thread: parse the v1 envelope
//      (ReadDict, strict RFC), enforce direction/source/version and the 1 MiB
//      cap, then hop to the UI thread with the text.
//   2. Write the text to the guest's clipboard (ui::ScopedClipboardWriter,
//      kCopyPaste). The guest's Chromium is an ozone/X11 process under Xvfb,
//      so this is a real clipboard the renderer can read back.
//   3. Synthesise Ctrl+V against the active WebContents, exactly as
//      CbInputDispatchClipboard synthesises Ctrl+C for copy: a
//      kRawKeyDown + kKeyUp pair with the Ctrl modifier on the per-event
//      modifiers only. The page's own `keydown`/`paste` handlers run, so a
//      page that intercepts paste (editors do) behaves as it does for a
//      physical keypress — which `WebContents::Paste()` would bypass.
//
//   The two halves are separate on purpose: writing the clipboard without
//   pasting is what a "copy to remote clipboard" affordance would do, and
//   the last-written text is what echo suppression (below) compares against.
//
// COPY (cloud -> client)
//
//   The page's copy lands in the OS clipboard via content's ClipboardHostImpl
//   -> ui::ScopedClipboardWriter, whose destructor notifies
//   ui::ClipboardMonitor. This relay is a ui::ClipboardObserver, so it learns
//   of every write — including ones NO user asked for: a page calling
//   `navigator.clipboard.writeText()` from a timer, an extension, a
//   focus-stealing script. Forwarding every change would hand a hostile page
//   the viewer's clipboard. So:
//
//   * A change is forwarded only inside a short WINDOW armed by an explicit
//     user gesture: the client's `clipboard_copy_request` envelope (its copy
//     gesture on the input channel), or a Ctrl/Cmd+C keydown that reached the
//     guest via the keyboard dispatcher. The window is kArmWindow long.
//   * A change whose text equals what WE last wrote for a paste is an echo of
//     our own write and is dropped (the spec's "receiver does not echo").
//   * Text is read asynchronously (ui::Clipboard::ReadText takes a callback),
//     capped at 1 MiB, and sent as a v1 cloud->client envelope on the
//     clipboard DC via CbDataChannelHost::SendAsync.
//
//   This is the spec's "no silent polling" rule, enforced at the only place
//   that can enforce it. It also means a copy the page performs on its own
//   (a "Copy link" button that calls writeText from a click handler) IS
//   forwarded if the click came through the input channel within the window
//   — the click is the gesture.
//
// THREADING
//
//   * OnMessage: libwebrtc signaling thread -> parse -> PostTask(UI).
//   * ui::Clipboard is UI-thread-only (ClipboardOzone DCHECKs the thread);
//     every clipboard call here runs on |ui_task_runner_|.
//   * ClipboardObserver notifications arrive on the UI thread.
//   * Outbound send goes through dc_host_->SendAsync, which hops to the
//     signaling thread itself and replies on |ui_task_runner_|.
//
// LIFETIME
//
//   Session-scoped, like CbControlChannel: constructed in
//   RebuildNativeDataChannels, bound with BindObserver(kClipboard), destroyed
//   on re-arm and in PostMainMessageLoopRun. The destructor unregisters from
//   ClipboardMonitor; |dc_host_| and |resolver| must outlive this object.

#ifndef CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_CLIPBOARD_RELAY_H_
#define CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_CLIPBOARD_RELAY_H_

#include <cstdint>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "cloud-browser/capture/signaling/cb_dc_host.h"
#include "third_party/webrtc/api/data_channel_interface.h"
#include "ui/base/clipboard/clipboard_observer.h"

namespace content {
class WebContents;
}  // namespace content

namespace cloud_browser {

class WebContentsResolver;

// Hard 1 MiB cap from docs/protocols/clipboard-channel.md, enforced on both
// directions: an oversize inbound frame is dropped before it is parsed, an
// oversize outbound read is dropped before it is sent.
inline constexpr size_t kMaxClipboardTextBytes = 1 << 20;

// How long after a copy gesture a clipboard change is treated as the result
// of that gesture. Long enough for the renderer round trip (the copy is
// dispatched as a key event, handled in the renderer, written back via
// ClipboardHostImpl); short enough that a page cannot ride the window for
// long. Measured renderer copy latency on the standalone stack is tens of
// milliseconds.
inline constexpr base::TimeDelta kClipboardArmWindow = base::Seconds(2);

class CbClipboardRelay : public webrtc::DataChannelObserver,
                         public ui::ClipboardObserver {
 public:
  // |dc_host| sends outbound envelopes; |resolver| supplies the active
  // WebContents for the synthesised paste; |ui_task_runner| is the
  // BrowserThread::UI runner (injectable for tests). All three are
  // caller-owned and must outlive this object.
  CbClipboardRelay(signaling::CbDataChannelHost* dc_host,
                   WebContentsResolver* resolver,
                   scoped_refptr<base::SequencedTaskRunner> ui_task_runner);
  CbClipboardRelay(const CbClipboardRelay&) = delete;
  CbClipboardRelay& operator=(const CbClipboardRelay&) = delete;
  ~CbClipboardRelay() override;

  // Arm the copy window: the viewer performed a copy gesture and the next
  // clipboard change within kClipboardArmWindow is theirs to receive. Called
  // by CbInputDispatchClipboard on `clipboard_copy_request` and by the
  // keyboard dispatcher on a Ctrl/Cmd+C keydown. UI thread.
  void ArmCopyWindow();

  // webrtc::DataChannelObserver (signaling thread):
  void OnMessage(const webrtc::DataBuffer& buffer) override;
  void OnStateChange() override;
  void OnBufferedAmountChange(uint64_t sent_data_size) override;
  bool IsOkToCallOnTheNetworkThread() override;

  // ui::ClipboardObserver (UI thread):
  void OnClipboardDataChanged() override;

  // For callbacks that may outlive this session-scoped object (the input
  // delegate's copy-gesture hook): a WeakPtr receiver makes them no-ops
  // after destruction instead of use-after-frees.
  base::WeakPtr<CbClipboardRelay> AsWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // Test seams.
  void OnMessageForTesting(const std::string& json);
  int64_t pastes_applied_for_testing() const { return pastes_applied_; }
  int64_t copies_sent_for_testing() const { return copies_sent_; }
  int64_t changes_ignored_for_testing() const { return changes_ignored_; }

 private:
  // Signaling-thread half of OnMessage: validate, then hop with the text.
  void HandleInboundJson(const std::string& raw);

  // UI thread: write |text| to the guest clipboard and synthesise Ctrl+V.
  void ApplyPaste(std::string text);
  void SynthesizePaste(content::WebContents* wc);

  // UI thread: ReadText completion for an armed clipboard change.
  void OnClipboardTextRead(std::u16string text);

  // UI thread: build and send a cloud->client envelope.
  void SendCopy(const std::string& text);

  const raw_ptr<signaling::CbDataChannelHost> dc_host_;
  const raw_ptr<WebContentsResolver> resolver_;
  const scoped_refptr<base::SequencedTaskRunner> ui_task_runner_;

  // Echo suppression: the UTF-8 text of our most recent paste write. A
  // clipboard change carrying exactly this text is our own write coming
  // back through the monitor, not a user copy.
  std::string last_written_text_;

  // Copy window. Zero when unarmed.
  base::TimeTicks copy_armed_until_;

  int64_t next_seq_ = 0;
  int64_t pastes_applied_ = 0;
  int64_t copies_sent_ = 0;
  int64_t changes_ignored_ = 0;

  SEQUENCE_CHECKER(ui_sequence_checker_);
  base::WeakPtrFactory<CbClipboardRelay> weak_factory_{this};
};

}  // namespace cloud_browser

#endif  // CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_CLIPBOARD_RELAY_H_

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbInputDispatch — webrtc::DataChannelObserver consumer of the v1
// input wire protocol described in docs/protocols/input-channel.md.
// Subscribed to the "input" DataChannel created on the browser-process
// PeerConnection (M3 host registers us); decodes the v1 envelope on the
// libwebrtc signaling/network thread, validates v == 1, and hops to
// BrowserThread::UI before invoking any typed dispatch.
//
// R1 scope (CV2-41):
//   * OnMessage envelope decode + v == 1 validation
//   * unknown-type events: skip + log + metric, no crash
//   * malformed JSON: log + metric, non-fatal
//   * signaling-thread to UI PostTask (mandatory — RWH / IME / drag /
//     touch dispatch APIs all require BrowserThread::UI)
//   * per-type handler dispatch as virtual on a Delegate interface;
//     R3 / R4 / R5 / R6 / R7 / R8 implement the actual injection
//
// Non-goals for R1:
//   * any per-type chromium injection (RWHV mouse/key/touch/wheel,
//     Input.imeSetComposition equivalents, drag-and-drop adapters)
//   * coordinate / DIP mapping (R3+)
//   * cross-envelope modifier state machines (input-bridge/main.go's
//     heldMods logic) — that lives on the UI side once we have an
//     event sink, not on the signaling-thread observer
//   * registering ourselves with the M3 DataChannel — the wiring lives
//     in cloud_browser_browser_main_parts.cc once M3.R hands us the
//     DataChannelInterface to attach to. See TODO(M4-R1-wire-m3).

#ifndef CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_H_
#define CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_H_

#include <cstdint>
#include <memory>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
#include "third_party/webrtc/api/data_channel_interface.h"
#include "third_party/webrtc/api/scoped_refptr.h"

namespace cloud_browser {

// Decoded v1 envelope. Mirrors the Go inputEnvelope in capture/input-
// bridge/main.go so anything that round-trips through the existing
// input-channel.md spec keeps the same field names + types. The
// payload stays as base::Value::Dict so the typed handlers can pull
// out their per-type fields with the chromium-standard accessor API
// instead of re-parsing.
struct InputEnvelope {
  // Protocol version. R1 rejects anything other than 1.
  int version = 0;
  // Event kind — "mouse_move", "mouse_button", "mouse_wheel", "key",
  // "composition_*", "clipboard_paste", "drag_*", "touch_*", etc. See
  // docs/protocols/input-channel.md for the authoritative list.
  std::string type;
  // Client-side originating timestamp (ms since epoch). Carried
  // through for one-way latency tracking; R1 doesn't act on it.
  int64_t t = 0;
  // Monotonic per-channel sequence. Carried through for gap / replay
  // detection; R1 doesn't act on it.
  int64_t seq = 0;
  // Per-type payload. Always a dict for the v1 types we know about;
  // R1 doesn't reach into it.
  base::Value::Dict data;
};

// Delegate that receives the typed, already-on-UI-thread dispatch
// from CbInputDispatch. R3 / R4 / R5 / R6 / R7 / R8 of M4 will swap
// in real implementations against RWHV / Input.* CDP analogues /
// touch dispatch / drag adapters. R1 ships with a LoggingDelegate
// that emits a single INFO line per dispatched event so the
// end-to-end test (and the operator) can confirm the data channel
// is plumbed through.
//
// All methods are invoked on BrowserThread::UI. Delegates must NOT
// re-post to other threads from within the handler — the per-type
// injection APIs (RWHV / IME / drag) are themselves UI-thread-only.
class CbInputDispatchDelegate {
 public:
  virtual ~CbInputDispatchDelegate() = default;

  // Called once per envelope whose `type` field matches a known v1
  // event. Default implementation is a logger; R3+ overrides per
  // type. The envelope is moved in so the delegate can hang onto
  // the Dict payload without copying.
  //
  // TODO(M4-R1-typed-handlers): split this into per-type virtuals
  // (OnMouseMove, OnMouseButton, OnMouseWheel, OnKey, OnComposition*,
  // OnClipboardPaste, OnDragStart/Over/Drop/End, OnTouchStart/Move/
  // End/Cancel) once R3 starts wiring real injection. Keeping a
  // single dispatch point for R1 minimises the surface that the M3
  // wiring needs to touch.
  virtual void OnInputEvent(InputEnvelope envelope) = 0;

  // Called when an envelope fails decode: invalid JSON, missing
  // required field, or v != 1. R1 logs + metrics; the dispatcher
  // does not call OnInputEvent for these. Delegates use this to
  // surface a metric / page a developer.
  virtual void OnInputEventDecodeError(
      const std::string& reason,
      const std::string& raw_payload_preview) {}

  // Called when an envelope decodes cleanly but its `type` field is
  // not on the v1 known-types list. R1 logs + metrics; delegates can
  // wire a counter or rely on the default LOG(WARNING).
  virtual void OnInputEventUnknownType(
      const std::string& type,
      int64_t seq) {}
};

// CbInputDispatch is the DataChannelObserver attached by M3 to the
// "input" DataChannel. It owns no chromium UI state of its own —
// everything UI-side is invoked through the delegate.
//
// Ownership: held by whoever creates it (typically
// CloudBrowserBrowserMainParts in M3 R.). The DataChannel keeps a
// raw pointer back to its observer via RegisterObserver/Unregister
// Observer; callers MUST UnregisterObserver before this object is
// destroyed.
class CbInputDispatch : public webrtc::DataChannelObserver {
 public:
  // |ui_task_runner| is the runner for BrowserThread::UI. Injectable
  // so unit tests can pass a TestSimpleTaskRunner without spinning
  // up a real chromium browser process. Production callers pass
  // content::GetUIThreadTaskRunner({}).
  //
  // |delegate| receives the typed dispatch on the UI thread. R1
  // production wiring passes a LoggingDelegate; R3+ supplies a real
  // injector. Lifetime: caller-owned, must outlive this object.
  CbInputDispatch(
      scoped_refptr<base::SequencedTaskRunner> ui_task_runner,
      CbInputDispatchDelegate* delegate);

  CbInputDispatch(const CbInputDispatch&) = delete;
  CbInputDispatch& operator=(const CbInputDispatch&) = delete;

  ~CbInputDispatch() override;

  // webrtc::DataChannelObserver — invoked on libwebrtc signaling /
  // network thread. R1 only implements OnMessage; OnStateChange is a
  // no-op stub here (we don't gate dispatch on channel state at this
  // layer; the channel either delivers a message or it doesn't).
  void OnMessage(const webrtc::DataBuffer& buffer) override;
  void OnStateChange() override;
  void OnBufferedAmountChange(uint64_t sent_data_size) override;
  bool IsOkToCallOnTheNetworkThread() override;

  // Test seam. ParseEnvelope is package-internal but exposed for the
  // R1 unit test (capture/build-integration/cb_input_dispatch_test.
  // cc, drafted alongside R1) so the malformed-JSON / v != 1 /
  // missing-type acceptance criteria can be asserted without
  // standing up a DataChannelObserver harness.
  //
  // Returns true on success and populates |out|. Returns false with
  // |out_reason| set on any decode failure.
  static bool ParseEnvelopeForTesting(
      const std::string& json,
      InputEnvelope* out,
      std::string* out_reason);

 private:
  // Protocol constant — bumped only when the wire envelope shape
  // (not its data payload) changes incompatibly. v1.1 additions
  // (composition selection/rect/candidates, drag, touch) stay at
  // v=1 because they extend `data` without changing the envelope.
  static constexpr int kProtocolVersion = 1;

  // PostTask helper. Always called from the signaling/network
  // thread; never reentrant from the UI thread.
  void HopToUiAndDispatch(InputEnvelope envelope);
  void HopToUiAndReportDecodeError(std::string reason,
                                   std::string preview);
  void HopToUiAndReportUnknownType(std::string type, int64_t seq);

  // Both members are immutable after construction; safe to touch
  // from the signaling thread without a lock.
  const scoped_refptr<base::SequencedTaskRunner> ui_task_runner_;
  CbInputDispatchDelegate* const delegate_;
};

// LoggingDelegate is the R1 production stand-in. Emits an INFO log
// per dispatched event so the operator can confirm the M3 → M4
// plumbing is alive end-to-end, and a WARNING per decode error /
// unknown type so any client-side regressions are observable in the
// chromium pod log without metric tooling.
//
// R3+ replaces this with a real injector that drives RWHV /
// Input.imeSetComposition / touch dispatch / drag adapters.
class CbInputLoggingDelegate : public CbInputDispatchDelegate {
 public:
  CbInputLoggingDelegate();
  ~CbInputLoggingDelegate() override;

  void OnInputEvent(InputEnvelope envelope) override;
  void OnInputEventDecodeError(
      const std::string& reason,
      const std::string& raw_payload_preview) override;
  void OnInputEventUnknownType(const std::string& type,
                               int64_t seq) override;
};

}  // namespace cloud_browser

#endif  // CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_INPUT_DISPATCH_H_

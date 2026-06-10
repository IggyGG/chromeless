// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cloud-browser/capture/build-integration/cb_input_dispatch.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "third_party/webrtc/api/data_channel_interface.h"

namespace cloud_browser {

namespace {

// Mirror of CbInputDispatch::kProtocolVersion. Kept here so the
// anonymous-namespace DecodeEnvelope can reach it without exposing
// the class constant publicly or declaring DecodeEnvelope a friend.
// Both must stay in sync; if you bump the wire-protocol version,
// update CbInputDispatch::kProtocolVersion in the header AND this
// constant.
constexpr int kProtocolVersion = 1;
static_assert(kProtocolVersion == 1,
              "v1 is the only wire envelope shape currently spec'd; "
              "bumping requires a coordinated client + server change");

// Set of v1 envelope `type` strings the dispatcher recognises. Must
// match docs/protocols/input-channel.md and the case dispatch in
// capture/input-bridge/main.go's Dispatch(). When adding a new type
// to the spec, add it here as well or it will be reported through
// OnInputEventUnknownType.
//
// Sorted alphabetically; binary-searchable. Keeping it as a const
// array (vs. a std::set) avoids static-init order issues and keeps
// the dispatcher allocation-free on the hot path.
constexpr const char* kKnownInputTypes[] = {
    "clipboard_copy_request",
    "clipboard_paste",
    "composition_cancel",
    "composition_end",
    "composition_start",
    "composition_update",
    "drag_end",
    "drag_over",
    "drag_start",
    "drop",
    "key_down",
    "key_up",
    "mouse_button",
    "mouse_enter",
    "mouse_leave",
    "mouse_move",
    "mouse_wheel",
    "touch_cancel",
    "touch_end",
    "touch_move",
    "touch_start",
};

constexpr double kMaxSafeJsonInteger = 9007199254740991.0;  // 2^53 - 1.

bool IsKnownInputType(const std::string& type) {
  return std::binary_search(std::begin(kKnownInputTypes),
                            std::end(kKnownInputTypes),
                            type);
}

// First N bytes of a payload, with non-printables stripped, for
// safe inclusion in error logs. Intentionally avoids dumping the
// full envelope — input payloads can carry pasted user text or
// composition strings that we don't want to spray into INFO logs.
std::string TruncatedPreview(const std::string& raw) {
  constexpr size_t kPreviewMax = 96;
  std::string out;
  out.reserve(std::min(raw.size(), kPreviewMax));
  for (size_t i = 0; i < raw.size() && out.size() < kPreviewMax; ++i) {
    char c = raw[i];
    if (c >= 0x20 && c < 0x7f) {
      out.push_back(c);
    } else {
      out.push_back('.');
    }
  }
  if (raw.size() > kPreviewMax) {
    out.append("...");
  }
  return out;
}

// JSONReader stores numbers that fit Chromium's int slot as int and
// wider JSON numbers as double. v1 specifies `t` as Date.now() epoch
// milliseconds, which is wider than int32 but still exactly representable
// as a JavaScript-safe integer. Accept integral numbers up to that bound
// so the native decoder matches the public input-channel spec.
std::optional<int64_t> FindSafeJsonInteger(const base::DictValue& dict,
                                           const char* key) {
  if (std::optional<int> int_value = dict.FindInt(key)) {
    return static_cast<int64_t>(*int_value);
  }

  std::optional<double> double_value = dict.FindDouble(key);
  if (!double_value.has_value()) {
    return std::nullopt;
  }
  if (!std::isfinite(*double_value) ||
      *double_value < -kMaxSafeJsonInteger ||
      *double_value > kMaxSafeJsonInteger ||
      std::trunc(*double_value) != *double_value) {
    return std::nullopt;
  }

  return static_cast<int64_t>(*double_value);
}

// Reads the v1 envelope from `raw`. On success populates `out` and
// returns true; on failure populates `out_reason` and returns false.
//
// Decode contract:
//   * malformed JSON → false ("invalid JSON: <parser error>")
//   * top level not a JSON object → false ("not an object")
//   * missing or non-int `v` → false ("missing/invalid 'v'")
//   * v != 1 → false ("unsupported protocol version v=<n>")
//   * missing or empty `type` → false ("missing/invalid 'type'")
//   * `t` / `seq` missing or non-int → false (we treat these as
//     mandatory; clients always emit them per spec)
//   * `data` missing or not an object → false; the per-type R3+
//     handlers assume a Dict and we'd rather reject at the seam
//
// Unknown `type` is NOT a decode error — it's a known-good envelope
// referring to a v1 event the dispatcher doesn't (yet) handle.
// That's reported through OnInputEventUnknownType, not here.
bool DecodeEnvelope(const std::string& raw,
                    InputEnvelope* out,
                    std::string* out_reason) {
  DCHECK(out);
  DCHECK(out_reason);

  // CV2-75 fix-forward: chromium 7727's ReadAndReturnValueWithError
  // now requires an explicit JSONParserOptions arg. base::JSON_PARSE_RFC
  // is the strict-RFC default matching the M4 R1 v=1 envelope spec (no
  // comments, no trailing commas) and what the functional-test harness
  // pre-emits.
  auto parsed = base::JSONReader::ReadAndReturnValueWithError(
      raw, base::JSON_PARSE_RFC);
  if (!parsed.has_value()) {
    *out_reason = "invalid JSON: " + parsed.error().message;
    return false;
  }
  if (!parsed->is_dict()) {
    *out_reason = "envelope not a JSON object";
    return false;
  }

  base::DictValue envelope = std::move(*parsed).TakeDict();

  std::optional<int> v = envelope.FindInt("v");
  if (!v.has_value()) {
    *out_reason = "missing or non-integer 'v'";
    return false;
  }
  if (*v != kProtocolVersion) {
    *out_reason = "unsupported protocol version v=" + base::NumberToString(*v);
    return false;
  }
  out->version = *v;

  const std::string* type_str = envelope.FindString("type");
  if (!type_str || type_str->empty()) {
    *out_reason = "missing or empty 'type'";
    return false;
  }
  out->type = *type_str;

  // `t` / `seq` are JSON numbers. The public v1 spec defines `t` as
  // Date.now() epoch-ms, which exceeds Chromium's 32-bit FindInt slot
  // today, so read it through the safe-integer helper above.
  std::optional<int64_t> t = FindSafeJsonInteger(envelope, "t");
  if (!t.has_value()) {
    *out_reason = "missing or non-integer 't'";
    return false;
  }
  out->t = *t;

  std::optional<int64_t> seq = FindSafeJsonInteger(envelope, "seq");
  if (!seq.has_value()) {
    *out_reason = "missing or non-integer 'seq'";
    return false;
  }
  out->seq = *seq;

  base::DictValue* data = envelope.FindDict("data");
  if (!data) {
    *out_reason = "missing or non-object 'data'";
    return false;
  }
  out->data = std::move(*data);

  return true;
}

}  // namespace

// ---------------------------------------------------------------------
// CbInputDispatch
// ---------------------------------------------------------------------

CbInputDispatch::CbInputDispatch(
    scoped_refptr<base::SequencedTaskRunner> ui_task_runner,
    CbInputDispatchDelegate* delegate)
    : ui_task_runner_(std::move(ui_task_runner)),
      delegate_(delegate) {
  DCHECK(ui_task_runner_);
  DCHECK(delegate_);
}

CbInputDispatch::~CbInputDispatch() = default;

void CbInputDispatch::OnMessage(const webrtc::DataBuffer& buffer) {
  // libwebrtc delivers binary OR text frames here. The v1 protocol
  // ships JSON text, so a binary frame is treated as malformed
  // input. R1 doesn't try to handle binary; clients that need to
  // send opaque blobs would bump to v=2 with a base64 / CBOR
  // payload.
  if (buffer.binary) {
    // Capacity-bounded preview is enough — the binary frame might be
    // huge and we don't want to copy it. The reason string also
    // routes through the metric path so the operator can see what
    // ratio of frames are binary vs text.
    HopToUiAndReportDecodeError(
        "binary frame on input channel (v1 expects JSON text)",
        "");
    return;
  }

  // DataBuffer carries a rtc::CopyOnWriteBuffer of bytes; reinterpret
  // as char so std::string can copy.
  const char* data = reinterpret_cast<const char*>(buffer.data.data());
  std::string raw(data, buffer.data.size());

  InputEnvelope envelope;
  std::string reason;
  if (!DecodeEnvelope(raw, &envelope, &reason)) {
    HopToUiAndReportDecodeError(std::move(reason), TruncatedPreview(raw));
    return;
  }

  if (!IsKnownInputType(envelope.type)) {
    HopToUiAndReportUnknownType(envelope.type, envelope.seq);
    return;
  }

  HopToUiAndDispatch(std::move(envelope));
}

void CbInputDispatch::OnStateChange() {
  // R1 no-op. Per spec we don't gate dispatch on channel state at
  // this layer; libwebrtc only invokes OnMessage on `open` channels.
  //
  // TODO(M4-R1-state-metric): wire a state-transition counter once
  // M3 R is done — the operator wants to see how often the input
  // channel reconnects.
}

void CbInputDispatch::OnBufferedAmountChange(uint64_t /*sent_data_size*/) {
  // R1 no-op. The input channel is consumer-side; we never send.
}

bool CbInputDispatch::IsOkToCallOnTheNetworkThread() {
  // Returning false keeps OnMessage on the signaling thread, which
  // matches the assumption the rest of this file makes. The UI hop
  // happens explicitly inside OnMessage; we don't want libwebrtc to
  // re-route us onto the network thread because that creates a
  // second source of concurrent OnMessage calls.
  return false;
}

// static
bool CbInputDispatch::ParseEnvelopeForTesting(
    const std::string& json,
    InputEnvelope* out,
    std::string* out_reason) {
  return DecodeEnvelope(json, out, out_reason);
}

void CbInputDispatch::HopToUiAndDispatch(InputEnvelope envelope) {
  ui_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&CbInputDispatchDelegate::OnInputEvent,
                     base::Unretained(delegate_),
                     std::move(envelope)));
}

void CbInputDispatch::HopToUiAndReportDecodeError(std::string reason,
                                                  std::string preview) {
  ui_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&CbInputDispatchDelegate::OnInputEventDecodeError,
                     base::Unretained(delegate_),
                     std::move(reason),
                     std::move(preview)));
}

void CbInputDispatch::HopToUiAndReportUnknownType(std::string type,
                                                  int64_t seq) {
  ui_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&CbInputDispatchDelegate::OnInputEventUnknownType,
                     base::Unretained(delegate_),
                     std::move(type),
                     seq));
}

// ---------------------------------------------------------------------
// CbInputLoggingDelegate
// ---------------------------------------------------------------------

CbInputLoggingDelegate::CbInputLoggingDelegate() = default;
CbInputLoggingDelegate::~CbInputLoggingDelegate() = default;

void CbInputLoggingDelegate::OnInputEvent(InputEnvelope envelope) {
  LOG(INFO) << "CbInputDispatch: dispatched type=" << envelope.type
            << " seq=" << envelope.seq
            << " t=" << envelope.t;
  // TODO(M4-R1-real-dispatch): R3+ replaces this with the RWHV /
  // Input.* injection per type. Until then the dispatched envelope
  // intentionally goes nowhere — the chromium pod log line is the
  // evidence that the M3 → M4 plumbing is alive.
}

void CbInputLoggingDelegate::OnInputEventDecodeError(
    const std::string& reason,
    const std::string& raw_payload_preview) {
  LOG(WARNING) << "CbInputDispatch: decode error: " << reason
               << " (preview=\"" << raw_payload_preview << "\")";
  // TODO(M4-R1-metric): wire a base::UmaHistogram counter so a
  // dashboard can surface decode-error rate without log scraping.
  // Deferred to M4 R8 (metrics consolidation).
}

void CbInputLoggingDelegate::OnInputEventUnknownType(
    const std::string& type,
    int64_t seq) {
  LOG(WARNING) << "CbInputDispatch: unknown v1 type=\"" << type
               << "\" seq=" << seq << " (skipped)";
  // TODO(M4-R1-metric): same as above. Unknown-type counter wants
  // a per-type breakdown (UmaHistogramSparse on a stable hash) so
  // we can spot spec drift between input-bridge and a future
  // client.
}

}  // namespace cloud_browser

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "capture/build-integration/cb_control_channel.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"

namespace cloud_browser {

namespace {

// Wire constants. Kept local so the header doesn't leak the vocabulary,
// matching how cb_input_dispatch.cc and cb_cursor_envelope.cc do it.
constexpr int kProtocolVersion = 1;
constexpr char kTypeRequest[] = "ui_request";
constexpr char kTypeResponse[] = "ui_response";
constexpr char kTypeEvent[] = "ui_event";

constexpr signaling::CbDcLabel kLabel = signaling::CbDcLabel::kControl;

// Bound on a single inbound frame. The control channel carries small JSON
// decisions; a multi-MB frame here is either a bug or an attempt to make
// us allocate. The input channel applies the same class of guard.
constexpr size_t kMaxInboundFrameBytes = 64 * 1024;

int64_t NowMs() {
  return base::Time::Now().InMillisecondsSinceUnixEpoch();
}

}  // namespace

CbControlChannel::CbControlChannel(
    signaling::CbDataChannelHost* dc_host,
    scoped_refptr<base::SequencedTaskRunner> ui_task_runner)
    : dc_host_(dc_host), ui_task_runner_(std::move(ui_task_runner)) {
  DETACH_FROM_SEQUENCE(ui_sequence_checker_);
}

CbControlChannel::~CbControlChannel() {
  // Do NOT rely on the dtor to resolve pending callbacks: by the time we
  // are destroyed the consumers that own those callbacks (the dialog
  // manager, the permission manager) may already be gone, and running a
  // callback into a freed object is worse than not running it. Teardown
  // must call CancelAllPending() explicitly while everything is still
  // alive. Loudly flag the leak if that ordering was not honoured.
  if (!pending_.empty()) {
    LOG(ERROR) << "CbControlChannel destroyed with " << pending_.size()
               << " request(s) still in flight — CancelAllPending() was not "
                  "called before teardown. Any page blocked on one of these "
                  "is now permanently wedged.";
  }
}

bool CbControlChannel::IsChannelOpen() const {
  return dc_host_ && dc_host_->IsOpen(kLabel);
}

void CbControlChannel::SendRequest(const std::string& kind,
                                   base::Value::Dict payload,
                                   base::TimeDelta deadline,
                                   CbControlResponseCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);

  // ── Closed-channel fast path ──
  //
  // Resolve synchronously with the default rather than minting an entry
  // nobody will ever answer. This is the branch that keeps unattended
  // agent-driven sessions behaving exactly as they did before this class
  // existed: no viewer attached => chromium's own default applies, just
  // routed through our caller instead of chromium's no-delegate path.
  if (!IsChannelOpen()) {
    VLOG(1) << "CbControlChannel: " << kind
            << " request while channel closed — applying default";
    std::move(callback).Run(base::Value::Dict());
    return;
  }

  const std::string id = base::NumberToString(++next_request_id_);

  base::Value::Dict data = std::move(payload);
  data.Set("id", id);
  data.Set("kind", kind);
  data.Set("deadline_ms", static_cast<int>(deadline.InMilliseconds()));

  base::Value::Dict envelope;
  envelope.Set("v", kProtocolVersion);
  envelope.Set("type", kTypeRequest);
  envelope.Set("t", static_cast<double>(NowMs()));
  envelope.Set("seq", static_cast<double>(++next_seq_));
  envelope.Set("data", std::move(data));

  std::string json;
  if (!base::JSONWriter::Write(base::Value(std::move(envelope)), &json)) {
    LOG(ERROR) << "CbControlChannel: failed to serialise " << kind
               << " request — applying default";
    std::move(callback).Run(base::Value::Dict());
    return;
  }

  // Arm the deadline BEFORE sending. If the send fails or the portal never
  // answers, this timer is the only thing that resolves the callback, and
  // arming it first means there is no window where the request exists
  // unguarded.
  PendingRequest entry;
  entry.callback = std::move(callback);
  entry.kind = kind;
  entry.deadline_timer = std::make_unique<base::OneShotTimer>();
  auto* timer = entry.deadline_timer.get();
  pending_.emplace(id, std::move(entry));

  timer->Start(FROM_HERE, deadline,
               base::BindOnce(
                   [](base::WeakPtr<CbControlChannel> self, std::string id) {
                     if (!self) {
                       return;
                     }
                     LOG(WARNING) << "CbControlChannel: request " << id
                                  << " timed out — applying default";
                     self->ResolvePending(id, base::Value::Dict());
                   },
                   weak_factory_.GetWeakPtr(), id));

  // SendAsync, not Send: this runs on the UI thread, often inside a posted
  // task scope that disallows blocking waits, and Send() BlockingCalls onto
  // the signaling thread. Same reasoning as the cursor emitter
  // (cb_cursor_dc_emitter.cc:357).
  dc_host_->SendAsync(
      kLabel, std::move(json), ui_task_runner_,
      base::BindOnce(
          [](base::WeakPtr<CbControlChannel> self, std::string id, bool ok,
             std::string message) {
            if (!self || ok) {
              return;
            }
            LOG(WARNING) << "CbControlChannel: send failed for request " << id
                         << " (" << message << ") — applying default";
            // Resolve now rather than making the page wait out the full
            // deadline for an answer that provably cannot arrive.
            self->ResolvePending(id, base::Value::Dict());
          },
          weak_factory_.GetWeakPtr(), id));
}

void CbControlChannel::SendEvent(const std::string& kind,
                                 base::Value::Dict payload) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);
  if (!IsChannelOpen()) {
    return;  // Nothing depends on a notice arriving.
  }

  base::Value::Dict data = std::move(payload);
  data.Set("kind", kind);

  base::Value::Dict envelope;
  envelope.Set("v", kProtocolVersion);
  envelope.Set("type", kTypeEvent);
  envelope.Set("t", static_cast<double>(NowMs()));
  envelope.Set("seq", static_cast<double>(++next_seq_));
  envelope.Set("data", std::move(data));

  std::string json;
  if (!base::JSONWriter::Write(base::Value(std::move(envelope)), &json)) {
    LOG(ERROR) << "CbControlChannel: failed to serialise " << kind << " event";
    return;
  }

  dc_host_->SendAsync(kLabel, std::move(json), ui_task_runner_,
                      base::BindOnce([](bool /*ok*/, std::string /*message*/) {
                        // Fire-and-forget: nothing depends on a notice
                        // arriving, so a failed send needs no recovery.
                      }));
}

void CbControlChannel::CancelAllPending() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);
  if (pending_.empty()) {
    return;
  }
  LOG(INFO) << "CbControlChannel: cancelling " << pending_.size()
            << " in-flight request(s) — applying defaults";

  // Move the map out first. Each callback may synthesise work that reaches
  // back into this object (a dialog callback can trigger a navigation,
  // which can open another dialog), and mutating pending_ while iterating
  // it would be undefined. Draining a local copy makes re-entry safe.
  auto drained = std::move(pending_);
  pending_.clear();
  for (auto& [id, entry] : drained) {
    if (entry.deadline_timer) {
      entry.deadline_timer->Stop();
    }
    if (entry.callback) {
      std::move(entry.callback).Run(base::Value::Dict());
    }
  }
}

void CbControlChannel::ResolvePending(const std::string& id,
                                      base::Value::Dict response) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);
  auto it = pending_.find(id);
  if (it == pending_.end()) {
    // Unknown id. Benign and expected: this is the losing side of the
    // response/timeout/cancel race, or a duplicated or fabricated response
    // from the peer. Either way there is nothing to resolve.
    VLOG(1) << "CbControlChannel: response for unknown request id " << id
            << " — dropped";
    return;
  }

  // Extract the entry BEFORE running the callback, for the same re-entry
  // reason as CancelAllPending: the callback may reach back in here.
  PendingRequest entry = std::move(it->second);
  pending_.erase(it);
  if (entry.deadline_timer) {
    entry.deadline_timer->Stop();
  }
  if (entry.callback) {
    std::move(entry.callback).Run(std::move(response));
  }
}

void CbControlChannel::OnMessage(const webrtc::DataBuffer& buffer) {
  // Signaling thread.
  if (buffer.binary) {
    LOG(WARNING) << "CbControlChannel: binary frame on control channel "
                    "(v1 expects JSON text) — dropped";
    return;
  }
  if (buffer.data.size() > kMaxInboundFrameBytes) {
    LOG(WARNING) << "CbControlChannel: oversized frame ("
                 << buffer.data.size() << " bytes) — dropped";
    return;
  }
  const char* data = reinterpret_cast<const char*>(buffer.data.data());
  HandleInboundJson(std::string(data, buffer.data.size()));
}

void CbControlChannel::HandleInboundJson(const std::string& raw) {
  // Parse on the signaling thread so a malformed frame costs no UI-thread
  // time, then hop with the already-validated pieces.
  std::optional<base::Value> parsed = base::JSONReader::Read(raw);
  if (!parsed || !parsed->is_dict()) {
    LOG(WARNING) << "CbControlChannel: inbound frame is not a JSON object "
                    "— dropped";
    return;
  }
  const base::Value::Dict& envelope = parsed->GetDict();

  const std::optional<int> v = envelope.FindInt("v");
  if (!v || *v != kProtocolVersion) {
    LOG(WARNING) << "CbControlChannel: unsupported protocol version — dropped";
    return;
  }
  const std::string* type = envelope.FindString("type");
  if (!type || *type != kTypeResponse) {
    // The guest only ever receives responses. Anything else is either a
    // portal bug or a future type we predate; ignoring is forward-safe.
    VLOG(1) << "CbControlChannel: ignoring inbound type "
            << (type ? *type : "<missing>");
    return;
  }
  const base::Value::Dict* data = envelope.FindDict("data");
  if (!data) {
    LOG(WARNING) << "CbControlChannel: response missing data — dropped";
    return;
  }
  const std::string* id = data->FindString("id");
  if (!id || id->empty()) {
    LOG(WARNING) << "CbControlChannel: response missing id — dropped";
    return;
  }

  ui_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&CbControlChannel::ResolvePending,
                                weak_factory_.GetWeakPtr(), *id,
                                data->Clone()));
}

void CbControlChannel::OnStateChange() {
  // Deliberately does NOT cancel pending requests on close.
  //
  // OnStateChange arrives on the signaling thread and libwebrtc gives no
  // state argument here; more importantly, a transient close during an
  // ICE blip should not nuke a dialog the user is mid-way through
  // answering — the per-request deadline already bounds that case. The
  // authoritative cancel points are session teardown and WebContents
  // destruction, both of which call CancelAllPending() on the UI sequence.
}

void CbControlChannel::OnBufferedAmountChange(uint64_t /*sent_data_size*/) {
  // Control frames are small and infrequent; no backpressure gate needed.
  // Bulk senders on kFiles use CbDataChannelHost::GetBufferedAmount.
}

bool CbControlChannel::IsOkToCallOnTheNetworkThread() {
  // Keep delivery on the signaling thread. Matches CbInputDispatch: letting
  // libwebrtc call us on the network thread would create a second source of
  // concurrent OnMessage calls.
  return false;
}

void CbControlChannel::OnMessageForTesting(const std::string& json) {
  HandleInboundJson(json);
}

size_t CbControlChannel::PendingCountForTesting() const {
  return pending_.size();
}

}  // namespace cloud_browser

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cloud-browser/capture/build-integration/cb_file_upload_relay.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "cloud-browser/capture/signaling/cb_dc_host.h"
#include "third_party/webrtc/api/data_channel_interface.h"

namespace cloud_browser {

namespace {

// Burst-rate-limited error log. Same cadence as
// cb_stats_relay.cc::ShouldLogFailure and cb_clipboard_relay.cc's copy
// (1st of burst, every 10th thereafter, every 100th past 100). Keeping
// the cadence aligned makes operator-visible log volume predictable
// across the M6 family.
bool ShouldLogFailure(int64_t consecutive_failures) {
  if (consecutive_failures == 1) return true;
  if (consecutive_failures < 100 && consecutive_failures % 10 == 0)
    return true;
  if (consecutive_failures % 100 == 0) return true;
  return false;
}

// IEqualOff — case-insensitive string compare for the "off" sentinel
// check. Matches the sentinel check in cb_stats_relay.cc and
// cb_clipboard_relay.cc, and the shape JS's url.toLowerCase() === "off"
// used in streamer.js.
bool IEqualOff(const std::string& s) {
  return base::EqualsCaseInsensitiveASCII(s, kFileUploadSentinelOff);
}

// Cap the reconnect backoff. 30 s matches the rest of the M6 family
// (and is well under any operator-visible incident window).
constexpr base::TimeDelta kReconnectBackoffMax = base::Seconds(30);

}  // namespace

// ---------------------------------------------------------------------
// CbFileUploadBridgeWsClient
// ---------------------------------------------------------------------

CbFileUploadBridgeWsClient::CbFileUploadBridgeWsClient(
    std::string url,
    scoped_refptr<base::SequencedTaskRunner> io_task_runner)
    : url_(std::move(url)),
      disabled_(url_.empty() || IEqualOff(url_)),
      io_task_runner_(std::move(io_task_runner)) {
  if (disabled_) {
    LOG(INFO) << "CbFileUploadBridgeWsClient: emit disabled (url="
              << url_ << ")";
  } else {
    LOG(INFO) << "CbFileUploadBridgeWsClient: bridge → " << url_;
    DCHECK(io_task_runner_) << "io_task_runner must be supplied when "
                               "the client is not disabled";
  }
}

CbFileUploadBridgeWsClient::~CbFileUploadBridgeWsClient() = default;

void CbFileUploadBridgeWsClient::SetOnFrameReceived(
    OnFrameReceivedCallback cb) {
  if (disabled_) return;
  if (!io_task_runner_->RunsTasksInCurrentSequence()) {
    io_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&CbFileUploadBridgeWsClient::SetOnFrameReceived,
                       weak_factory_.GetWeakPtr(), std::move(cb)));
    return;
  }
  on_frame_received_ = std::move(cb);
}

void CbFileUploadBridgeWsClient::PostText(std::string frame) {
  if (disabled_) return;
  // Defence-in-depth cap. The bridge enforces 1 MiB raw chunks; we
  // permit ~1.5 MiB on the wire (envelope + base64 inflation) and
  // drop oversize here so a misconfigured peer cannot pin the IO
  // thread on a giant body.
  if (frame.size() > kMaxFileUploadFrameBytes) {
    LOG(WARNING) << "CbFileUploadBridgeWsClient: dropping oversized "
                    "inbound frame (" << frame.size() << " bytes > "
                 << kMaxFileUploadFrameBytes << ")";
    return;
  }
  if (!io_task_runner_->RunsTasksInCurrentSequence()) {
    io_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&CbFileUploadBridgeWsClient::PostOnIoSequence,
                       weak_factory_.GetWeakPtr(), std::move(frame)));
    return;
  }
  PostOnIoSequence(std::move(frame));
}

void CbFileUploadBridgeWsClient::PostOnIoSequence(std::string frame) {
  DCHECK(io_task_runner_->RunsTasksInCurrentSequence());

  // TODO(M6-R3-ws-backend): wire to net/websockets/WebSocketChannel
  // (parity with chromium's DevTools WS dialer) or services/network's
  // WebSocket here. The contract for the production impl:
  //   1. Lazy-connect on first PostText. EnsureConnected() spins up
  //      a fresh WS handshake against url_; reconnects are gated by
  //      reconnect_backoff_ (1 s initial, *2 up to 30 s cap).
  //   2. On open, the read-loop pump starts; each text frame the
  //      bridge sends invokes OnFrameRead → on_frame_received_.
  //   3. On open, drain a single frame and OnFrameSent. We do NOT
  //      buffer across reconnects — see header comment for the
  //      ordering rationale (the v1 protocol's monotone seq within
  //      an upload makes mid-stream queuing dangerous).
  //   4. On any IO error, OnTransportDisconnected, increment
  //      consecutive_send_failures_ via RecordSendFailure, and
  //      schedule the next reconnect via reconnect_backoff_.
  //
  // For the DRAFT, log + count as if the frame was sent so the rest
  // of the relay (test seam in cb_file_upload_relay_test.cc) can
  // land and exercise the dispatch path. Production binding lands
  // in the companion CL.
  EnsureConnected();
  // Pretend-send. The real impl will await an OnWriteComplete from
  // the WS layer; the test seam can synthesise OnFrameSent.
  OnFrameSent(true, /*net_error_or_status=*/0);
  (void)frame;
}

void CbFileUploadBridgeWsClient::DispatchInboundForTest(std::string frame) {
  DCHECK(io_task_runner_->RunsTasksInCurrentSequence());
  OnFrameRead(std::move(frame));
}

void CbFileUploadBridgeWsClient::EnsureConnected() {
  // TODO(M6-R3-ws-backend): real connect. R3 draft leaves this empty
  // so the test fake can subclass without inheriting a partial
  // connect implementation.
}

void CbFileUploadBridgeWsClient::OnTransportConnected() {
  reconnect_backoff_ = base::Seconds(1);
  reconnect_pending_ = false;
  LOG(INFO) << "CbFileUploadBridgeWsClient: transport connected";
}

void CbFileUploadBridgeWsClient::OnTransportDisconnected(
    int net_error_or_status) {
  LOG(WARNING) << "CbFileUploadBridgeWsClient: transport disconnected "
                  "code=" << net_error_or_status;
  // Double the backoff up to the cap. The next PostText hits
  // EnsureConnected which will respect reconnect_pending_ +
  // reconnect_backoff_ via the production wiring.
  reconnect_backoff_ =
      std::min(reconnect_backoff_ * 2, kReconnectBackoffMax);
  reconnect_pending_ = true;
}

void CbFileUploadBridgeWsClient::OnFrameSent(bool ok,
                                             int net_error_or_status) {
  if (ok) {
    RecordSendSuccess();
  } else {
    RecordSendFailure(net_error_or_status);
  }
}

void CbFileUploadBridgeWsClient::OnFrameRead(std::string frame) {
  DCHECK(io_task_runner_->RunsTasksInCurrentSequence());
  ++frames_received_;

  // Defence-in-depth inbound cap. Server→client envelopes are all
  // small (progress/complete/error <4 KiB) but a wedged bridge that
  // emits a giant body shouldn't be able to push it across to the DC.
  if (frame.size() > kMaxFileUploadFrameBytes) {
    ++frames_dropped_oversize_inbound_;
    LOG(WARNING) << "CbFileUploadBridgeWsClient: dropping oversized "
                    "outbound (bridge→DC) frame ("
                 << frame.size() << " bytes > "
                 << kMaxFileUploadFrameBytes
                 << "; dropped_total="
                 << frames_dropped_oversize_inbound_ << ")";
    return;
  }

  if (on_frame_received_.is_null()) {
    // Read loop is running before the relay wired its callback. Drop;
    // not an error — the test fake can race the order, and in
    // production EnsureConnected runs after SetOnFrameReceived.
    return;
  }
  on_frame_received_.Run(std::move(frame));
}

void CbFileUploadBridgeWsClient::RecordSendSuccess() {
  ++send_success_count_;
  if (consecutive_send_failures_ > 0) {
    LOG(INFO) << "CbFileUploadBridgeWsClient: send recovered "
                 "after_failures=" << consecutive_send_failures_;
    consecutive_send_failures_ = 0;
  }
}

void CbFileUploadBridgeWsClient::RecordSendFailure(
    int net_error_or_status) {
  ++consecutive_send_failures_;
  if (ShouldLogFailure(consecutive_send_failures_)) {
    LOG(WARNING) << "CbFileUploadBridgeWsClient: send failed code="
                 << net_error_or_status
                 << " consecutive=" << consecutive_send_failures_;
  }
}

// ---------------------------------------------------------------------
// CbFileUploadRelay
// ---------------------------------------------------------------------

CbFileUploadRelay::CbFileUploadRelay(
    std::unique_ptr<CbFileUploadBridgeWsClient> client,
    signaling::CbDataChannelHost* dc_host)
    : client_(std::move(client)),
      dc_host_(dc_host) {
  DCHECK(client_);
  if (!dc_host_) {
    LOG(INFO) << "CbFileUploadRelay: dc_host=nullptr, outbound "
                 "(bridge→DC) path disabled — relay will drop reply "
                 "frames";
  }
  // Wire the inbound (bridge→DC) callback BEFORE any real connect
  // can fire so the WS read-loop always has somewhere to dispatch.
  // The client's SetOnFrameReceived is a no-op when the client is
  // disabled, so this is safe in both states.
  client_->SetOnFrameReceived(base::BindRepeating(
      &CbFileUploadRelay::OnBridgeReplyFrame,
      weak_factory_.GetWeakPtr()));
}

CbFileUploadRelay::~CbFileUploadRelay() = default;

void CbFileUploadRelay::OnMessage(const webrtc::DataBuffer& buffer) {
  // The v1 file-upload wire is JSON text (chunks carry base64 inside
  // the envelope). Binary frames are unexpected — the JS shape
  // (streamer.js's file-upload relay) logged-and-dropped them; same
  // here. A binary frame on this label is client-side drift or wrong-
  // channel routing.
  if (buffer.binary) {
    LOG(WARNING) << "CbFileUploadRelay: binary frame on files channel,"
                    " dropping (v1 expects JSON text)";
    return;
  }
  if (client_->disabled()) return;

  // Forward the raw body. The relay deliberately does NOT parse the
  // envelope — that's the bridge's job (v=1, type=file_upload_*,
  // upload_id, SHA-256 verification, size cap, MIME allowlist, path-
  // traversal sanitisation). A parse step here would couple us to
  // the wire shape, which is owned by the portal client + bridge
  // pair (docs/protocols/file-upload.md).
  const char* data = reinterpret_cast<const char*>(buffer.data.data());
  std::string raw(data, buffer.data.size());
  client_->PostText(std::move(raw));
}

void CbFileUploadRelay::OnStateChange() {
  // R3 no-op. Channel state transitions are observed by the M3 host
  // (CbDataChannelHostObserver::OnChannelStateChanged) and fanned
  // out to M6 R1's CbWebrtcEventEmitter as the dc.opened beacon.
  // Relay itself doesn't react to open/close — a closed channel
  // just stops delivering OnMessage, which is the desired behaviour.
  //
  // TODO(M6-R3-state-on-close): if a future R# wants to drain the
  // WS reply pump on channel close (so a stale bridge reply doesn't
  // race a freshly-rebound observer), do that here. R3 ships
  // without the drain because the bridge's reply correlation is
  // upload_id-keyed and a stale reply for a vanished upload is
  // dropped by the client.
}

void CbFileUploadRelay::OnBufferedAmountChange(
    uint64_t /*sent_data_size*/) {
  // R3 no-op. Outbound DC sends go through dc_host_->Send, which
  // routes through the host's own buffer accounting. Backpressure
  // on the inbound (client→bridge) direction is the client's
  // responsibility per docs/protocols/file-upload.md § Backpressure.
}

bool CbFileUploadRelay::IsOkToCallOnTheNetworkThread() {
  // Stay on the signaling thread. The WS hop is handled inside
  // CbFileUploadBridgeWsClient, which PostTasks onto io_task_runner_
  // before touching the transport; OnMessage on the signaling thread
  // is fine.
  return false;
}

void CbFileUploadRelay::OnBridgeReplyFrame(std::string frame) {
  // Fires on io_task_runner_ via the client's read-loop. The cap was
  // already applied inside the client (kMaxFileUploadFrameBytes); we
  // re-check as a courtesy because future refactors could move the
  // cap or the client could subclass to bypass it.
  if (!FrameWithinCap(frame)) {
    ++outbound_frames_dropped_oversize_;
    LOG(WARNING) << "CbFileUploadRelay: dropping oversized outbound "
                    "frame (" << frame.size() << " bytes > "
                 << kMaxFileUploadFrameBytes
                 << "; dropped_total="
                 << outbound_frames_dropped_oversize_ << ")";
    return;
  }

  if (dc_host_ == nullptr) {
    ++outbound_frames_dropped_no_host_;
    // Rate-limited so we don't flood when the bridge bounces while
    // the host is intentionally unwired (e.g. during test setup).
    if (ShouldLogFailure(outbound_frames_dropped_no_host_)) {
      LOG(WARNING) << "CbFileUploadRelay: dropping outbound frame, "
                      "dc_host=nullptr; dropped_total="
                   << outbound_frames_dropped_no_host_;
    }
    return;
  }

  // dc_host_->Send is thread-safe by contract (M3 R5 header); it
  // PostTasks onto its own signaling runner before touching
  // DataChannelInterface::Send. We pass through.
  auto result = dc_host_->Send(signaling::CbDcLabel::kFiles, frame);
  if (!result.ok()) {
    ++consecutive_outbound_send_failures_;
    if (ShouldLogFailure(consecutive_outbound_send_failures_)) {
      LOG(WARNING) << "CbFileUploadRelay: dc_host->Send failed code="
                   << static_cast<int>(result.type())
                   << " consecutive="
                   << consecutive_outbound_send_failures_;
    }
    return;
  }
  ++outbound_frames_forwarded_;
  if (consecutive_outbound_send_failures_ > 0) {
    LOG(INFO) << "CbFileUploadRelay: outbound send recovered "
                 "after_failures=" << consecutive_outbound_send_failures_;
    consecutive_outbound_send_failures_ = 0;
  }
}

bool CbFileUploadRelay::FrameWithinCap(const std::string& frame) const {
  return frame.size() <= kMaxFileUploadFrameBytes;
}

// ---------------------------------------------------------------------
// Env-var helpers.
// ---------------------------------------------------------------------

namespace {

std::string EnvOrDefault(const char* var, const char* fallback) {
  const char* raw = std::getenv(var);
  if (raw == nullptr || *raw == '\0') return fallback;
  return std::string(raw);
}

}  // namespace

std::string ResolveFileUploadBridgeUrl() {
  return EnvOrDefault("CHROMELESS_FILE_UPLOAD_BRIDGE_URL",
                      kDefaultFileBridgeWsUrl);
}

}  // namespace cloud_browser

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cloud-browser/capture/build-integration/cb_clipboard_relay.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "cloud-browser/capture/signaling/cb_dc_host.h"
#include "third_party/webrtc/api/data_channel_interface.h"

namespace cloud_browser {

namespace {

// Hard 1 MiB cap mirroring clipboard-bridge/main.go's maxBytes. The
// bridge already enforces; this is defence-in-depth so a wedged
// portal or wedged bridge cannot push a giant frame across our WS
// hop. Matches the v1 protocol cap documented in
// docs/protocols/clipboard-channel.md.
constexpr size_t kMaxClipboardFrameBytes = 1 << 20;

// Burst-rate-limited error log. Same cadence as
// cb_stats_relay.cc::ShouldLogFailure (1st of burst, every 10th
// thereafter, every 100th past 100). Keeping the cadence aligned
// makes operator-visible log volume predictable across the M6
// family.
bool ShouldLogFailure(int64_t consecutive_failures) {
  if (consecutive_failures == 1) return true;
  if (consecutive_failures < 100 && consecutive_failures % 10 == 0)
    return true;
  if (consecutive_failures % 100 == 0) return true;
  return false;
}

// IEqual — case-insensitive string compare for the "off" sentinel
// check. Matches the sentinel check in cb_stats_relay.cc and the
// shape JS's url.toLowerCase() === "off" used in streamer.js.
bool IEqualOff(const std::string& s) {
  return base::EqualsCaseInsensitiveASCII(s, kClipboardSentinelOff);
}

// Cap the reconnect backoff. 30 s matches the rest of the M6 family
// (and is well under any operator-visible incident window).
constexpr base::TimeDelta kReconnectBackoffMax = base::Seconds(30);

}  // namespace

// ---------------------------------------------------------------------
// CbClipboardBridgeWsClient
// ---------------------------------------------------------------------

CbClipboardBridgeWsClient::CbClipboardBridgeWsClient(
    std::string label,
    std::string url,
    scoped_refptr<base::SequencedTaskRunner> io_task_runner)
    : label_(std::move(label)),
      url_(std::move(url)),
      disabled_(url_.empty() || IEqualOff(url_)),
      io_task_runner_(std::move(io_task_runner)) {
  if (disabled_) {
    LOG(INFO) << "CbClipboardBridgeWsClient[" << label_
              << "]: emit disabled (url=" << url_ << ")";
  } else {
    LOG(INFO) << "CbClipboardBridgeWsClient[" << label_
              << "]: emit → " << url_;
    DCHECK(io_task_runner_) << "io_task_runner must be supplied when "
                               "the client is not disabled";
  }
}

CbClipboardBridgeWsClient::~CbClipboardBridgeWsClient() = default;

void CbClipboardBridgeWsClient::PostText(std::string frame) {
  if (disabled_) return;
  // Defence-in-depth cap. The bridge already enforces 1 MiB; we drop
  // here too so a misconfigured bridge cannot pin the relay's IO
  // thread on a giant body.
  if (frame.size() > kMaxClipboardFrameBytes) {
    LOG(WARNING) << "CbClipboardBridgeWsClient[" << label_
                 << "]: dropping oversized inbound frame ("
                 << frame.size() << " bytes > "
                 << kMaxClipboardFrameBytes << ")";
    return;
  }
  if (!io_task_runner_->RunsTasksInCurrentSequence()) {
    io_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&CbClipboardBridgeWsClient::PostOnIoSequence,
                       weak_factory_.GetWeakPtr(), std::move(frame)));
    return;
  }
  PostOnIoSequence(std::move(frame));
}

void CbClipboardBridgeWsClient::PostOnIoSequence(std::string frame) {
  DCHECK(io_task_runner_->RunsTasksInCurrentSequence());

  // TODO(M6-R2-ws-backend): wire to net/websockets/WebSocketChannel
  // or services/network's WebSocket here. The contract for the
  // production impl:
  //   1. Lazy-connect on first PostText. EnsureConnected() spins up
  //      a fresh WS handshake against url_; reconnects are gated by
  //      reconnect_backoff_ (1 s initial, *2 up to 30 s cap).
  //   2. On open, drain a single frame and OnFrameSent. We do NOT
  //      buffer across reconnects — see header comment for the
  //      ordering rationale.
  //   3. On any IO error, OnTransportDisconnected, increment
  //      consecutive_failures_ via RecordFailure, and schedule the
  //      next reconnect via reconnect_backoff_.
  //
  // For the DRAFT, log + count as if the frame was sent so the rest
  // of the relay (test seam in cb_clipboard_relay_test.cc) can land
  // and exercise the dispatch path. Production binding lands in the
  // companion CL.
  EnsureConnected();
  // Pretend-send. The real impl will await an OnWriteComplete from
  // the WS layer; the test seam can synthesise OnFrameSent.
  OnFrameSent(true, /*net_error_or_status=*/0);
  (void)frame;
}

void CbClipboardBridgeWsClient::EnsureConnected() {
  // TODO(M6-R2-ws-backend): real connect. R2 draft leaves this empty
  // so the test fake can subclass without inheriting a partial
  // connect implementation.
}

void CbClipboardBridgeWsClient::OnTransportConnected() {
  reconnect_backoff_ = base::Seconds(1);
  reconnect_pending_ = false;
  LOG(INFO) << "CbClipboardBridgeWsClient[" << label_
            << "]: transport connected";
}

void CbClipboardBridgeWsClient::OnTransportDisconnected(
    int net_error_or_status) {
  LOG(WARNING) << "CbClipboardBridgeWsClient[" << label_
               << "]: transport disconnected code=" << net_error_or_status;
  // Double the backoff up to the cap. The next PostText hits
  // EnsureConnected which will respect reconnect_pending_ +
  // reconnect_backoff_ via the production wiring.
  reconnect_backoff_ =
      std::min(reconnect_backoff_ * 2, kReconnectBackoffMax);
  reconnect_pending_ = true;
}

void CbClipboardBridgeWsClient::OnFrameSent(bool ok,
                                            int net_error_or_status) {
  if (ok) {
    RecordSuccess();
  } else {
    RecordFailure(net_error_or_status);
  }
}

void CbClipboardBridgeWsClient::RecordSuccess() {
  ++success_count_;
  if (consecutive_failures_ > 0) {
    LOG(INFO) << "CbClipboardBridgeWsClient[" << label_
              << "]: recovered after_failures=" << consecutive_failures_;
    consecutive_failures_ = 0;
  }
}

void CbClipboardBridgeWsClient::RecordFailure(int net_error_or_status) {
  ++consecutive_failures_;
  if (ShouldLogFailure(consecutive_failures_)) {
    LOG(WARNING) << "CbClipboardBridgeWsClient[" << label_
                 << "]: send failed code=" << net_error_or_status
                 << " consecutive=" << consecutive_failures_;
  }
}

// ---------------------------------------------------------------------
// CbClipboardRelay
// ---------------------------------------------------------------------

CbClipboardRelay::CbClipboardRelay(
    std::unique_ptr<CbClipboardBridgeWsClient> client)
    : client_(std::move(client)) {
  DCHECK(client_);
}

CbClipboardRelay::~CbClipboardRelay() = default;

void CbClipboardRelay::OnMessage(const webrtc::DataBuffer& buffer) {
  // The v1 clipboard wire shape is JSON text. Binary frames are
  // unexpected — the JS shape (streamer.js's clipboard handler)
  // logged-and-dropped them, same here. A binary frame on this label
  // is either client-side drift or someone routing the wrong proto
  // down the wrong channel.
  if (buffer.binary) {
    LOG(WARNING) << "CbClipboardRelay: binary frame on clipboard "
                    "channel, dropping (v1 expects JSON text)";
    return;
  }
  if (client_->disabled()) return;

  // Forward the raw body. The relay deliberately does NOT parse the
  // envelope — that's the bridge's job (v=1, type=clipboard_offer,
  // source=user_action, direction, 1 MiB cap). A parse step here
  // would couple us to the wire shape, which is owned by the
  // portal + bridge pair (TODO(M6-R2-protocol-fixture): formalise
  // the shape in docs/protocols/clipboard-channel.md and link from
  // here).
  const char* data = reinterpret_cast<const char*>(buffer.data.data());
  std::string raw(data, buffer.data.size());
  client_->PostText(std::move(raw));
}

void CbClipboardRelay::OnStateChange() {
  // R2 no-op. Channel state transitions are observed by the M3 host
  // (CbDataChannelHostObserver::OnChannelStateChanged) and fanned out
  // to M6 R1's CbWebrtcEventEmitter as the dc.opened beacon. Relay
  // itself doesn't react to open/close — a closed channel just stops
  // delivering OnMessage, which is the desired behaviour.
}

void CbClipboardRelay::OnBufferedAmountChange(
    uint64_t /*sent_data_size*/) {
  // R2 no-op. The "clipboard" channel is bidirectional but the relay
  // never directly buffers DC sends — outbound goes through
  // dc_host_->Send via CbClipboardOutboundServer. The host owns
  // BufferedAmount surfacing if any future R# wants to backpressure.
}

bool CbClipboardRelay::IsOkToCallOnTheNetworkThread() {
  // Stay on the signaling thread. The WS hop is handled inside
  // CbClipboardBridgeWsClient, which PostTasks onto io_task_runner_
  // before touching the transport; OnMessage on the signaling thread
  // is fine.
  return false;
}

// ---------------------------------------------------------------------
// CbClipboardOutboundServer
// ---------------------------------------------------------------------

CbClipboardOutboundServer::CbClipboardOutboundServer(
    signaling::CbDataChannelHost* dc_host,
    std::string listen_addr,
    std::string listen_path,
    scoped_refptr<base::SequencedTaskRunner> io_task_runner)
    : dc_host_(dc_host),
      listen_addr_(std::move(listen_addr)),
      listen_path_(std::move(listen_path)),
      disabled_(listen_addr_.empty() || IEqualOff(listen_addr_)),
      io_task_runner_(std::move(io_task_runner)) {
  DCHECK(dc_host_);
  if (disabled_) {
    LOG(INFO) << "CbClipboardOutboundServer: bind disabled (addr="
              << listen_addr_ << ")";
  } else {
    LOG(INFO) << "CbClipboardOutboundServer: will accept "
              << listen_addr_ << listen_path_;
    DCHECK(io_task_runner_) << "io_task_runner must be supplied when "
                               "the server is not disabled";
  }
}

CbClipboardOutboundServer::~CbClipboardOutboundServer() {
  Shutdown();
}

bool CbClipboardOutboundServer::Start() {
  if (disabled_) {
    listening_ = false;
    return true;
  }
  if (listening_) return true;

  // TODO(M6-R2-ws-server): bind a net::server::HttpServer instance,
  // route requests on listen_path_ to a WebSocket upgrade handler,
  // and route each text frame from the upgraded socket through
  // OnFrameReceived. The handler runs on io_task_runner_. The bridge
  // re-dials on transport loss as part of its own loop
  // (clipboard-bridge/main.go's wsEmitter), so the server's
  // accept-loop being single-connection is fine.
  //
  // For the DRAFT, mark listening_ true so dependent code can wire
  // up. The companion CL lands the real bind.
  listening_ = true;
  LOG(INFO) << "CbClipboardOutboundServer: listening (stub)";
  return true;
}

void CbClipboardOutboundServer::Shutdown() {
  if (!listening_) return;
  listening_ = false;
  LOG(INFO) << "CbClipboardOutboundServer: shutdown";
  // TODO(M6-R2-ws-server): tear down the server instance + any
  // active connection here.
}

void CbClipboardOutboundServer::OnFrameReceived(std::string frame) {
  DCHECK(io_task_runner_->RunsTasksInCurrentSequence());
  ++frames_received_;

  if (!FrameWithinCap(frame)) {
    ++frames_dropped_oversize_;
    LOG(WARNING) << "CbClipboardOutboundServer: dropping oversized "
                    "outbound frame (" << frame.size() << " bytes > "
                 << kMaxClipboardFrameBytes
                 << "; dropped_total=" << frames_dropped_oversize_ << ")";
    return;
  }

  // The bridge already validated the envelope shape (v, type,
  // source, direction). A relay-side parse here would couple us to
  // the wire shape. Forward verbatim.
  ForwardToDc(std::move(frame));
}

void CbClipboardOutboundServer::ForwardToDc(std::string text) {
  // dc_host_->Send is thread-safe by contract (M3 R5 header); it
  // PostTasks onto its own signaling runner before touching
  // DataChannelInterface::Send. We pass through.
  auto result = dc_host_->Send(signaling::CbDcLabel::kClipboard, text);
  if (!result.ok()) {
    ++consecutive_send_failures_;
    if (ShouldLogFailure(consecutive_send_failures_)) {
      LOG(WARNING) << "CbClipboardOutboundServer: dc_host->Send "
                      "failed code=" << static_cast<int>(result.type())
                   << " consecutive=" << consecutive_send_failures_;
    }
    return;
  }
  if (consecutive_send_failures_ > 0) {
    LOG(INFO) << "CbClipboardOutboundServer: recovered after_failures="
              << consecutive_send_failures_;
    consecutive_send_failures_ = 0;
  }
}

bool CbClipboardOutboundServer::FrameWithinCap(
    const std::string& frame) const {
  return frame.size() <= kMaxClipboardFrameBytes;
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

std::string ResolveClipboardInboundUrl() {
  return EnvOrDefault("CHROMELESS_CLIPBOARD_INBOUND_URL",
                      kDefaultBridgeInboundUrl);
}

std::string ResolveClipboardOutboundAddr() {
  return EnvOrDefault("CHROMELESS_CLIPBOARD_OUTBOUND_ADDR",
                      kDefaultRelayOutboundAddr);
}

std::string ResolveClipboardOutboundPath() {
  return EnvOrDefault("CHROMELESS_CLIPBOARD_OUTBOUND_PATH",
                      kDefaultRelayOutboundPath);
}

}  // namespace cloud_browser

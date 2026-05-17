// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cloud-browser/capture/build-integration/cb_stats_relay.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "net/base/load_flags.h"
#include "net/http/http_request_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "third_party/webrtc/api/data_channel_interface.h"
#include "url/gurl.h"

namespace cloud_browser {

namespace {

// Burst-rate-limited error log. Returns true when the caller should
// emit a LOG line for this failure (1st of a burst, every 10th, then
// every 100th to bound log volume during long outages). Matches the
// streamer.js StatsRelay/MetricsEmitter pattern.
bool ShouldLogFailure(int64_t consecutive_failures) {
  if (consecutive_failures == 1) return true;
  if (consecutive_failures < 100 && consecutive_failures % 10 == 0)
    return true;
  if (consecutive_failures % 100 == 0) return true;
  return false;
}

// Network traffic annotation. Required by network::SimpleURLLoader —
// see services/network/public/cpp/resource_request.h. The annotation
// documents what this request is, who triggered it, and what user
// data it carries; auditing infra reads it.
constexpr net::NetworkTrafficAnnotationTag kStatsRelayTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("cloud_browser_metrics_sidecar",
                                        R"(
    semantics {
      sender: "Cloud Browser WebRTC Metrics Relay"
      description:
        "Posts WebRTC stats samples and lifecycle event beacons to "
        "the chromeless-metrics-sidecar service running on loopback "
        "(http://localhost:9100). The sidecar increments Prometheus "
        "counters/histograms and re-exports the events through OTLP "
        "for the chromeless observability dashboards. Replaces the "
        "JS-side fetch() calls in streamer.js's StatsRelay and "
        "MetricsEmitter classes (deleted in M7 of the ChromelessV2 "
        "native-peer cutover)."
      trigger:
        "(1) The peer's client opens an RTCDataChannel('stats') and "
        "emits a {v,t,sample} envelope once per second — every such "
        "frame is forwarded. (2) PeerConnection lifecycle state "
        "transitions (ICE connect/fail, DC open, session create/"
        "close) each generate one beacon."
      data:
        "Stats relay: the raw WebRTC RTCStatsReport-derived envelope "
        "produced by the client (timestamps + connection-level "
        "counters; no user content). Event beacons: event name + a "
        "small attrs dict (label, ice_state, duration_ms, "
        "handshake_ms, session_id — opaque per-bootstrap UUID)."
      destination: LOCAL
    }
    policy {
      cookies_allowed: NO
      setting:
        "This request is internal to the chromeless container; the "
        "sidecar runs on the same loopback interface and is not "
        "user-configurable. To disable observability entirely, set "
        "CHROMELESS_METRICS_URL=off and CHROMELESS_WEBRTC_METRICS_URL="
        "off in the chromium pod's env."
      policy_exception_justification:
        "Not implemented — no policy gates pod-internal metrics "
        "telemetry. The sidecar's OTLP exporter respects the "
        "platform observability opt-out at the collector layer."
    })");

// IEqual — case-insensitive string compare for the "off" sentinel
// check. Matches JS's url.toLowerCase() === "off".
bool IEqualOff(const std::string& s) {
  return base::EqualsCaseInsensitiveASCII(s, kSentinelOff);
}

// Build a "v=1" session_id. base::RandBytes + hex is plenty for what
// is effectively a per-bootstrap nonce; the JS shape uses
// crypto.randomUUID. We use a 16-byte hex string to keep the same
// effective entropy and a recognisable length.
std::string MintSessionId() {
  // Modern chromium base/rand_util.h removed the (void*, size_t) overload;
  // use the vector-returning API instead.
  auto bytes = base::RandBytesAsVector(16);
  std::string hex;
  hex.reserve(2 * bytes.size());
  static constexpr char kHex[] = "0123456789abcdef";
  for (uint8_t b : bytes) {
    hex.push_back(kHex[b >> 4]);
    hex.push_back(kHex[b & 0x0f]);
  }
  // Prefix with "s" so the value sorts before any other session-id
  // shape we might mint in future (matches the JS short-id format
  // in streamer.js MintSessionId).
  return "s" + hex;
}

}  // namespace

// ---------------------------------------------------------------------
// CbMetricsSidecarClient
// ---------------------------------------------------------------------

CbMetricsSidecarClient::CbMetricsSidecarClient(
    std::string label,
    std::string url,
    scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
    scoped_refptr<base::SequencedTaskRunner> http_task_runner)
    : label_(std::move(label)),
      url_(std::move(url)),
      disabled_(url_.empty() || IEqualOff(url_)),
      loader_factory_(std::move(loader_factory)),
      http_task_runner_(std::move(http_task_runner)) {
  if (disabled_) {
    LOG(INFO) << "CbMetricsSidecarClient[" << label_
              << "]: emit disabled (url=" << url_ << ")";
  } else {
    LOG(INFO) << "CbMetricsSidecarClient[" << label_
              << "]: emit → " << url_;
    DCHECK(loader_factory_) << "loader_factory must be supplied when "
                               "the relay is not disabled";
    DCHECK(http_task_runner_) << "http_task_runner must be supplied "
                                 "when the relay is not disabled";
  }
}

CbMetricsSidecarClient::~CbMetricsSidecarClient() = default;

void CbMetricsSidecarClient::PostFireAndForget(std::string body,
                                               std::string content_type) {
  if (disabled_) return;
  if (!http_task_runner_->RunsTasksInCurrentSequence()) {
    http_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&CbMetricsSidecarClient::PostOnHttpSequence,
                       weak_factory_.GetWeakPtr(),
                       std::move(body),
                       std::move(content_type)));
    return;
  }
  PostOnHttpSequence(std::move(body), std::move(content_type));
}

void CbMetricsSidecarClient::PostOnHttpSequence(std::string body,
                                                std::string content_type) {
  DCHECK(http_task_runner_->RunsTasksInCurrentSequence());

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(url_);
  request->method = "POST";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->load_flags = net::LOAD_DISABLE_CACHE | net::LOAD_BYPASS_CACHE;
  request->headers.SetHeader(net::HttpRequestHeaders::kContentType,
                             content_type);

  auto loader = network::SimpleURLLoader::Create(
      std::move(request), kStatsRelayTrafficAnnotation);
  // No retry — fire-and-forget. The next sample is at most 1 s away
  // (stats relay) and lifecycle beacons are best-effort. JS shape
  // matches: no retry, no await on result.
  loader->SetRetryOptions(0, network::SimpleURLLoader::RETRY_NEVER);
  // 5 s timeout so a wedged sidecar doesn't pile up loaders. The
  // happy path responds in <10 ms on loopback.
  loader->SetTimeoutDuration(base::Seconds(5));
  loader->AttachStringForUpload(std::move(body), content_type);

  const int64_t id = next_loader_id_++;
  network::SimpleURLLoader* loader_raw = loader.get();
  in_flight_.emplace(id, std::move(loader));

  loader_raw->DownloadToString(
      loader_factory_.get(),
      base::BindOnce(&CbMetricsSidecarClient::OnLoadComplete,
                     weak_factory_.GetWeakPtr(), id),
      // 64 KB response-body cap — the sidecar never replies with
      // more than a few bytes, and we'd drop the body anyway. Cap
      // is a defense against a misconfigured sidecar.
      /*max_body_size=*/64 * 1024);
}

void CbMetricsSidecarClient::OnLoadComplete(
    int64_t loader_id,
    std::unique_ptr<std::string> /*response_body*/) {
  DCHECK(http_task_runner_->RunsTasksInCurrentSequence());

  auto it = in_flight_.find(loader_id);
  if (it == in_flight_.end()) {
    // Should be impossible; defensive.
    LOG(WARNING) << "CbMetricsSidecarClient[" << label_
                 << "]: stale loader id=" << loader_id;
    return;
  }

  network::SimpleURLLoader* loader = it->second.get();
  const int net_error = loader->NetError();
  const int http_status =
      loader->ResponseInfo() && loader->ResponseInfo()->headers
          ? loader->ResponseInfo()->headers->response_code()
          : 0;

  // Success: net_error == OK AND http_status in [200, 300).
  const bool ok =
      net_error == net::OK && http_status >= 200 && http_status < 300;
  if (ok) {
    RecordSuccess();
  } else {
    // Prefer http_status when present, else net_error (negative).
    RecordFailure(http_status != 0 ? http_status : net_error);
  }
  in_flight_.erase(it);
}

void CbMetricsSidecarClient::RecordSuccess() {
  ++success_count_;
  if (consecutive_failures_ > 0) {
    LOG(INFO) << "CbMetricsSidecarClient[" << label_
              << "]: recovered after_failures=" << consecutive_failures_;
    consecutive_failures_ = 0;
  }
}

void CbMetricsSidecarClient::RecordFailure(int net_error_or_http_status) {
  ++consecutive_failures_;
  if (ShouldLogFailure(consecutive_failures_)) {
    LOG(WARNING) << "CbMetricsSidecarClient[" << label_
                 << "]: POST failed code=" << net_error_or_http_status
                 << " consecutive=" << consecutive_failures_;
  }
}

// ---------------------------------------------------------------------
// CbStatsRelay
// ---------------------------------------------------------------------

CbStatsRelay::CbStatsRelay(std::unique_ptr<CbMetricsSidecarClient> client)
    : client_(std::move(client)) {
  DCHECK(client_);
}

CbStatsRelay::~CbStatsRelay() = default;

void CbStatsRelay::OnMessage(const webrtc::DataBuffer& buffer) {
  // The v1 stats wire shape is JSON text (RTCStatsReport-derived
  // envelope from the client). Binary frames are unexpected; the JS
  // shape skips them with a warn. We do the same; binary on stats is
  // either client-side drift or someone sending a different protocol
  // down the wrong channel.
  if (buffer.binary) {
    LOG(WARNING) << "CbStatsRelay: binary frame on stats channel, "
                    "dropping (v1 expects JSON text)";
    return;
  }
  if (client_->disabled()) return;

  // Forward the raw body. We deliberately do NOT parse — the sidecar
  // expects the same envelope the client sends, and a parse step
  // here would couple us to the v1 stats schema, which is owned by
  // the client + sidecar pair (TODO(M6-R2-stats-schema): formalise
  // the shape in docs/protocols/stats-channel.md).
  const char* data = reinterpret_cast<const char*>(buffer.data.data());
  std::string raw(data, buffer.data.size());
  client_->PostFireAndForget(std::move(raw), "application/json");
}

void CbStatsRelay::OnStateChange() {
  // R1 no-op. Channel state transitions don't drive the relay; we
  // either get OnMessage or we don't. M6 R2 may want a state-change
  // counter (open/close/error) — deferred.
}

void CbStatsRelay::OnBufferedAmountChange(uint64_t /*sent_data_size*/) {
  // R1 no-op. The stats channel is consumer-side; we never send.
}

bool CbStatsRelay::IsOkToCallOnTheNetworkThread() {
  // Stay on the signaling thread. The HTTP hop is handled inside
  // CbMetricsSidecarClient, so OnMessage on the signaling thread is
  // fine.
  return false;
}

// ---------------------------------------------------------------------
// CbWebrtcEventEmitter
// ---------------------------------------------------------------------

CbWebrtcEventEmitter::CbWebrtcEventEmitter(
    std::unique_ptr<CbMetricsSidecarClient> client)
    : session_id_(MintSessionId()), client_(std::move(client)) {
  DCHECK(client_);
}

CbWebrtcEventEmitter::~CbWebrtcEventEmitter() = default;

void CbWebrtcEventEmitter::SetSignalingSessionId(std::string id) {
  base::AutoLock lock(mu_);
  signaling_session_id_ = std::move(id);
}

void CbWebrtcEventEmitter::EmitSessionCreated(std::string reason) {
  base::Value::Dict attrs;
  {
    base::AutoLock lock(mu_);
    if (session_created_) return;  // latched
    session_created_ = true;
    session_started_at_ = base::TimeTicks::Now();
    attrs.Set("session_id", session_id_);
    if (!signaling_session_id_.empty()) {
      attrs.Set("signaling_session", signaling_session_id_);
    }
    attrs.Set("reason", std::move(reason));
  }
  LOG(INFO) << "CbWebrtcEventEmitter: session_created";
  EmitInternal("chromeless.webrtc.session_created", std::move(attrs));
}

void CbWebrtcEventEmitter::EmitSessionClosed(std::string reason) {
  base::Value::Dict attrs;
  {
    base::AutoLock lock(mu_);
    if (!session_created_) return;  // never opened → never closes
    if (session_started_at_) {
      const auto delta = base::TimeTicks::Now() - *session_started_at_;
      attrs.Set("duration_ms", static_cast<int>(delta.InMilliseconds()));
    } else {
      attrs.Set("duration_ms", base::Value());
    }
    attrs.Set("session_id", session_id_);
    attrs.Set("reason", std::move(reason));

    // Latch: a second close is a no-op until MintNewSession().
    session_started_at_.reset();
    session_created_ = false;
  }
  LOG(INFO) << "CbWebrtcEventEmitter: session_closed";
  EmitInternal("chromeless.webrtc.session_closed", std::move(attrs));
}

void CbWebrtcEventEmitter::EmitIceConnected(std::string ice_state) {
  base::Value::Dict attrs;
  {
    base::AutoLock lock(mu_);
    // Cancel any pending ice.failed debounce; ICE recovered.
    if (ice_failed_debounce_) {
      ice_failed_debounce_->Cancel();
      ice_failed_debounce_.reset();
    }
    attrs.Set("session_id", session_id_);
    attrs.Set("ice_state", std::move(ice_state));
  }
  EmitInternal("chromeless.webrtc.ice.connected", std::move(attrs));
}

void CbWebrtcEventEmitter::FireIceFailed(
    std::string trigger_state,
    std::function<std::string()> current_state_probe) {
  // Probe current ICE state; skip emit if it has recovered. The
  // probe is a std::function rather than a base::OnceCallback so
  // tests can pass a tiny lambda without dragging in the callback
  // bind machinery.
  std::string final_state =
      current_state_probe ? current_state_probe() : std::string("unknown");
  if (final_state == "connected" || final_state == "completed") {
    // Recovered between trigger and fire — JS shape: skip.
    base::AutoLock lock(mu_);
    ice_failed_debounce_.reset();
    return;
  }
  base::Value::Dict attrs;
  {
    base::AutoLock lock(mu_);
    attrs.Set("session_id", session_id_);
    attrs.Set("ice_state", final_state);
    attrs.Set("reason", trigger_state);
    ice_failed_debounce_.reset();
  }
  EmitInternal("chromeless.webrtc.ice.failed", std::move(attrs));
}

void CbWebrtcEventEmitter::EmitIceFailed(
    std::string trigger_state,
    std::optional<std::function<std::string()>> current_state_probe,
    bool debounce_now) {
  std::function<std::string()> probe =
      current_state_probe.has_value() ? *current_state_probe
                                      : std::function<std::string()>();

  if (debounce_now) {
    FireIceFailed(std::move(trigger_state), std::move(probe));
    return;
  }

  scoped_refptr<base::SequencedTaskRunner> runner;
  base::OnceClosure cancellable;
  {
    base::AutoLock lock(mu_);
    if (ice_failed_debounce_) {
      // Already debouncing — drop the duplicate trigger. JS shape:
      // "if (iceFailedDebounce === null) … setTimeout(…)".
      return;
    }
    runner = debounce_task_runner_;
    if (runner) {
      // Unretained is safe: the M3 host MUST outlive any pending
      // debounce timer (lifetime contract documented on the class).
      // Cancellation via CancelableOnceClosure::Cancel() nulls the
      // bound closure, so a still-armed timer firing after we cancel
      // is a no-op rather than a use-after-free.
      ice_failed_debounce_ = std::make_unique<base::CancelableOnceClosure>(
          base::BindOnce(&CbWebrtcEventEmitter::FireIceFailed,
                         base::Unretained(this),
                         std::move(trigger_state),
                         std::move(probe)));
      // Take the bound closure while still under the lock; subsequent
      // EmitIceConnected() calls can Cancel() it concurrently and
      // that's fine — the cancellation just nulls the wrapped target.
      cancellable = ice_failed_debounce_->callback();
    }
  }
  if (!runner) {
    // No runner wired yet (caller forgot SetDebounceTaskRunner in
    // the M3 host). Fall back to immediate fire so we don't lose
    // the signal; log so the wiring gap is visible.
    LOG(WARNING) << "CbWebrtcEventEmitter: ice.failed without "
                    "debounce_task_runner — firing immediately";
    FireIceFailed(std::move(trigger_state), std::move(probe));
    return;
  }
  runner->PostDelayedTask(FROM_HERE, std::move(cancellable),
                          base::Seconds(3));
}

void CbWebrtcEventEmitter::EmitDcOpened(std::string label) {
  base::Value::Dict attrs;
  {
    base::AutoLock lock(mu_);
    attrs.Set("label", std::move(label));
    attrs.Set("session_id", session_id_);
    if (!first_dc_opened_seen_) {
      first_dc_opened_seen_ = true;
      // Prefer measuring from session_started_at_ when present; the
      // JS shape falls back to a page-relative measure when the DC
      // open beats the PC connectionState=connected. We don't have
      // a separate "dc_started_at" wall clock here, so when
      // session_started_at_ is unset we report handshake_ms = 0
      // (an honest "we don't know") rather than fabricate a base.
      if (session_started_at_) {
        const auto delta = base::TimeTicks::Now() - *session_started_at_;
        attrs.Set("handshake_ms",
                  static_cast<int>(std::max<int64_t>(
                      0, delta.InMilliseconds())));
      } else {
        attrs.Set("handshake_ms", 0);
      }
    }
  }
  EmitInternal("chromeless.webrtc.dc.opened", std::move(attrs));
}

void CbWebrtcEventEmitter::SetDebounceTaskRunner(
    scoped_refptr<base::SequencedTaskRunner> runner) {
  base::AutoLock lock(mu_);
  debounce_task_runner_ = std::move(runner);
}

void CbWebrtcEventEmitter::MintNewSession() {
  base::AutoLock lock(mu_);
  session_id_ = MintSessionId();
  session_started_at_.reset();
  session_created_ = false;
  first_dc_opened_seen_ = false;
  if (ice_failed_debounce_) {
    ice_failed_debounce_->Cancel();
    ice_failed_debounce_.reset();
  }
}

std::string CbWebrtcEventEmitter::session_id_for_testing() const {
  base::AutoLock lock(mu_);
  return session_id_;
}

void CbWebrtcEventEmitter::EmitInternal(const std::string& event,
                                        base::Value::Dict attrs) {
  if (client_->disabled()) return;

  base::Value::Dict envelope;
  envelope.Set("event", event);
  envelope.Set("attrs", std::move(attrs));
  std::string body;
  if (!base::JSONWriter::Write(base::Value(std::move(envelope)), &body)) {
    LOG(WARNING) << "CbWebrtcEventEmitter: JSONWriter::Write failed for "
                 << event;
    return;
  }
  client_->PostFireAndForget(std::move(body), "application/json");
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

std::string ResolveStatsSidecarUrl() {
  return EnvOrDefault("CHROMELESS_METRICS_URL", kDefaultStatsSidecarUrl);
}

std::string ResolveWebrtcEventUrl() {
  return EnvOrDefault("CHROMELESS_WEBRTC_METRICS_URL",
                      kDefaultWebrtcEventUrl);
}

}  // namespace cloud_browser

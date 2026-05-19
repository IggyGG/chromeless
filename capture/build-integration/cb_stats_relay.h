// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbStatsRelay + CbWebrtcEventEmitter — browser-process replacement for
// streamer.js's StatsRelay and MetricsEmitter classes (Wave 2 A4,
// chromeless commit 9850264). After M7 deletes the streamer page, the
// chromeless-metrics-sidecar (kept per CV2 M6 disposition) still needs
// client-side stats + lifecycle beacons; these classes plumb them from
// the native peer.
//
// Two contracts, kept separate because they have different lifetimes:
//
//   CbStatsRelay
//     webrtc::DataChannelObserver attached to the "stats" DataChannel
//     hosted by M3. On each text frame, POSTs the raw body verbatim to
//     `http://localhost:9100/stats-update` (override via constructor
//     argument). Burst-rate-limited error log on consecutive failures,
//     mirrors the JS shape. Per Plane CV2-33: "body = raw frame,
//     fire-and-forget, burst-rate-limited log".
//
//   CbWebrtcEventEmitter
//     Direct-emit helper for the five lifecycle beacons the streamer
//     fires today: session_created, session_closed, ice.connected,
//     ice.failed, dc.opened (with handshake_ms on the FIRST dc.opened
//     only). Owned by the M3 PCF host; the host's state-change
//     callbacks call Emit*() at the right moments. POSTs single
//     {event, attrs} JSON envelopes to `http://localhost:9100/
//     webrtc-event`.
//
// Both share an internal CbMetricsSidecarClient that wraps the actual
// HTTP POST via network::SimpleURLLoader. The client owns a
// scoped_refptr<network::SharedURLLoaderFactory> handed in at
// construction time (M3 wiring provides it from
// content::StoragePartition::GetURLLoaderFactoryForBrowserProcess).
//
// Env equivalents for the JS `?metrics=off` / `?webrtc_metrics=off`
// gates: pass the literal string "off" as the URL, or pre-empty the
// URL with the env-var lookup helpers UrlOrOffFromEnv("...") /
// WebrtcUrlOrOffFromEnv("..."). The relay logs one INFO line on
// construction recording the chosen URL or the disabled state, then
// stays silent during steady-state.
//
// Threading
//
// CbStatsRelay::OnMessage fires on the libwebrtc signaling/network
// thread. CbWebrtcEventEmitter's Emit* methods are designed to be
// called from the M3 host's natural thread (UI for the PCF callbacks,
// signaling for DC `open` events). Both POST helpers internally
// PostTask onto `http_task_runner_` — supplied by the embedder — so
// network::SimpleURLLoader is created and destroyed on the same
// sequence regardless of which thread emitted the event.
//
// R1 scope (CV2-33):
//   * forward stats DC text frames to /stats-update
//   * emit session_created (first PC connectionState=connected, latched)
//   * emit session_closed (paired with session_created)
//   * emit ice.connected (iceConnectionState ∈ {connected, completed})
//   * emit ice.failed (debounced ~3s on {failed, disconnected})
//   * emit dc.opened per DC label, with handshake_ms on the first one
//   * `off` sentinel disables emit entirely (test-friendly)
//   * burst-rate-limited log on failures (1st + every 10th)
//
// Non-goals for R1:
//   * scraping libwebrtc RTCStatsCollector — that's "server-side
//     stats", explicitly listed under non-goals in CV2-33. The stats
//     relay forwards the CLIENT's stats frame; the server-side
//     equivalent is a future R.
//   * per-channel semantic counters (cursor.coalesced,
//     clipboard.stale_seq, file_upload.orphan_timeout, …) — those
//     live with the M6 R2/R3 per-channel relays.
//   * OTLP exporter — sidecar already exports OTLP-logs (FU #28).

#ifndef CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_STATS_RELAY_H_
#define CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_STATS_RELAY_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "base/cancelable_callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "third_party/webrtc/api/data_channel_interface.h"
#include "third_party/webrtc/api/scoped_refptr.h"

namespace cloud_browser {

// Default endpoints. Mirror the JS constants in streamer.js lines
// 57 + 68. Loopback because the metrics-sidecar runs in the same
// supervisord-managed container as the chromium binary.
inline constexpr char kDefaultStatsSidecarUrl[] =
    "http://localhost:9100/stats-update";
inline constexpr char kDefaultWebrtcEventUrl[] =
    "http://localhost:9100/webrtc-event";

// Sentinel matching streamer.js's "?metrics=off" gate. When the URL
// equals this value (case-insensitive), the relay constructs in
// disabled state and emits no HTTP traffic. Tests use this to keep
// the chromium pod's log from filling with connect-refused warnings
// when no sidecar is bound to :9100.
inline constexpr char kSentinelOff[] = "off";

// CbMetricsSidecarClient — small adapter that turns a (url, body)
// pair into a fire-and-forget HTTP POST against the metrics-sidecar.
// Both CbStatsRelay and CbWebrtcEventEmitter own one; the client is
// the only place that touches network::SimpleURLLoader so the two
// observers stay decoupled from the loader machinery.
//
// Lifetime: created on the http_task_runner sequence, lives for the
// life of the owning relay/emitter, and reaps SimpleURLLoaders as
// their callbacks fire. PostUrlEncoded* may be called from any
// thread; it PostTask's onto http_task_runner_ before touching the
// loader factory.
class CbMetricsSidecarClient {
 public:
  CbMetricsSidecarClient(
      std::string label,                       // "stats"|"webrtc-event"
      std::string url,                         // or "off" sentinel
      scoped_refptr<network::SharedURLLoaderFactory> loader_factory,
      scoped_refptr<base::SequencedTaskRunner> http_task_runner);

  CbMetricsSidecarClient(const CbMetricsSidecarClient&) = delete;
  CbMetricsSidecarClient& operator=(const CbMetricsSidecarClient&) = delete;

  ~CbMetricsSidecarClient();

  // True when the URL is the "off" sentinel or empty — callers can
  // short-circuit body construction in that case.
  bool disabled() const { return disabled_; }

  // Fire-and-forget POST. `body` is moved in. Safe to call from any
  // thread; the actual SimpleURLLoader is constructed on
  // http_task_runner_. R1 doesn't await the response — the next stats
  // frame is at most 1 s away and a missed lifecycle beacon is a
  // single dashboard tick.
  //
  // `content_type` defaults to application/json; pass an explicit
  // value for non-JSON bodies (R2/R3 may carry binary).
  void PostFireAndForget(std::string body,
                         std::string content_type = "application/json");

 private:
  // Per-loader trampoline. SimpleURLLoader's callback signature wants
  // a (response_body) string; we drop it and use the result code
  // only. `loader_id` lets us reap the unique_ptr by id from the
  // in-flight map.
  void OnLoadComplete(int64_t loader_id,
                      std::unique_ptr<std::string> response_body);

  // Body of PostFireAndForget after the PostTask hop onto
  // http_task_runner_. Must run on http_task_runner_'s sequence.
  void PostOnHttpSequence(std::string body, std::string content_type);

  // Rate-limited log helper. We log on the first failure of a burst
  // and on every 10th thereafter; on recovery (next success after a
  // burst) we log once. Matches the JS StatsRelay shape so the
  // operator-visible log volume is unchanged across the M7 cutover.
  void RecordSuccess();
  void RecordFailure(int net_error_or_http_status);

  const std::string label_;       // "stats"|"webrtc-event" for logs
  const std::string url_;
  const bool disabled_;
  const scoped_refptr<network::SharedURLLoaderFactory> loader_factory_;
  const scoped_refptr<base::SequencedTaskRunner> http_task_runner_;

  // Loader bookkeeping — all touched only on http_task_runner_.
  int64_t next_loader_id_ = 0;
  std::map<int64_t, std::unique_ptr<network::SimpleURLLoader>> in_flight_;

  // Burst counters — all touched only on http_task_runner_.
  int64_t consecutive_failures_ = 0;
  int64_t success_count_ = 0;

  base::WeakPtrFactory<CbMetricsSidecarClient> weak_factory_{this};
};

// CbStatsRelay — webrtc::DataChannelObserver attached to the "stats"
// DC. On each text message, forwards the raw body verbatim to the
// /stats-update endpoint. Mirrors streamer.js StatsRelay (lines
// 391-449 of capture/streamer-page/streamer.js as of commit 9850264).
//
// Ownership: the M3 PCF host constructs this and calls
// dc->RegisterObserver(this). The DC keeps a raw pointer back via
// RegisterObserver/UnregisterObserver; callers MUST UnregisterObserver
// before destroying.
class CbStatsRelay : public webrtc::DataChannelObserver {
 public:
  CbStatsRelay(std::unique_ptr<CbMetricsSidecarClient> client);

  CbStatsRelay(const CbStatsRelay&) = delete;
  CbStatsRelay& operator=(const CbStatsRelay&) = delete;

  ~CbStatsRelay() override;

  // webrtc::DataChannelObserver — invoked on libwebrtc signaling/
  // network thread.
  void OnMessage(const webrtc::DataBuffer& buffer) override;
  void OnStateChange() override;
  void OnBufferedAmountChange(uint64_t sent_data_size) override;
  bool IsOkToCallOnTheNetworkThread() override;

 private:
  const std::unique_ptr<CbMetricsSidecarClient> client_;
};

// CbWebrtcEventEmitter — five Emit*() methods, one per lifecycle
// beacon the streamer page emits today. Owned by the M3 PCF host;
// the host's PeerConnectionObserver / DataChannelObserver callbacks
// call into us at the appropriate state transitions.
//
// Per-session state (session_id, session_started_at, first_dc_opened,
// ice_failed_debounce) lives in this object so a future page-bootstrap
// reattach mints a fresh session pair when the host calls
// MintNewSession() between teardown and re-bootstrap. The
// session-created → session-closed pair is guaranteed even when
// teardown takes an exceptional path (assert in tests).
//
// Threading: Emit* may be called from any thread; everything except
// the HTTP POST runs on the calling thread. The atomic-flag-style
// state (one bool, one Time, one int) is protected by `mu_` because
// the M3 host invokes us from both signaling (DC open) and UI (PC
// connectionStateChange) threads.
class CbWebrtcEventEmitter {
 public:
  CbWebrtcEventEmitter(std::unique_ptr<CbMetricsSidecarClient> client);

  CbWebrtcEventEmitter(const CbWebrtcEventEmitter&) = delete;
  CbWebrtcEventEmitter& operator=(const CbWebrtcEventEmitter&) = delete;

  ~CbWebrtcEventEmitter();

  // Caller-supplied signaling session id — equivalent to streamer.js's
  // SESSION_ID query param (set once at bootstrap, propagated through
  // every event's attrs). Empty string omits the attr.
  void SetSignalingSessionId(std::string id);

  // session_created — emitted on first PC connectionState=connected.
  // Latched: re-entry is a no-op until MintNewSession() is called.
  // `reason` is logged in attrs (typically "connectionState=connected"
  // or "fallback"); kept as an opaque string so the host doesn't need
  // a per-cause enum.
  void EmitSessionCreated(std::string reason);

  // session_closed — paired with session_created. Computes
  // duration_ms from session_started_at_. Latched: a second close is
  // a no-op (the JS shape exposes this via `active.emitSessionClosed`
  // so teardown paths can fire idempotently).
  void EmitSessionClosed(std::string reason);

  // ice.connected — emitted when iceConnectionState transitions to
  // connected or completed. Cancels any pending ice.failed debounce.
  void EmitIceConnected(std::string ice_state);

  // ice.failed — debounced ~3 s on iceConnectionState ∈
  // {failed, disconnected}. The debounce reads the current state on
  // fire and skips the emit if ICE has recovered. The reason carried
  // in attrs is the *triggering* state, not the final.
  //
  // `debounce_now` lets tests fire synchronously; production callers
  // omit it (default false → use the natural 3s timer on
  // ice_debounce_task_runner_).
  void EmitIceFailed(std::string trigger_state,
                     std::optional<std::function<std::string()>>
                         current_state_probe,
                     bool debounce_now = false);

  // dc.opened — per-DC `open` event. The FIRST dc.opened in a session
  // carries handshake_ms (Now() - session_started_at_); subsequent
  // ones omit it. Caller passes the DC label
  // ("input"|"stats"|"cursor"|"clipboard"|"files").
  void EmitDcOpened(std::string label);

  // Mint a fresh session_id + clear all per-session latches. Called
  // by the M3 host between teardown and re-bootstrap so the next
  // session_created → session_closed pair is independent.
  void MintNewSession();

  // Caller-supplied task runner used to fire the 3 s ice.failed
  // debounce. Must be set before any EmitIceFailed() invocation in
  // production; tests can either set this to a TestMockTimeTaskRunner
  // or pass debounce_now=true to skip the timer entirely. M3 host
  // wires content::GetUIThreadTaskRunner({}) here.
  void SetDebounceTaskRunner(
      scoped_refptr<base::SequencedTaskRunner> runner);

  // Accessor for tests — returns the session_id currently in use.
  std::string session_id_for_testing() const;

 private:
  // Build the {event, attrs} envelope and hand it to client_.
  // Always callable from any thread; client_ handles the hop.
  void EmitInternal(const std::string& event,
                    base::DictValue attrs);

  // Real fire path for ice.failed — invoked synchronously when the
  // debounce timer expires, or directly when debounce_now=true /
  // debounce_task_runner_ is unset. Touches mu_ internally; callers
  // must NOT hold mu_ across the call.
  void FireIceFailed(std::string trigger_state,
                     std::function<std::string()> current_state_probe);

  // Per-session state. Protected by mu_.
  mutable base::Lock mu_;
  std::string session_id_;
  std::string signaling_session_id_;
  std::optional<base::TimeTicks> session_started_at_;
  bool session_created_ = false;
  bool first_dc_opened_seen_ = false;
  // Cancellable debounce for ice.failed. Owned by us so a recovery
  // can cancel before the timer fires.
  std::unique_ptr<base::CancelableOnceClosure> ice_failed_debounce_;

  // HTTP client — shared with all Emit*() methods.
  const std::unique_ptr<CbMetricsSidecarClient> client_;

  // Task runner for ice.failed debounce. Caller-supplied via
  // SetDebounceTaskRunner (typically a SequencedTaskRunner on the
  // UI thread). When unset at the time EmitIceFailed() is called we
  // fall back to a synchronous fire with a WARNING — see
  // EmitIceFailed for the rationale.
  //
  // Lifetime contract: the M3 host MUST destroy this emitter only
  // after every posted debounce task has either fired or been
  // cancelled. The class uses base::Unretained for the bound
  // FireIceFailed callback (matches the M4 R1 dispatcher's pattern).
  scoped_refptr<base::SequencedTaskRunner> debounce_task_runner_;
};

// ---------------------------------------------------------------------
// Env-var helpers.
// ---------------------------------------------------------------------

// Returns the value of CHROMELESS_METRICS_URL if set (with the "off"
// sentinel forwarded unchanged), otherwise kDefaultStatsSidecarUrl.
// Lives here so the M3 wiring code doesn't need to know the env-var
// name shape.
std::string ResolveStatsSidecarUrl();

// Returns the value of CHROMELESS_WEBRTC_METRICS_URL if set (with
// the "off" sentinel forwarded unchanged), otherwise
// kDefaultWebrtcEventUrl.
std::string ResolveWebrtcEventUrl();

}  // namespace cloud_browser

#endif  // CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_STATS_RELAY_H_

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbClipboardRelay — browser-process replacement for streamer.js's
// clipboard hookup (capture/streamer-page/streamer.js, deleted in M7).
// Bidirectional bridge between the WebRTC "clipboard" DataChannel and
// the existing clipboard-bridge sidecar (capture/clipboard-bridge/),
// which owns the CDP-driven OS-clipboard write + the in-page copy
// probe.
//
// # Wire model (preserved across the cutover)
//
// The wire envelope on BOTH the DC and the bridge WS is the v1
// clipboard_offer shape documented in docs/protocols/clipboard-
// channel.md and parsed by clipboard-bridge/main.go:
//
//     {
//       "v": 1,
//       "type": "clipboard_offer",
//       "t": <unix-ms>,
//       "seq": <monotonic int>,
//       "data": {
//         "direction": "client->cloud" | "cloud->client",
//         "source":    "user_action",
//         "text":      "<utf-8 string, ≤1 MiB>"
//       }
//     }
//
// The relay does NOT mint or rewrite this envelope — it shuttles the
// raw text body between the DC and the bridge byte-for-byte. The seq
// counter, timestamp, and direction are authored by whichever side
// originated the event (portal client on inbound; bridge's outbound
// probe on outbound). Re-stamping here would desync the seq stream
// against the portal-side dedup window.
//
// # Topology
//
//   ┌──────────────────────────┐                  ┌────────────────────────┐
//   │ cb-chromium browser proc │                  │ clipboard-bridge (Go)  │
//   │  ┌────────────────────┐  │  inbound (push)  │   --source=ws          │
//   │  │ CbClipboardRelay   │──┼─→ WS client ────→│  ws://127.0.0.1:9300/  │
//   │  │ DataChannelObserver│  │                  │  /clipboard            │
//   │  └────────────────────┘  │                  │                        │
//   │           ↑              │                  │                        │
//   │           │ DC msg       │                  │   --sink=ws            │
//   │  ┌────────┴───────────┐  │  outbound (pull) │  dials this server     │
//   │  │ Outbound listener  │←─┼── WS server ←────│  ws://127.0.0.1:9301/  │
//   │  │ (host->Send)       │  │                  │  /clipboard            │
//   │  └────────────────────┘  │                  │                        │
//   └──────────────────────────┘                  └────────────────────────┘
//
// Both endpoints loopback because the bridge runs in the same
// supervisord-managed container as the chromium binary. The default
// addresses match clipboard-bridge/main.go's --ws-addr 127.0.0.1:9300
// and --sink-url ws://127.0.0.1:9301/clipboard flag defaults.
//
// # Why two unidirectional WS connections, not one duplex
//
// Two reasons:
//   1. Zero bridge changes. The bridge's source (WS server) and sink
//      (WS client) shapes already match what we need; we just have to
//      stand up the complement on the chromium side — a client for
//      the source, a server for the sink. A duplex variant would need
//      either a third mode in the bridge or a different framing
//      contract; neither pays for itself.
//   2. Clean failure isolation. Inbound failures (bridge sidecar
//      unreachable / OS-clipboard write failed) MUST NOT cascade into
//      outbound delivery and vice versa. Separate sockets give us
//      independent reconnect timers + independent burst-rate-limited
//      log lines.
//
// # Echo suppression
//
// Lives in the BRIDGE (main.go's `b.last` field), not here. When the
// bridge writes `text` to the cloud clipboard via CDP, it stashes
// `text` and drops the next outbound that matches. The relay forwards
// envelopes verbatim and trusts the bridge's suppression. This avoids
// a relay-side memory of "what did we just push" (which would race
// the bridge's CDP timing) and keeps the relay stateless aside from
// the WS connections.
//
// # Threading
//
// CbClipboardRelay's webrtc::DataChannelObserver methods fire on the
// libwebrtc signaling/network thread (same as CbStatsRelay). Body
// construction is cheap (no parse, no mint) so we PostTask onto the
// io_task_runner_ where the inbound WS client lives, no hop in the
// hot path beyond that.
//
// CbClipboardOutboundReceiver's WS frames arrive on the io_task_-
// runner_ sequence; the relay PostTasks onto signaling_task_runner_
// before invoking dc_host_->Send(kClipboard, text) because
// DataChannelInterface::Send is signaling-only.
//
// # R2 scope (CV2-34)
//
//   * inbound: forward "clipboard" DC text frames to the bridge's WS
//     source endpoint (default ws://127.0.0.1:9300/clipboard)
//   * outbound: accept WS connections from the bridge on the sink
//     endpoint (default 127.0.0.1:9301 path /clipboard) and forward
//     each text frame via dc_host_->Send(kClipboard, ...)
//   * burst-rate-limited log on inbound POST failures (same 1st /
//     every-10th cadence as CbStatsRelay; see ShouldLogFailure)
//   * `off` sentinel disables the relay entirely (test-friendly,
//     matches CbStatsRelay's kSentinelOff)
//
// # Non-goals for R2
//
//   * parsing the clipboard_offer envelope — that's the bridge's job
//     (validation, version gate, source gate, 1 MiB cap). The relay
//     forwards bytes; an oversized DC frame is dropped with a WARN
//     before the WS hop only as defence-in-depth.
//   * mutating direction. The relay never rewrites `direction` —
//     client->cloud envelopes flow inbound only, cloud->client flow
//     outbound only. Wrong-direction envelopes are forwarded
//     verbatim and dropped by the receiving side per the v1 contract.
//   * synthesising clipboard_offer envelopes from non-DC sources
//     (e.g. host paste-into-cb-chromium). The bridge's outbound
//     probe is the sole authority on cloud→client events.
//   * reconnect storm protection. We reconnect with the same
//     exponential-backoff cadence the rest of the M6 family uses
//     (initial 1s, doubles to a 30s cap). A wedged bridge or wedged
//     DC each independently surface in metrics — no relay-side
//     circuit-breaker.

#ifndef CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_CLIPBOARD_RELAY_H_
#define CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_CLIPBOARD_RELAY_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "cloud-browser/capture/signaling/cb_dc_host.h"
#include "third_party/webrtc/api/data_channel_interface.h"
#include "third_party/webrtc/api/scoped_refptr.h"

namespace cloud_browser {

// Default endpoints. Mirror clipboard-bridge/main.go's flag defaults:
//   --ws-addr 127.0.0.1:9300  (bridge's source — relay dials this)
//   --sink-url ws://127.0.0.1:9301/clipboard (bridge's sink — relay
//                                             accepts here)
// Loopback because the bridge runs in the same container as cb-
// chromium under supervisord.
inline constexpr char kDefaultBridgeInboundUrl[] =
    "ws://127.0.0.1:9300/clipboard";
inline constexpr char kDefaultRelayOutboundAddr[] = "127.0.0.1:9301";
inline constexpr char kDefaultRelayOutboundPath[] = "/clipboard";

// "off" sentinel matching CbStatsRelay's kSentinelOff. When either
// URL/addr equals this value (case-insensitive), the corresponding
// half of the relay constructs in disabled state and emits no
// traffic. Lets tests run the M3 host stack without dragging in a
// real bridge sidecar.
inline constexpr char kClipboardSentinelOff[] = "off";

// CbClipboardBridgeWsClient — small adapter that wraps the actual
// WebSocket client used to push inbound envelopes at the bridge's
// source endpoint. Parallel to CbMetricsSidecarClient in M6 R1, but
// over WS instead of HTTP because the bridge already speaks WS.
//
// Lifetime: created on the io_task_runner sequence, lives for the
// life of CbClipboardRelay. Reconnects opaquely on transport loss
// with exponential backoff. PostText() may be called from any thread;
// it PostTasks onto io_task_runner_ before touching the WS handle.
//
// # WS-backend choice (production wiring is a follow-up)
//
// Chromium browser process exposes WebSocket clients through two
// surfaces: services/network's mojo-fronted WebSocket (the Web API
// surface) and net/websockets/WebSocketChannel (the raw-net surface
// used by the network service itself). Both work for loopback; the
// network-service surface is heavier wiring (mojo pipes, profile-
// scoped URLLoaderFactory) but matches the pattern CbStatsRelay
// already uses. The raw-net surface is simpler but bypasses the
// network-traffic-annotation chain.
//
// TODO(M6-R2-ws-backend): pick one and wire. R2 draft leaves this
// behind an interface so the rest of the relay can land + be
// integration-tested with a fake WS client before the production
// choice is locked in. The integration-test fake covered by the
// follow-up cb_clipboard_relay_test.cc target uses an in-process
// fake satisfying the same WsTransport interface.
class CbClipboardBridgeWsClient {
 public:
  CbClipboardBridgeWsClient(
      std::string label,                                   // "inbound"
      std::string url,                                     // or "off"
      scoped_refptr<base::SequencedTaskRunner> io_task_runner);

  CbClipboardBridgeWsClient(const CbClipboardBridgeWsClient&) = delete;
  CbClipboardBridgeWsClient& operator=(const CbClipboardBridgeWsClient&) =
      delete;

  virtual ~CbClipboardBridgeWsClient();

  // True when the URL is the "off" sentinel or empty — callers can
  // short-circuit body construction.
  bool disabled() const { return disabled_; }

  // Send a single text frame to the bridge. Fire-and-forget; the
  // relay does not await a response (the bridge's HTTP/WS source
  // does not produce a per-frame reply). If the WS is not currently
  // connected, the frame is dropped and a failure is recorded; the
  // relay does NOT queue across reconnects because the bridge expects
  // monotone-seq envelopes from the portal client and a queued
  // backlog can reorder relative to fresh frames the portal is also
  // sending. JS shape matches: streamer.js dropped on disconnect too.
  //
  // Marked virtual so cb_clipboard_relay_test.cc can subclass with a
  // capture sink. Production callers always invoke through the base
  // pointer.
  virtual void PostText(std::string frame);

 protected:
  // Body of PostText after the io_task_runner hop. Exposed to the
  // test seam — production callers go through PostText.
  void PostOnIoSequence(std::string frame);

  // Connection lifecycle hooks — production impl wires WS open /
  // close / message; test fake leaves them no-op and synthesises
  // OnFrameSent directly. TODO(M6-R2-ws-backend) lands the bodies.
  virtual void EnsureConnected();    // idempotent connect
  virtual void OnTransportConnected();
  virtual void OnTransportDisconnected(int net_error_or_status);
  virtual void OnFrameSent(bool ok, int net_error_or_status);

  // Rate-limited log helper. Same cadence as CbStatsRelay's
  // ShouldLogFailure (1st of a burst, every 10th, then every 100th).
  void RecordSuccess();
  void RecordFailure(int net_error_or_status);

  const std::string label_;
  const std::string url_;
  const bool disabled_;
  const scoped_refptr<base::SequencedTaskRunner> io_task_runner_;

  // Burst counters — all touched only on io_task_runner_.
  int64_t consecutive_failures_ = 0;
  int64_t success_count_ = 0;

  // Reconnect backoff state — all touched only on io_task_runner_.
  // Initial 1 s, doubles up to 30 s cap, resets on connect success.
  // Matches the cadence the rest of M6 uses.
  base::TimeDelta reconnect_backoff_ = base::Seconds(1);
  bool reconnect_pending_ = false;

  base::WeakPtrFactory<CbClipboardBridgeWsClient> weak_factory_{this};
};

// CbClipboardRelay — webrtc::DataChannelObserver attached to the
// "clipboard" DC. On each inbound text message, forwards the raw
// body to the bridge WS source endpoint via CbClipboardBridgeWsClient.
//
// Ownership: the M3 PCF host constructs this and calls
// dc_host_->BindObserver(CbDcLabel::kClipboard, this). The host's
// per-channel trampoline keeps a raw pointer back via BindObserver;
// callers MUST BindObserver(kClipboard, nullptr) (or destroy the
// host) before destroying this relay.
class CbClipboardRelay : public webrtc::DataChannelObserver {
 public:
  CbClipboardRelay(std::unique_ptr<CbClipboardBridgeWsClient> client);

  CbClipboardRelay(const CbClipboardRelay&) = delete;
  CbClipboardRelay& operator=(const CbClipboardRelay&) = delete;

  ~CbClipboardRelay() override;

  // webrtc::DataChannelObserver — invoked on libwebrtc signaling/
  // network thread.
  void OnMessage(const webrtc::DataBuffer& buffer) override;
  void OnStateChange() override;
  void OnBufferedAmountChange(uint64_t sent_data_size) override;
  bool IsOkToCallOnTheNetworkThread() override;

 private:
  const std::unique_ptr<CbClipboardBridgeWsClient> client_;
};

// CbClipboardOutboundServer — small WS server that the bridge's sink
// dials into. For every text frame the bridge emits, forwards via
// dc_host_->Send(CbDcLabel::kClipboard, text).
//
// Lifetime: caller (the M3 host wiring) constructs and owns. The
// constructor binds + listens; Shutdown() (or dtor) tears the listen
// socket + any active connection down. The bridge re-dials as part
// of its own reconnect loop; the server tolerates churn.
//
// # Why a server-side trampoline instead of a libwebrtc inbound
// channel
//
// The DC's "clipboard" inbound direction is already in use by
// portal-client→cb-chromium (handled by CbClipboardRelay above). The
// bridge's outbound stream is a *separate* path: it originates from
// in-chromium copy events that the bridge observes via its CDP probe,
// not from DC traffic. So we need a second sink that the bridge can
// dump those into, and the cleanest shape is a tiny WS server that
// re-injects them into the outbound DC direction.
//
// TODO(M6-R2-ws-server): like CbClipboardBridgeWsClient, the server
// is left behind an interface (CbClipboardOutboundTransport) so the
// rest of the relay lands testably. The production server is a thin
// wrapper over net/server/http_server.h with WebSocket upgrade — that
// is the same dep chromium uses for its DevTools WS server, so the
// lifecycle + threading patterns transfer directly.
class CbClipboardOutboundServer {
 public:
  // |dc_host|: M3 R5's CbDataChannelHost. Must outlive this server.
  //     Outbound frames are forwarded via dc_host->Send(kClipboard,
  //     text); the host handles the signaling-thread hop internally.
  // |listen_addr|: "host:port" the bridge's sink dials into (default
  //     127.0.0.1:9301). The "off" sentinel disables — the server
  //     never binds, and incoming frames (since there's no way for
  //     them to arrive) are trivially dropped.
  // |listen_path|: WS upgrade path (default /clipboard). The bridge
  //     dials ws://<addr><path>; mismatched path returns 404.
  // |io_task_runner|: the task runner the listen socket + accepted
  //     connection run on. Typically the browser's network IO
  //     thread; in unit tests, a TestSimpleTaskRunner.
  CbClipboardOutboundServer(
      signaling::CbDataChannelHost* dc_host,
      std::string listen_addr,
      std::string listen_path,
      scoped_refptr<base::SequencedTaskRunner> io_task_runner);

  CbClipboardOutboundServer(const CbClipboardOutboundServer&) = delete;
  CbClipboardOutboundServer& operator=(const CbClipboardOutboundServer&) =
      delete;

  virtual ~CbClipboardOutboundServer();

  // True iff the listen address is the "off" sentinel or empty.
  bool disabled() const { return disabled_; }

  // Bind + listen. Idempotent; returns true on a fresh bind or when
  // already listening, false on bind error. Disabled state returns
  // true (the no-op path is considered successful — tests assert
  // disabled() && Start() rather than racing a port).
  //
  // Thread: caller's thread; internally posts onto io_task_runner_.
  // Blocks on the post in production but a test seam can override.
  virtual bool Start();

  // Stop accepting + drop any active connection. Idempotent. Called
  // by the dtor; the M3 host calls this explicitly during shutdown
  // to ensure the listen socket releases before the host's PC
  // teardown progresses.
  virtual void Shutdown();

 protected:
  // OnFrameReceived — invoked on io_task_runner_ when the bridge's
  // sink connection emits a text frame. Default impl validates +
  // forwards to dc_host_->Send(kClipboard, text). Subclasses can
  // override for test capture; production goes through the base.
  virtual void OnFrameReceived(std::string frame);

  // Forward the validated frame to the DC. PostTasks onto the host's
  // signaling runner via host's own Send() (host handles that hop).
  void ForwardToDc(std::string text);

  // Per-frame guard. R2 enforces the 1 MiB cap as defence-in-depth
  // even though the bridge already does — a misconfigured bridge
  // shouldn't be able to wedge the relay with a giant frame.
  bool FrameWithinCap(const std::string& frame) const;

  // raw_ptr because dc_host_ is caller-owned and must outlive us.
  // CV2-75 fix-forward: wrapped in raw_ptr<T> wrapper per chromium-
  // rawptr lint (author comment already declared raw_ptr intent).
  const raw_ptr<signaling::CbDataChannelHost> dc_host_;
  const std::string listen_addr_;
  const std::string listen_path_;
  const bool disabled_;
  const scoped_refptr<base::SequencedTaskRunner> io_task_runner_;

  // Burst counters — touched only on io_task_runner_.
  int64_t frames_received_ = 0;
  int64_t frames_dropped_oversize_ = 0;
  int64_t consecutive_send_failures_ = 0;

  // Listen state — touched only on io_task_runner_.
  bool listening_ = false;

  base::WeakPtrFactory<CbClipboardOutboundServer> weak_factory_{this};
};

// ---------------------------------------------------------------------
// Env-var helpers — match the M6 R1 ResolveStatsSidecarUrl shape.
// ---------------------------------------------------------------------

// Returns the value of CHROMELESS_CLIPBOARD_INBOUND_URL if set (with
// the "off" sentinel forwarded unchanged), otherwise the default
// inbound URL. Lives here so the M3 wiring code doesn't need to
// know the env-var name shape.
std::string ResolveClipboardInboundUrl();

// Returns the value of CHROMELESS_CLIPBOARD_OUTBOUND_ADDR if set,
// otherwise kDefaultRelayOutboundAddr.
std::string ResolveClipboardOutboundAddr();

// Returns the value of CHROMELESS_CLIPBOARD_OUTBOUND_PATH if set,
// otherwise kDefaultRelayOutboundPath.
std::string ResolveClipboardOutboundPath();

}  // namespace cloud_browser

#endif  // CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_CLIPBOARD_RELAY_H_

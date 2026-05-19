// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbFileUploadRelay — browser-process replacement for streamer.js's
// file-upload hookup (Wave 1 of the file-upload pipeline; the JS
// relay in capture/streamer-page/streamer.js, deleted in M7).
// Bidirectional bridge between the WebRTC "files" DataChannel and
// the existing file-bridge sidecar (capture/file-bridge/), which
// owns the SHA-256 verification, MIME re-sniff, on-disk staging,
// and CDP-driven DOM.setFileInputFiles attach.
//
// # Wire model (preserved across the cutover)
//
// The wire envelope on BOTH the DC and the bridge WS is the v1
// file_upload_* shape documented in docs/protocols/file-upload.md
// and parsed by file-bridge/main.go. Each envelope is one line of
// UTF-8 JSON over the WebSocket. Client → server traffic carries:
//
//     file_upload_start  { upload_id, name, mime_type, size, sha256,
//                          target_selector? }
//     file_upload_chunk  { upload_id, seq, data: <base64> }
//     file_upload_end    { upload_id }
//     file_upload_cancel { upload_id }
//
// Server → client traffic carries:
//
//     file_upload_progress  { upload_id, bytes_received, bytes_total }
//     file_upload_complete  { upload_id, server_path, attached_via }
//     file_upload_error     { upload_id, code, error }
//
// The relay does NOT mint, parse, or rewrite these envelopes — it
// shuttles raw text frames between the DC and the bridge WS byte-
// for-byte. The bridge owns all validation: protocol version, size
// cap (default 100 MiB), per-chunk cap (default 1 MiB raw), MIME
// allowlist, SHA-256 verification, path-traversal sanitisation. The
// relay enforces only one defence-in-depth cap (kMaxFileUploadFrame
// Bytes, ~1.5 MiB to accommodate the 1 MiB raw + base64 inflation
// + envelope overhead) to keep a wedged peer from pinning the IO
// thread on a giant body. Re-stamping or filtering here would
// duplicate the bridge's policy and inevitably drift.
//
// # Topology
//
//   ┌──────────────────────────┐                  ┌────────────────────────┐
//   │ cb-chromium browser proc │                  │ file-bridge (Go)       │
//   │  ┌────────────────────┐  │                  │   --ws-addr            │
//   │  │ CbFileUploadRelay  │  │                  │  127.0.0.1:9400        │
//   │  │ DataChannelObserver│──┼─→ WS client ────→│  /files                │
//   │  └────────────────────┘  │  client→bridge   │                        │
//   │           ↑              │   inbound text   │                        │
//   │           │ DC msg       │                  │  ↑ same WS connection  │
//   │  ┌────────┴───────────┐  │                  │  ↓ duplex             │
//   │  │ WS read-loop       │←─┼── WS server ←────│   bridge sends back    │
//   │  │ → dc_host->Send    │  │   outbound text  │   progress/complete/   │
//   │  └────────────────────┘  │                  │   error envelopes      │
//   └──────────────────────────┘                  └────────────────────────┘
//
// Loopback because file-bridge runs in the same supervisord-managed
// container as the chromium binary. The default address matches
// file-bridge/main.go's --ws-addr 127.0.0.1:9400 and --ws-path
// /files flag defaults.
//
// # Why one duplex WS (not two unidirectional like clipboard)
//
// The clipboard bridge (M6 R2) has a request/no-response source
// shape on one socket and a separate publish shape on another. The
// file-bridge protocol is fundamentally request/response: the client
// sends file_upload_start and expects file_upload_progress and
// file_upload_complete back, all correlated by upload_id. A single
// duplex WS preserves that correlation naturally — the bridge speaks
// only over one socket per session, and the relay observes a strict
// "this peer-bridge pair owns one upload_id space at a time".
//
// Forcing the clipboard's two-socket shape onto file-upload would
// require either:
//   (a) the bridge growing a separate emit socket per session, or
//   (b) the relay maintaining an upload_id → response-socket map
//       and routing replies through it.
//
// (a) would diverge the bridge from its documented one-server
// shape (file-bridge/main.go's r.HandleFunc(ws-path, ...) is a
// single endpoint that handles both directions of one upload
// lifecycle). (b) would couple the relay to the v1 wire shape —
// the relay would need to parse upload_id off every envelope to
// route replies — which is exactly the coupling we keep out of M6
// R1/R2 by treating the body as opaque bytes. So we follow the
// bridge's shape: one duplex WS, the relay forwards verbatim in
// both directions over the same socket.
//
// # Echo suppression
//
// Not needed. The file-upload protocol has no echo loop — client→
// server and server→client carry orthogonal envelope types
// (file_upload_chunk vs file_upload_progress, etc). A frame
// originating on the DC never re-appears on the WS server's read
// side, and vice versa. Unlike clipboard, where a single
// clipboard_offer envelope shape flows in both directions, here the
// types disambiguate direction without any state in the relay.
//
// # Threading
//
// CbFileUploadRelay's webrtc::DataChannelObserver methods fire on
// the libwebrtc signaling/network thread (same as CbClipboardRelay).
// Body construction is cheap (no parse, no mint) so we PostTask
// onto the io_task_runner_ where the WS client lives, no hop in
// the hot path beyond that.
//
// CbFileUploadBridgeWsClient's WS read frames arrive on the io_-
// task_runner_ sequence; the client PostTasks onto signaling_task_-
// runner_ before invoking dc_host_->Send(kFileUpload, text) because
// DataChannelInterface::Send is signaling-only.
//
// # Backpressure note
//
// docs/protocols/file-upload.md § Backpressure requires the
// CLIENT to throttle chunks against DC bufferedAmount. The relay
// does not need its own backpressure: the DC's SCTP send buffer
// is bounded by libwebrtc, and the WS hop is a localhost loopback.
// If a future R# observes WS-side queuing under sustained 100 MiB
// uploads we can add a high-water shed-and-error path; for the
// 1 MiB-raw-chunks-at-localhost shape we expect the bridge to drain
// at line rate.
//
// # R3 scope (CV2-35)
//
//   * inbound: forward "files" DC text frames to the bridge's WS
//     endpoint (default ws://127.0.0.1:9400/files)
//   * outbound: read the bridge's WS reply frames and forward each
//     via dc_host_->Send(kFileUpload, ...)
//   * defence-in-depth cap on per-frame size (kMaxFileUploadFrame
//     Bytes); oversize drop + WARN
//   * binary-frame drop on either direction (the v1 wire is JSON
//     text only; chunks carry base64 inside the JSON)
//   * burst-rate-limited log on transport failure (same 1st/every
//     10th cadence as CbStatsRelay; see ShouldLogFailure)
//   * `off` sentinel disables the relay entirely (test-friendly,
//     matches CbStatsRelay's kSentinelOff)
//   * reconnect with exponential backoff (1 s initial, doubles to
//     30 s cap; matches the rest of the M6 family)
//
// # Non-goals for R3
//
//   * parsing the file_upload_* envelopes — that's the bridge's
//     job (validation, version gate, size cap, MIME, SHA-256). The
//     relay forwards bytes; an oversized frame is dropped before
//     the WS hop only as defence-in-depth.
//   * upload_id state. The relay is stateless w.r.t. uploads — it
//     does not track which upload_ids are live or expect any
//     particular envelope ordering. Out-of-order or interleaved
//     uploads (the protocol permits up to 4 concurrent per channel)
//     pass through without relay-side correlation.
//   * synthesising progress/complete/error envelopes. If the bridge
//     is unreachable, inbound frames are dropped on the WS hop
//     with a rate-limited WARN; we do NOT mint a file_upload_error
//     reply. The client's existing inactivity-timeout (60 s per
//     docs/protocols/file-upload.md) covers the visible failure
//     mode. Minting fake replies would couple the relay to the
//     wire shape.
//   * per-upload metrics (orphan_timeout, chunk_too_large, etc.).
//     The bridge's own metrics path covers these end-to-end; the
//     relay only counts frame-level transport health (sends ok /
//     failed) for the M6 R1 stats lane to pick up if needed.
//   * concurrent-upload accounting. The relay sees one byte
//     stream; the bridge handles the upload_id demux. Concurrent
//     uploads (per docs/protocols/file-upload.md § Size and chunk
//     policy: 4 per channel default) are transparent to us.
//   * reconnect of the WS while an upload is mid-stream. If the WS
//     drops during an upload, in-flight chunks on the inbound side
//     are lost; the client's upload eventually times out. A
//     future R# may add upload_id-aware resumption, but v1 keeps
//     the relay stateless. Documented in the v1 known-gaps list
//     for file-bridge.

#ifndef CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_FILE_UPLOAD_RELAY_H_
#define CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_FILE_UPLOAD_RELAY_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "cloud-browser/capture/signaling/cb_dc_host.h"
#include "third_party/webrtc/api/data_channel_interface.h"
#include "third_party/webrtc/api/scoped_refptr.h"

namespace cloud_browser {

// Default endpoint. Mirrors file-bridge/main.go's flag defaults:
//   --ws-addr 127.0.0.1:9400
//   --ws-path /files
// Loopback because the bridge runs in the same container as cb-
// chromium under supervisord.
inline constexpr char kDefaultFileBridgeWsUrl[] =
    "ws://127.0.0.1:9400/files";

// "off" sentinel matching CbStatsRelay's kSentinelOff and CbClipboardRelay's
// kClipboardSentinelOff. When the URL equals this value (case-insensitive),
// the relay constructs in disabled state and emits no traffic. Lets tests
// run the M3 host stack without dragging in a real bridge sidecar.
inline constexpr char kFileUploadSentinelOff[] = "off";

// Defence-in-depth per-frame cap. The bridge enforces 1 MiB raw chunks +
// envelope overhead; base64 inflation on the wire pushes a max
// file_upload_chunk envelope to roughly 1.4 MiB. We cap at 1.5 MiB to
// accommodate that comfortably while still bounding worst-case
// allocations on the IO thread. Out-of-band envelopes (start/end/
// progress/complete/error) are all well under 4 KiB so this cap only
// ever bites file_upload_chunk.
//
// TODO(M6-R3-cap-source): if a future tenant config raises the chunk
// cap, plumb the value from file-bridge config to this constant.
inline constexpr size_t kMaxFileUploadFrameBytes = (3UL << 19);  // 1.5 MiB

// CbFileUploadBridgeWsClient — duplex WebSocket client to the file-
// bridge. Owns the WS handle for both directions of file-upload
// traffic: writes inbound DC frames to the bridge, reads outbound
// reply frames and dispatches them via the supplied callback.
//
// Lifetime: created on the io_task_runner sequence, lives for the
// life of CbFileUploadRelay. Reconnects opaquely on transport loss
// with exponential backoff. PostText() may be called from any
// thread; it PostTasks onto io_task_runner_ before touching the WS
// handle. The on-frame callback fires on io_task_runner_.
//
// # WS-backend choice (production wiring is a follow-up)
//
// Same calculus as M6 R2's CbClipboardBridgeWsClient. Chromium
// browser process exposes WebSocket clients through services/
// network's mojo-fronted WebSocket and net/websockets/WebSocket
// Channel. The network-service surface is the natural match for
// CbStatsRelay's SimpleURLLoader path but the raw-net surface is
// lighter wiring for loopback. R3 leaves the choice behind an
// interface so the bridge wiring + tests can land first.
//
// TODO(M6-R3-ws-backend): pick a backend (likely
// net/websockets/WebSocketChannel for parity with chromium's own
// DevTools WS dialer) and wire the EnsureConnected / read-loop /
// reconnect machinery. The DRAFT compiles + integration-tests via
// the fake-WS subclass used in cb_file_upload_relay_test.cc.
class CbFileUploadBridgeWsClient {
 public:
  // Callback fired on each text frame received from the bridge.
  // The relay's outbound dispatch wires this to dc_host_->Send(
  // kFileUpload, frame). The callback runs on io_task_runner_; the
  // host's Send handles its own thread hop.
  using OnFrameReceivedCallback =
      base::RepeatingCallback<void(std::string frame)>;

  CbFileUploadBridgeWsClient(
      std::string url,                                     // or "off"
      scoped_refptr<base::SequencedTaskRunner> io_task_runner);

  CbFileUploadBridgeWsClient(const CbFileUploadBridgeWsClient&) = delete;
  CbFileUploadBridgeWsClient& operator=(const CbFileUploadBridgeWsClient&) =
      delete;

  virtual ~CbFileUploadBridgeWsClient();

  // True when the URL is the "off" sentinel or empty — callers can
  // short-circuit body construction.
  bool disabled() const { return disabled_; }

  // Install the inbound-from-bridge callback. Must be called BEFORE
  // any reply frames may arrive (i.e. before EnsureConnected runs in
  // production, or before the test fake synthesises frames). Calling
  // with an empty callback is allowed — the read path becomes a drop.
  //
  // Thread: caller's thread; internally posts onto io_task_runner_
  // because the callback is read from the read-loop sequence.
  void SetOnFrameReceived(OnFrameReceivedCallback cb);

  // Send a single text frame to the bridge. Fire-and-forget; the
  // relay does not await a per-frame ack (the bridge's reply for an
  // upload arrives after the full end envelope is processed, not
  // per-chunk). If the WS is not currently connected, the frame is
  // dropped and a failure is recorded; the relay does NOT queue
  // across reconnects because mid-upload chunks would arrive out
  // of order relative to fresh frames the client is also sending
  // (the v1 protocol uses strictly-monotonic seq within an upload).
  //
  // Marked virtual so cb_file_upload_relay_test.cc can subclass with
  // a capture sink. Production callers always invoke through the
  // base pointer.
  virtual void PostText(std::string frame);

 protected:
  // Body of PostText after the io_task_runner hop. Exposed to the
  // test seam — production callers go through PostText.
  void PostOnIoSequence(std::string frame);

  // Test seam: dispatch a frame as if it had arrived from the bridge.
  // Production uses the real read loop; tests bypass it.
  void DispatchInboundForTest(std::string frame);

  // Connection lifecycle hooks — production impl wires WS open /
  // close / message; test fake leaves them no-op and synthesises
  // OnFrameSent + DispatchInboundForTest directly.
  // TODO(M6-R3-ws-backend) lands the bodies.
  virtual void EnsureConnected();    // idempotent connect
  virtual void OnTransportConnected();
  virtual void OnTransportDisconnected(int net_error_or_status);
  virtual void OnFrameSent(bool ok, int net_error_or_status);
  virtual void OnFrameRead(std::string frame);

  // Rate-limited log helper. Same cadence as CbStatsRelay's
  // ShouldLogFailure (1st of a burst, every 10th, then every 100th).
  void RecordSendSuccess();
  void RecordSendFailure(int net_error_or_status);

  const std::string url_;
  const bool disabled_;
  const scoped_refptr<base::SequencedTaskRunner> io_task_runner_;

  // Inbound dispatch (bridge → DC). Touched only on io_task_runner_.
  OnFrameReceivedCallback on_frame_received_;

  // Burst counters — all touched only on io_task_runner_.
  int64_t consecutive_send_failures_ = 0;
  int64_t send_success_count_ = 0;
  int64_t frames_received_ = 0;
  int64_t frames_dropped_oversize_inbound_ = 0;

  // Reconnect backoff state — all touched only on io_task_runner_.
  // Initial 1 s, doubles up to 30 s cap, resets on connect success.
  // Matches the cadence the rest of M6 uses.
  base::TimeDelta reconnect_backoff_ = base::Seconds(1);
  bool reconnect_pending_ = false;

  base::WeakPtrFactory<CbFileUploadBridgeWsClient> weak_factory_{this};
};

// CbFileUploadRelay — webrtc::DataChannelObserver attached to the
// "files" DC. On each inbound text message, forwards the raw body
// to the file-bridge WS endpoint via CbFileUploadBridgeWsClient. On
// outbound reply frames from the bridge (received via the client's
// read-loop callback), forwards each via dc_host_->Send(kFileUpload,
// text).
//
// Ownership: the M3 PCF host constructs this and calls
// dc_host_->BindObserver(CbDcLabel::kFileUpload, this). The host's
// per-channel trampoline keeps a raw pointer back via BindObserver;
// callers MUST BindObserver(kFileUpload, nullptr) (or destroy the
// host) before destroying this relay.
//
// Unlike CbClipboardRelay (which delegates the outbound direction
// to a separate CbClipboardOutboundServer), CbFileUploadRelay owns
// BOTH directions because the file-bridge speaks one duplex WS. The
// inbound DC observer and the outbound WS read-loop share a single
// CbFileUploadBridgeWsClient instance.
class CbFileUploadRelay : public webrtc::DataChannelObserver {
 public:
  // |client|: owns the duplex WS to the bridge. Must not be null.
  // |dc_host|: M3 R5's CbDataChannelHost. Must outlive this relay.
  //     Outbound frames are forwarded via dc_host->Send(kFileUpload,
  //     text); the host handles the signaling-thread hop internally.
  //     Pass nullptr to disable the outbound direction entirely (the
  //     read-loop still drains the WS but drops frames; useful when
  //     the host isn't wired in early-startup tests).
  CbFileUploadRelay(
      std::unique_ptr<CbFileUploadBridgeWsClient> client,
      signaling::CbDataChannelHost* dc_host);

  CbFileUploadRelay(const CbFileUploadRelay&) = delete;
  CbFileUploadRelay& operator=(const CbFileUploadRelay&) = delete;

  ~CbFileUploadRelay() override;

  // webrtc::DataChannelObserver — invoked on libwebrtc signaling/
  // network thread.
  void OnMessage(const webrtc::DataBuffer& buffer) override;
  void OnStateChange() override;
  void OnBufferedAmountChange(uint64_t sent_data_size) override;
  bool IsOkToCallOnTheNetworkThread() override;

 private:
  // Called from the WS read-loop (io_task_runner_) for every text
  // frame the bridge sends back. Validates + forwards via dc_host_
  // ->Send(kFileUpload, text). Static-bound on construction via
  // client_->SetOnFrameReceived.
  void OnBridgeReplyFrame(std::string frame);

  // Per-frame guard. R3 enforces the cap as defence-in-depth even
  // though the bridge already does — a misconfigured bridge
  // shouldn't be able to wedge the relay with a giant frame.
  bool FrameWithinCap(const std::string& frame) const;

  const std::unique_ptr<CbFileUploadBridgeWsClient> client_;
  // raw_ptr because dc_host_ is caller-owned and must outlive us.
  // May be nullptr — see ctor docs.
  // CV2-75 fix-forward: wrapped in raw_ptr<T> wrapper per chromium-
  // rawptr lint (author comment already declared raw_ptr intent;
  // form was raw `*` instead of wrapped template).
  const raw_ptr<signaling::CbDataChannelHost> dc_host_;

  // Outbound DC send counters — touched only on the io_task_runner_
  // sequence where OnBridgeReplyFrame fires.
  int64_t outbound_frames_forwarded_ = 0;
  int64_t outbound_frames_dropped_oversize_ = 0;
  int64_t outbound_frames_dropped_no_host_ = 0;
  int64_t consecutive_outbound_send_failures_ = 0;

  base::WeakPtrFactory<CbFileUploadRelay> weak_factory_{this};
};

// ---------------------------------------------------------------------
// Env-var helper — matches the M6 R1 ResolveStatsSidecarUrl shape.
// ---------------------------------------------------------------------

// Returns the value of CHROMELESS_FILE_UPLOAD_BRIDGE_URL if set (with
// the "off" sentinel forwarded unchanged), otherwise the default
// bridge URL. Lives here so the M3 wiring code doesn't need to know
// the env-var name shape.
std::string ResolveFileUploadBridgeUrl();

}  // namespace cloud_browser

#endif  // CLOUD_BROWSER_CAPTURE_BUILD_INTEGRATION_CB_FILE_UPLOAD_RELAY_H_

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// ICE-server configuration source for cb-chromium's native signaling
// client — M3 R3 (CV2-53).
//
// Replaces streamer.js's iceServers / ice_transport_policy URL-query
// plumbing (capture/streamer-page/streamer.js, lines 84-86 +
// parseIceServersParam at line 126) with a native C++ env-driven
// config loader. The streamer page is being torn out wholesale; this
// loader feeds M3 R4's handshake driver, which assembles the
// webrtc::PeerConnectionInterface::RTCConfiguration that pcf_->
// CreatePeerConnection consumes.
//
// Scope (CV2-53):
//   * Read STUN / TURN URLs + TURN credentials from POD ENV at
//     browser-process startup.
//   * Surface them as a std::vector<webrtc::PeerConnectionInterface::
//     IceServer> ready to slot into RTCConfiguration::servers.
//   * Surface the ice_transport_policy choice ("all" vs "relay") as
//     webrtc::PeerConnectionInterface::IceTransportsType, ready to
//     slot into RTCConfiguration::type.
//   * Empty / unset env → a single Google public STUN server
//     (mirrors streamer.js's DEFAULT_ICE_SERVERS).
//
// Out of scope:
//   * Dynamic TURN credential rotation / TURN REST API short-lived
//     credential fetch — left for a later sub-deliverable.
//   * Bundle / RTCP-mux / SDP semantics — those live in
//     RTCConfiguration too but are owned by M3 R4 (CV2-54).
//   * ICE candidate generation, trickle, gathering — webrtc owns
//     all of that internally once RTCConfiguration is handed off.
//
// # Configuration source — env vars
//
//   WEBRTC_ICE_SERVERS              JSON, optional. The same JSON
//                                   shape that the legacy streamer.js
//                                   `ice_servers` URL param accepts:
//
//                                     [{ "urls": ["stun:..."] },
//                                      { "urls": ["turn:...:3478?transport=udp",
//                                                 "turn:...:3478?transport=tcp"],
//                                        "username": "alice",
//                                        "credential": "hunter2" }]
//
//                                   …or the wrapped form
//                                   `{ "iceServers": [ ... ] }`.
//                                   Unset / empty / unparsable →
//                                   the default single-STUN list.
//
//   WEBRTC_ICE_TRANSPORT_POLICY     "all" (default) | "relay". Any
//                                   other value falls back to "all".
//
// LoadConfigFromEnv() is the env-reading helper; the embedder in
// cloud_browser_browser_main_parts.{cc,h} calls it once at
// PreMainMessageLoopRun and hands the resulting IceConfig off to M3
// R4 alongside the M1 PCF + the M3 R2 WS client.
//
// # Why JSON and not a delimited list?
//
// streamer.js shipped the JSON shape since the wave-1 cut over; the
// triform.dev portal already serializes the operator-configured
// iceServers as the JSON form. Reusing the exact shape means
// physics's existing TURN-credential plumbing (any future short-
// lived TURN REST issuer) lands without a wire-shape change — only
// the transport flips from URL query param to env.
//
// # Threading model
//
// LoadConfigFromEnv() runs once on the UI thread at startup. The
// returned IceConfig is immutable; subsequent reads happen from M3
// R4's PeerConnection assembly on the UI thread before any
// PeerConnection exists. No cross-thread access.
//
// Cross-references:
//   * capture/signaling/cb_signaling_ws_client.{h,cc} (M3 R2 — peer
//     WS client; LoadConfigFromEnv there mirrors this style)
//   * capture/build-integration/cloud_browser_pcf.{h,cc} (M1 — PCF
//     that pcf_->CreatePeerConnection lives on)
//   * capture/streamer-page/streamer.js (legacy — JSON parser
//     reference that this loader is contract-compatible with)
//   * physics/.../webrtc_signaling.rs (broker, not directly involved
//     here but downstream of the IceServer values that end up in SDP)

#ifndef CAPTURE_SIGNALING_CB_ICE_CONFIG_H_
#define CAPTURE_SIGNALING_CB_ICE_CONFIG_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "api/peer_connection_interface.h"

namespace cloud_browser::signaling {

// Aggregate immutable result of reading the ICE config env vars.
//
// Constructed once at startup; passed by const-ref into M3 R4's
// PeerConnection assembly. The webrtc types are value-typed (
// IceServer is a plain struct of std::string / std::vector;
// IceTransportsType is an enum class), so copies are cheap and
// the embedder can keep one canonical instance for the
// cb-chromium browser-process lifetime.
struct IceConfig {
  // Slots directly into RTCConfiguration::servers. Always non-
  // empty after a successful Load*; defaults to a single
  // stun:stun.l.google.com:19302 entry when env is unset / unparsable.
  std::vector<webrtc::PeerConnectionInterface::IceServer> servers;

  // Slots directly into RTCConfiguration::type. kAll (default) or
  // kRelay. The legacy streamer.js parser collapsed every non-"relay"
  // value to "all"; we keep that behaviour for source-compat.
  webrtc::PeerConnectionInterface::IceTransportsType transport_policy =
      webrtc::PeerConnectionInterface::IceTransportsType::kAll;

  // Counts derived at parse time for a single deterministic info-
  // log line in the embedder. Mirrors streamer.js's
  // iceServerSummary() output. Not used by webrtc; pure
  // observability.
  struct Summary {
    size_t stun = 0;
    size_t turn = 0;
    size_t other = 0;  // unrecognised scheme — kept in `servers`
                       // verbatim, webrtc will reject at PC time.
  };
  Summary summary;
};

// Parse the legacy streamer.js JSON shape into IceConfig.servers.
//
// `raw_json` is the verbatim string read from env (or empty when
// unset). Returns std::nullopt when:
//   * raw_json is empty;
//   * JSON parse fails;
//   * the parsed value is not an array nor an object with an
//     `iceServers` array key;
//   * after normalisation, no server entries are usable.
//
// On success: the returned vector is non-empty and each entry has
// at least one non-empty url. username + credential are populated
// only when present and non-empty (mirrors streamer.js's
// normalised output exactly).
//
// Surfaced separately from LoadConfigFromEnv() so unit tests can
// exercise the parser without env-state coupling.
std::optional<std::vector<webrtc::PeerConnectionInterface::IceServer>>
ParseIceServersJson(std::string_view raw_json);

// Map the ice-transport-policy env value to the webrtc enum.
//
// "relay" (case-insensitive) → kRelay; everything else (including
// empty / unset / unrecognised) → kAll. Pure function; no env
// access. Mirrors streamer.js's parseIceTransportPolicyParam.
webrtc::PeerConnectionInterface::IceTransportsType ParseIceTransportPolicy(
    std::string_view raw);

// Build the default single-STUN server list — the fallback used
// when WEBRTC_ICE_SERVERS is unset or unparsable. Mirrors
// streamer.js's DEFAULT_ICE_SERVERS.
std::vector<webrtc::PeerConnectionInterface::IceServer>
BuildDefaultIceServers();

// Compute the (stun/turn/other) counts for a server list — useful
// for the embedder's startup info log and for tests.
IceConfig::Summary SummariseIceServers(
    const std::vector<webrtc::PeerConnectionInterface::IceServer>& servers);

// Read WEBRTC_ICE_SERVERS / WEBRTC_ICE_TRANSPORT_POLICY from the
// process env (getenv(3)) and produce the resolved IceConfig.
//
// Never returns nullopt — the contract guarantees a usable config
// (the default-STUN fallback fires whenever the JSON path fails).
// The optional return type is reserved for a future hard-fail
// mode (e.g. strict env: refuse to boot without a configured
// TURN server) — not used yet.
//
// TODO(M3-R3-env-source): chromium's preferred env-reading idiom
// in the browser process is base::Environment::Create() ->
// GetVar(name, &value), not raw getenv(3). M3 R2 carries the same
// TODO; resolve both in one pass during the first compile
// validation on triform-8.
//
// TODO(M3-R3-turn-rest): wire short-lived TURN REST credential
// fetch behind a separate env (WEBRTC_TURN_REST_URL). Out of
// scope for R3's static-credentials cut; the IceConfig struct
// stays the same shape.
std::optional<IceConfig> LoadConfigFromEnv();

}  // namespace cloud_browser::signaling

#endif  // CAPTURE_SIGNALING_CB_ICE_CONFIG_H_

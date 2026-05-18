// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Wire-envelope codec for the cb-chromium ↔ physics signaling channel.
//
// Module M3 R1 of the ChromelessV2 native-peer migration (CV2-51).
// Source-pins the wire contract that the now-deleted streamer.js
// historically owned: a JSON envelope `{type, from, data}` with six
// permitted `type` tags. The browser-process signaling client (M3 R2)
// is the sole producer; physics's `webrtc_signaling.rs` broker is the
// sole arbiter of which envelopes round-trip across the WS.
//
// # Wire contract
//
// ```
// { "type":  "offer" | "answer" | "ice" | "bye"
//          | "request_renegotiate" | "probe_result",
//   "from":  "browser",      // always "browser" on emit; sibling of
//                            // type, NOT nested under data.
//   "data":  <variant> }     // per-tag shape — see EnvelopeData below.
// ```
//
// # Per-tag `data` shape
//
//   offer    / answer  : { "type": "offer"|"answer", "sdp": "<sdp>" }
//   ice                : RTCIceCandidateInit object  OR  JSON null
//                        (null == end-of-candidates marker).
//   bye                : the `data` field is OMITTED entirely
//                        (not present, not null, not {}).
//   request_renegotiate: JSON null.
//   probe_result       : opaque object (receive-only — the browser
//                        peer never emits this; physics-side test code
//                        and the portal client are the only producers).
//
// # Source-pinning
//
// The six tags above are the COMPLETE accept-list. Any other tag
// (`sdp_offer`, `candidate`, `hello`, `restart_ice`, etc.) MUST be
// rejected at decode time so the contract cannot drift unobserved.
// Anchors:
//   * physics/src/api/handlers/webrtc_signaling.rs:29  (doc-comment
//     ABNF of the same six tags)
//   * physics/src/api/handlers/webrtc_signaling.rs:124-155
//     (SignalingEnvelope serde-tagged enum, lockstep with this header)
//   * physics/src/api/handlers/webrtc_signaling.rs:233
//     (REPLAYABLE_TYPES = offer / answer / request_renegotiate)
//   * capture/streamer-page/streamer.js:1975-1978  (legacy `offer`
//     emit — preserved as the M3 R1 round-trip fixture corpus until
//     the streamer page is physically deleted in M7)
//
// # Non-goals (other M3 R#s + later modules)
//
//   * R2 — WebSocket transport, reconnect, JWT auth.
//   * R3 — ICE config endpoint fetch + TURN credential rotation.
//   * R4 — PeerConnection wire-up onto M1's PCF.
//   * R5 — DataChannel host (input, cursor, ancillary).
//
// This file is pure codec: encode a typed value to JSON string,
// decode a JSON string to a typed value. No I/O, no threading.

#ifndef CAPTURE_SIGNALING_CB_WIRE_ENVELOPE_H_
#define CAPTURE_SIGNALING_CB_WIRE_ENVELOPE_H_

#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "base/values.h"

namespace cloud_browser {
namespace signaling {

// The six permitted envelope tags. Wire encoding matches the string
// names below (lowercase, underscore-separated for the multi-word
// tags) — locked by physics/src/api/handlers/webrtc_signaling.rs's
// `#[serde(tag = "type", rename_all = "snake_case")]`.
enum class EnvelopeType {
  kOffer,
  kAnswer,
  kIce,
  kBye,
  kRequestRenegotiate,
  kProbeResult,
};

// Decode-side: returns the EnvelopeType matching `tag` or nullopt if
// `tag` is not one of the six pinned strings. EVERY caller MUST treat
// nullopt as a rejected envelope — no fallthrough, no "best-effort"
// remap. The negative-test fixture in cb_wire_envelope_test.cc locks
// this with `sdp_offer` (the historic v0 tag the contract intentionally
// dropped).
std::optional<EnvelopeType> TagFromString(std::string_view tag);

// Encode-side: the canonical wire string for each tag.
std::string_view TagToString(EnvelopeType type);

// `from` field. Browser is the only value the cb-chromium emitter ever
// writes; we model the other side so decoded envelopes (replays from
// physics, probe_result frames originating elsewhere) round-trip
// without lossy down-conversion to a bool. Mirrors
// physics::PeerRole (webrtc_signaling.rs:94).
enum class PeerRole {
  kBrowser,
  kClient,
};

std::optional<PeerRole> RoleFromString(std::string_view role);
std::string_view RoleToString(PeerRole role);

// Strongly-typed per-tag payload.
//
// kBye carries no data (the wire field is omitted entirely), so it
// has no payload struct here. We discriminate on EnvelopeType in
// Envelope::data; encode-side omits `data` for kBye and skips
// serializing it.
//
// kRequestRenegotiate carries JSON null on the wire — we model that
// as an empty struct so the typed shape stays stable across the six
// tags (callers don't have to special-case it on encode).
struct SdpPayload {
  // `type` field INSIDE data — value must equal the envelope's tag
  // (i.e. "offer" for kOffer, "answer" for kAnswer). The streamer.js
  // legacy emitter sets this from offer.type (RTCSessionDescription's
  // own type field), so it's redundant with the outer tag but the
  // contract requires it.
  std::string sdp_type;
  std::string sdp;
};

// RTCIceCandidateInit subset — the four fields RTCIceCandidate's
// JSON.stringify form emits via toJSON(). usernameFragment is part
// of the W3C spec but libwebrtc's emit-side currently omits it; we
// keep it optional on decode so a future libwebrtc bump that starts
// emitting it doesn't crash the codec.
//
// Special case: an `ice` envelope with JSON-null `data` is the
// end-of-candidates marker. We model that as `Envelope::data` holding
// a default-constructed (empty) IceCandidatePayload with
// `is_end_of_candidates = true`. Callers MUST check that bool before
// reading the candidate string.
struct IceCandidatePayload {
  bool is_end_of_candidates = false;
  std::string candidate;
  std::optional<std::string> sdp_mid;
  std::optional<int> sdp_m_line_index;
  std::optional<std::string> username_fragment;
};

// Opaque receive-side payload for probe_result. The browser peer
// NEVER emits this — the four numeric fields M0 R5's streamer
// applies in applyProbeResult() are consumed in M3 R4 by the
// PeerConnection-config code, not here. We keep the raw base::Value
// so we can pass it through verbatim without locking in a numeric
// schema that physics may evolve.
struct ProbeResultPayload {
  base::DictValue raw;

  // chromium 7727: base::DictValue (formerly base::Value::Dict) is
  // move-only — no implicit copy. Without an explicit deep-copy ctor
  // here, ProbeResultPayload → EnvelopeData variant → Envelope all
  // become non-copyable, which breaks value-semantics callers like
  // cb_signaling_ws_client.cc Send() (copies an Envelope to rewrite
  // `from`). DictValue::Clone() gives the deep copy.
  ProbeResultPayload() = default;
  explicit ProbeResultPayload(base::DictValue r) : raw(std::move(r)) {}
  ProbeResultPayload(ProbeResultPayload&&) = default;
  ProbeResultPayload& operator=(ProbeResultPayload&&) = default;
  ProbeResultPayload(const ProbeResultPayload& other)
      : raw(other.raw.Clone()) {}
  ProbeResultPayload& operator=(const ProbeResultPayload& other) {
    raw = other.raw.Clone();
    return *this;
  }
};

// Discriminated union over the per-tag payloads. The active
// alternative MUST match the Envelope::type tag — encode asserts
// this in DEBUG, decode constructs it directly from the wire tag.
//
// std::monostate covers kBye (no payload) and kRequestRenegotiate
// (null payload).
using EnvelopeData = std::variant<std::monostate,
                                  SdpPayload,
                                  IceCandidatePayload,
                                  ProbeResultPayload>;

// The decoded envelope. Encode-side fills in type + from + data;
// decode-side returns this on success. `from` is rewritten to
// kBrowser unconditionally by the M3 R2 emitter before encode
// (mirrors streamer.js:1976, where the browser always sets
// `from: "browser"` on its own outgoing envelopes). The codec itself
// does NOT enforce that — the broker contract is what enforces it
// (webrtc_signaling.rs::with_from rewrites server-side too).
struct Envelope {
  EnvelopeType type;
  PeerRole from;
  EnvelopeData data;
};

// Serialize `env` to a UTF-8 JSON string suitable for direct write
// onto the signaling WebSocket. Returns nullopt only on encoding
// invariant violation (active variant doesn't match the tag);
// well-formed input always succeeds.
std::optional<std::string> Encode(const Envelope& env);

// Decode a UTF-8 JSON string from the signaling WebSocket into an
// Envelope. Returns nullopt for:
//   * non-JSON input (parse failure)
//   * JSON that isn't a top-level object
//   * missing `type` or `from` field
//   * `type` not one of the six pinned tags (THIS IS LOAD-BEARING —
//     `sdp_offer` and friends MUST be rejected; the negative test
//     pins this)
//   * `from` not one of "browser" / "client"
//   * per-tag `data` shape violation (e.g. ice with non-null,
//     non-object data; offer/answer with non-object data; bye with
//     a `data` field present)
//
// On success the returned Envelope's data variant active alternative
// matches the type tag.
std::optional<Envelope> Decode(std::string_view json);

// Test-helper: build an `ice` envelope with null `data` (the
// end-of-candidates marker). Exported because the negative-test
// fixture and the M3 R2 wiring both need it, and putting the
// construction recipe in one place keeps the marker convention
// from drifting.
Envelope MakeIceEndOfCandidates(PeerRole from);

}  // namespace signaling
}  // namespace cloud_browser

#endif  // CAPTURE_SIGNALING_CB_WIRE_ENVELOPE_H_

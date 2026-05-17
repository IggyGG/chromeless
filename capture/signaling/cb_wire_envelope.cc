// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cb_wire_envelope.cc — see cb_wire_envelope.h.

#include "capture/signaling/cb_wire_envelope.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"

namespace cloud_browser {
namespace signaling {

// ---------------------------------------------------------------------
// Tag <-> string
// ---------------------------------------------------------------------

std::optional<EnvelopeType> TagFromString(std::string_view tag) {
  if (tag == "offer") return EnvelopeType::kOffer;
  if (tag == "answer") return EnvelopeType::kAnswer;
  if (tag == "ice") return EnvelopeType::kIce;
  if (tag == "bye") return EnvelopeType::kBye;
  if (tag == "request_renegotiate") return EnvelopeType::kRequestRenegotiate;
  if (tag == "probe_result") return EnvelopeType::kProbeResult;
  // Source-pin: every other string (including the historic v0
  // `sdp_offer`, `candidate`, `hello`) MUST fall through to nullopt.
  // The negative test in cb_wire_envelope_test.cc locks this.
  return std::nullopt;
}

std::string_view TagToString(EnvelopeType type) {
  switch (type) {
    case EnvelopeType::kOffer: return "offer";
    case EnvelopeType::kAnswer: return "answer";
    case EnvelopeType::kIce: return "ice";
    case EnvelopeType::kBye: return "bye";
    case EnvelopeType::kRequestRenegotiate: return "request_renegotiate";
    case EnvelopeType::kProbeResult: return "probe_result";
  }
  // Unreachable — enum is closed and exhaustively matched above.
  // CHECK rather than fall through so a future expansion that forgets
  // to update the switch crashes loudly in debug builds.
  CHECK(false) << "unhandled EnvelopeType";
  return {};
}

std::optional<PeerRole> RoleFromString(std::string_view role) {
  if (role == "browser") return PeerRole::kBrowser;
  if (role == "client") return PeerRole::kClient;
  return std::nullopt;
}

std::string_view RoleToString(PeerRole role) {
  switch (role) {
    case PeerRole::kBrowser: return "browser";
    case PeerRole::kClient: return "client";
  }
  CHECK(false) << "unhandled PeerRole";
  return {};
}

// ---------------------------------------------------------------------
// Encode
// ---------------------------------------------------------------------
//
// Each per-tag branch builds the `data` field's base::DictValue (or
// returns the omit-data / null-data marker), then we splice into the
// outer envelope and JSONWriter::Write.

namespace {

// Encode the per-tag `data` payload. Returns:
//   * std::nullopt + ok=true   -> emit envelope with `data` field
//                                 OMITTED (only happens for kBye).
//   * a base::Value (incl. null) + ok=true -> use as the data value.
//   * ok=false on invariant violation (active variant doesn't match
//     the tag) — callers propagate this to Encode()'s nullopt return.
struct EncodedData {
  bool ok = false;
  bool omit_field = false;
  base::Value value;
};

EncodedData EncodeData(EnvelopeType type, const EnvelopeData& data) {
  EncodedData out;
  switch (type) {
    case EnvelopeType::kOffer:
    case EnvelopeType::kAnswer: {
      const auto* sdp = std::get_if<SdpPayload>(&data);
      if (!sdp) return out;  // ok=false
      base::DictValue d;
      d.Set("type", sdp->sdp_type);
      d.Set("sdp", sdp->sdp);
      out.ok = true;
      out.value = base::Value(std::move(d));
      return out;
    }
    case EnvelopeType::kIce: {
      const auto* ice = std::get_if<IceCandidatePayload>(&data);
      if (!ice) return out;
      if (ice->is_end_of_candidates) {
        // null `data` — distinct from "absent". JSONWriter writes
        // base::Value() (a NONE Value) as the literal `null`.
        out.ok = true;
        out.value = base::Value();  // NONE -> null on the wire.
        return out;
      }
      base::DictValue d;
      d.Set("candidate", ice->candidate);
      if (ice->sdp_mid) d.Set("sdpMid", *ice->sdp_mid);
      if (ice->sdp_m_line_index) {
        d.Set("sdpMLineIndex", *ice->sdp_m_line_index);
      }
      if (ice->username_fragment) {
        d.Set("usernameFragment", *ice->username_fragment);
      }
      out.ok = true;
      out.value = base::Value(std::move(d));
      return out;
    }
    case EnvelopeType::kBye: {
      // `data` field omitted entirely on the wire (streamer.js:1489
      // `{type: "bye", from: "browser"}` — no data field). The
      // variant MUST be monostate for kBye.
      if (!std::holds_alternative<std::monostate>(data)) return out;
      out.ok = true;
      out.omit_field = true;
      return out;
    }
    case EnvelopeType::kRequestRenegotiate: {
      // Wire is JSON null. Monostate variant is the only legal shape.
      if (!std::holds_alternative<std::monostate>(data)) return out;
      out.ok = true;
      out.value = base::Value();  // null on the wire.
      return out;
    }
    case EnvelopeType::kProbeResult: {
      const auto* probe = std::get_if<ProbeResultPayload>(&data);
      if (!probe) return out;
      // Clone the dict — Envelope is logically immutable; we don't
      // mutate the caller's payload.
      out.ok = true;
      out.value = base::Value(probe->raw.Clone());
      return out;
    }
  }
  return out;
}

}  // namespace

std::optional<std::string> Encode(const Envelope& env) {
  EncodedData ed = EncodeData(env.type, env.data);
  if (!ed.ok) {
    // Active variant doesn't match the tag — encode invariant
    // violation. Callers should treat this as a programming bug;
    // we return nullopt rather than crash so tests can exercise it.
    return std::nullopt;
  }

  base::DictValue envelope;
  envelope.Set("type", TagToString(env.type));
  envelope.Set("from", RoleToString(env.from));
  if (!ed.omit_field) {
    envelope.Set("data", std::move(ed.value));
  }

  std::string out;
  if (!base::JSONWriter::Write(envelope, &out)) {
    return std::nullopt;
  }
  return out;
}

// ---------------------------------------------------------------------
// Decode
// ---------------------------------------------------------------------

namespace {

// Returns nullopt on per-tag data-shape violation; returns a
// monostate variant for kBye / kRequestRenegotiate; returns the typed
// payload for the other four.
//
// `data` may be nullptr (meaning the field was absent on the wire) or
// point into the parsed top-level dict. The kBye branch demands
// nullptr; every other branch demands a non-null value (which may be
// JSON null for ice's end-of-candidates marker and for
// request_renegotiate).
std::optional<EnvelopeData> DecodeData(EnvelopeType type,
                                       const base::Value* data) {
  switch (type) {
    case EnvelopeType::kOffer:
    case EnvelopeType::kAnswer: {
      if (!data || !data->is_dict()) return std::nullopt;
      const base::DictValue& d = data->GetDict();
      const std::string* sdp_type = d.FindString("type");
      const std::string* sdp = d.FindString("sdp");
      if (!sdp_type || !sdp) return std::nullopt;
      // Contract: data.type must equal the envelope tag. This mirrors
      // streamer.js:1977 (`data: { type: offer.type, sdp: ... }`) where
      // offer.type came from RTCSessionDescription.type. If it ever
      // diverges, that's a producer bug and we reject the frame.
      const std::string_view expected = TagToString(type);
      if (*sdp_type != expected) return std::nullopt;
      return EnvelopeData{SdpPayload{*sdp_type, *sdp}};
    }
    case EnvelopeType::kIce: {
      if (!data) return std::nullopt;  // field must be present.
      if (data->is_none()) {
        IceCandidatePayload eoc;
        eoc.is_end_of_candidates = true;
        return EnvelopeData{std::move(eoc)};
      }
      if (!data->is_dict()) return std::nullopt;
      const base::DictValue& d = data->GetDict();
      IceCandidatePayload p;
      p.is_end_of_candidates = false;
      // RTCIceCandidate.toJSON() guarantees `candidate` is always a
      // string (possibly empty). libwebrtc emits it through the
      // streamer in this same shape (streamer.js:1964 calls
      // candidate.toJSON()).
      const std::string* candidate = d.FindString("candidate");
      if (!candidate) return std::nullopt;
      p.candidate = *candidate;
      if (const std::string* sm = d.FindString("sdpMid")) {
        p.sdp_mid = *sm;
      }
      if (std::optional<int> idx = d.FindInt("sdpMLineIndex")) {
        p.sdp_m_line_index = *idx;
      }
      if (const std::string* uf = d.FindString("usernameFragment")) {
        p.username_fragment = *uf;
      }
      return EnvelopeData{std::move(p)};
    }
    case EnvelopeType::kBye: {
      // `data` MUST be absent for bye. A present-but-null `data`
      // field is a contract violation — bye carries no payload at
      // all, not even null.
      if (data) return std::nullopt;
      return EnvelopeData{std::monostate{}};
    }
    case EnvelopeType::kRequestRenegotiate: {
      // Wire is null. Absent field is also accepted (defensive — a
      // future producer might omit instead of nulling, and the
      // semantic is identical).
      if (data && !data->is_none()) return std::nullopt;
      return EnvelopeData{std::monostate{}};
    }
    case EnvelopeType::kProbeResult: {
      if (!data || !data->is_dict()) return std::nullopt;
      return EnvelopeData{ProbeResultPayload{data->GetDict().Clone()}};
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<Envelope> Decode(std::string_view json) {
  std::optional<base::Value> parsed = base::JSONReader::Read(json);
  if (!parsed || !parsed->is_dict()) return std::nullopt;
  const base::DictValue& dict = parsed->GetDict();

  const std::string* type_str = dict.FindString("type");
  if (!type_str) return std::nullopt;
  std::optional<EnvelopeType> type = TagFromString(*type_str);
  if (!type) return std::nullopt;  // source-pin rejection.

  const std::string* from_str = dict.FindString("from");
  if (!from_str) return std::nullopt;
  std::optional<PeerRole> from = RoleFromString(*from_str);
  if (!from) return std::nullopt;

  const base::Value* data = dict.Find("data");
  std::optional<EnvelopeData> data_payload = DecodeData(*type, data);
  if (!data_payload) return std::nullopt;

  return Envelope{*type, *from, std::move(*data_payload)};
}

// ---------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------

Envelope MakeIceEndOfCandidates(PeerRole from) {
  IceCandidatePayload eoc;
  eoc.is_end_of_candidates = true;
  return Envelope{EnvelopeType::kIce, from, EnvelopeData{std::move(eoc)}};
}

}  // namespace signaling
}  // namespace cloud_browser

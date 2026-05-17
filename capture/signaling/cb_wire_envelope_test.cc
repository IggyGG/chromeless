// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Unit tests for capture/signaling/cb_wire_envelope.{h,cc} — M3 R1
// (CV2-51) wire-envelope codec.
//
// # Fixture provenance discipline
//
// Per feedback_test_fixtures_from_real_artifacts (memory), every
// envelope literal in this file MUST come from a REAL emitter or a
// REAL accept-path, not from spec-by-reading. Each fixture carries an
// inline provenance comment naming the producer + the file:line the
// shape was captured from. If the producer changes, refresh the
// fixture rather than mutating it to "match what the test expects".
//
// Provenance sources for this round:
//   * capture/streamer-page/streamer.js — pre-M7 JS emitter. Still
//     authoritative for the offer / ice / bye / ice-end-of-candidates
//     shapes because that's what physics's broker has been
//     round-tripping since chromeless v0; deletion-target in M7 only
//     after the native peer is fully wired.
//   * physics/src/api/handlers/webrtc_signaling.rs — accept-side
//     serde enum. Authoritative for `from` semantics + the
//     `request_renegotiate` / `probe_result` shapes the JS emitter
//     doesn't produce.
//   * physics/src/api/handlers/screencast_ws.rs:4238-4260 — fixture
//     for `request_renegotiate` and `probe_result` is lifted from
//     the pattern-C reject test there.

#include "capture/signaling/cb_wire_envelope.h"

#include <optional>
#include <string>
#include <string_view>

#include "base/json/json_reader.h"
#include "base/values.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace cloud_browser {
namespace signaling {
namespace {

using ::testing::Optional;

// ---------------------------------------------------------------------
// Fixtures — copy/paste from REAL artifacts. Do not edit shape!
// ---------------------------------------------------------------------

// Provenance: capture/streamer-page/streamer.js:1975-1978. The
// streamer constructs offer.sdp from a real RTCPeerConnection.
// createOffer() inside the running cb-chromium streamer page; the
// shape (`type: "offer", from: "browser", data: { type: "offer", sdp
// }`) is unconditional. The `sdp` body below is a trimmed real
// libwebrtc-emitted SDP captured from a smoke run; the codec does
// not parse SDP so the shortened form exercises the same code path.
constexpr char kRealOfferEnvelope[] = R"({
  "type": "offer",
  "from": "browser",
  "data": {
    "type": "offer",
    "sdp": "v=0\r\no=- 4611731400430051336 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\nc=IN IP4 0.0.0.0\r\na=mid:0\r\na=sendonly\r\na=rtpmap:96 VP9/90000\r\n"
  }
})";

// Provenance: capture/streamer-page/streamer.js:1961-1968. onicecandidate
// emits exactly this shape when ev.candidate is non-null
// (candidate.toJSON()). The candidate string below is a real srflx
// candidate captured from a libwebrtc gathering pass — only the IP +
// port were redacted; the structural fields are intact.
constexpr char kRealIceEnvelope[] = R"({
  "type": "ice",
  "from": "browser",
  "data": {
    "candidate": "candidate:842163049 1 udp 1677729535 192.0.2.1 51234 typ srflx raddr 0.0.0.0 rport 0 generation 0 ufrag 4ZcD network-id 1",
    "sdpMid": "0",
    "sdpMLineIndex": 0,
    "usernameFragment": "4ZcD"
  }
})";

// Provenance: capture/streamer-page/streamer.js:1961-1968 with the
// `ev.candidate ? ... : null` branch taken. End-of-candidates marker
// is the null-data form.
constexpr char kRealIceEndOfCandidatesEnvelope[] = R"({
  "type": "ice",
  "from": "browser",
  "data": null
})";

// Provenance: capture/streamer-page/streamer.js:1489. teardown()
// emits this two-field envelope — no `data` field at all.
constexpr char kRealByeEnvelope[] = R"({"type":"bye","from":"browser"})";

// Provenance: physics/src/api/handlers/screencast_ws.rs:4238-4244
// (pattern_c_request_renegotiate_and_probe_result_drop test fixture).
// The wire shape physics's broker accepts and the M3 R4 PeerConnection
// will react to.
constexpr char kRealRequestRenegotiateEnvelope[] = R"({
  "type": "request_renegotiate",
  "from": "client",
  "data": null
})";

// Provenance: physics/src/api/handlers/screencast_ws.rs:4245-4260 +
// streamer.js:1516 (`probe_result: bad numbers` warn-path showing
// the four numeric fields). The browser DECODES this from the
// client; it never emits it.
constexpr char kRealProbeResultEnvelope[] = R"({
  "type": "probe_result",
  "from": "client",
  "data": {
    "down_kbps": 4500,
    "up_kbps": 1200,
    "rtt_ms": 38,
    "loss": 0.0
  }
})";

// Negative-test fixture: the historic v0 `sdp_offer` tag. Physics
// has NEVER accepted this — webrtc_signaling.rs:124-155's serde enum
// renames to snake_case and only declares the six current tags. We
// pin the rejection here so a future codec edit that "helpfully"
// remaps sdp_offer -> offer breaks loudly.
constexpr char kRejectedSdpOfferEnvelope[] = R"({
  "type": "sdp_offer",
  "from": "browser",
  "data": { "type": "offer", "sdp": "v=0\r\n" }
})";

// ---------------------------------------------------------------------
// Decode -> Encode -> Decode round-trip helpers
// ---------------------------------------------------------------------

// Equality on the parsed JSON tree (so whitespace / key-order
// differences between the original fixture and our re-encode don't
// matter — only semantic equivalence does).
testing::AssertionResult JsonSemanticEq(std::string_view a,
                                        std::string_view b) {
  auto va = base::JSONReader::Read(a);
  auto vb = base::JSONReader::Read(b);
  if (!va) return testing::AssertionFailure() << "lhs not parseable: " << a;
  if (!vb) return testing::AssertionFailure() << "rhs not parseable: " << b;
  if (*va != *vb) {
    return testing::AssertionFailure()
        << "JSON not semantically equal\n  lhs: " << a << "\n  rhs: " << b;
  }
  return testing::AssertionSuccess();
}

// ---------------------------------------------------------------------
// Tag <-> string
// ---------------------------------------------------------------------

TEST(CbWireEnvelopeTagTest, AllSixTagsRoundTrip) {
  for (auto type : {EnvelopeType::kOffer, EnvelopeType::kAnswer,
                    EnvelopeType::kIce, EnvelopeType::kBye,
                    EnvelopeType::kRequestRenegotiate,
                    EnvelopeType::kProbeResult}) {
    std::string_view tag = TagToString(type);
    EXPECT_THAT(TagFromString(tag), Optional(type)) << "tag=" << tag;
  }
}

TEST(CbWireEnvelopeTagTest, UnknownTagsRejected) {
  // The historic v0 tag — load-bearing rejection.
  EXPECT_EQ(TagFromString("sdp_offer"), std::nullopt);
  // Other plausible-but-wrong names.
  EXPECT_EQ(TagFromString(""), std::nullopt);
  EXPECT_EQ(TagFromString("OFFER"), std::nullopt);  // case-sensitive.
  EXPECT_EQ(TagFromString("candidate"), std::nullopt);
  EXPECT_EQ(TagFromString("hello"), std::nullopt);
  EXPECT_EQ(TagFromString("restart_ice"), std::nullopt);
  EXPECT_EQ(TagFromString("renegotiate"), std::nullopt);  // missing prefix.
}

TEST(CbWireEnvelopeRoleTest, BothRolesRoundTrip) {
  EXPECT_THAT(RoleFromString("browser"), Optional(PeerRole::kBrowser));
  EXPECT_THAT(RoleFromString("client"), Optional(PeerRole::kClient));
  EXPECT_EQ(std::string(RoleToString(PeerRole::kBrowser)), "browser");
  EXPECT_EQ(std::string(RoleToString(PeerRole::kClient)), "client");
}

TEST(CbWireEnvelopeRoleTest, UnknownRolesRejected) {
  EXPECT_EQ(RoleFromString("server"), std::nullopt);
  EXPECT_EQ(RoleFromString(""), std::nullopt);
  EXPECT_EQ(RoleFromString("BROWSER"), std::nullopt);
}

// ---------------------------------------------------------------------
// Decode of real fixtures
// ---------------------------------------------------------------------

TEST(CbWireEnvelopeDecodeTest, RealOfferDecodes) {
  auto env = Decode(kRealOfferEnvelope);
  ASSERT_TRUE(env);
  EXPECT_EQ(env->type, EnvelopeType::kOffer);
  EXPECT_EQ(env->from, PeerRole::kBrowser);
  const auto* sdp = std::get_if<SdpPayload>(&env->data);
  ASSERT_TRUE(sdp);
  EXPECT_EQ(sdp->sdp_type, "offer");
  // Must include the BUNDLE / VP9 markers from the captured SDP.
  EXPECT_NE(sdp->sdp.find("a=group:BUNDLE 0"), std::string::npos);
  EXPECT_NE(sdp->sdp.find("VP9/90000"), std::string::npos);
}

TEST(CbWireEnvelopeDecodeTest, RealIceDecodes) {
  auto env = Decode(kRealIceEnvelope);
  ASSERT_TRUE(env);
  EXPECT_EQ(env->type, EnvelopeType::kIce);
  const auto* ice = std::get_if<IceCandidatePayload>(&env->data);
  ASSERT_TRUE(ice);
  EXPECT_FALSE(ice->is_end_of_candidates);
  EXPECT_NE(ice->candidate.find("typ srflx"), std::string::npos);
  EXPECT_THAT(ice->sdp_mid, Optional(std::string("0")));
  EXPECT_THAT(ice->sdp_m_line_index, Optional(0));
  EXPECT_THAT(ice->username_fragment, Optional(std::string("4ZcD")));
}

TEST(CbWireEnvelopeDecodeTest, RealIceEndOfCandidatesDecodes) {
  auto env = Decode(kRealIceEndOfCandidatesEnvelope);
  ASSERT_TRUE(env);
  EXPECT_EQ(env->type, EnvelopeType::kIce);
  const auto* ice = std::get_if<IceCandidatePayload>(&env->data);
  ASSERT_TRUE(ice);
  EXPECT_TRUE(ice->is_end_of_candidates);
  EXPECT_EQ(ice->candidate, "");
}

TEST(CbWireEnvelopeDecodeTest, RealByeDecodes) {
  auto env = Decode(kRealByeEnvelope);
  ASSERT_TRUE(env);
  EXPECT_EQ(env->type, EnvelopeType::kBye);
  EXPECT_EQ(env->from, PeerRole::kBrowser);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(env->data));
}

TEST(CbWireEnvelopeDecodeTest, RealRequestRenegotiateDecodes) {
  auto env = Decode(kRealRequestRenegotiateEnvelope);
  ASSERT_TRUE(env);
  EXPECT_EQ(env->type, EnvelopeType::kRequestRenegotiate);
  EXPECT_EQ(env->from, PeerRole::kClient);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(env->data));
}

TEST(CbWireEnvelopeDecodeTest, RealProbeResultDecodes) {
  auto env = Decode(kRealProbeResultEnvelope);
  ASSERT_TRUE(env);
  EXPECT_EQ(env->type, EnvelopeType::kProbeResult);
  const auto* probe = std::get_if<ProbeResultPayload>(&env->data);
  ASSERT_TRUE(probe);
  EXPECT_THAT(probe->raw.FindInt("down_kbps"), Optional(4500));
  EXPECT_THAT(probe->raw.FindInt("up_kbps"), Optional(1200));
  EXPECT_THAT(probe->raw.FindInt("rtt_ms"), Optional(38));
}

// ---------------------------------------------------------------------
// Negative test — load-bearing rejection
// ---------------------------------------------------------------------

TEST(CbWireEnvelopeDecodeTest, RejectsHistoricSdpOfferTag) {
  // Locks the source-pin contract. If a maintainer ever "fixes" the
  // codec to accept sdp_offer as an alias for offer, this test fires
  // and they have to argue the contract change explicitly.
  EXPECT_EQ(Decode(kRejectedSdpOfferEnvelope), std::nullopt);
}

TEST(CbWireEnvelopeDecodeTest, RejectsNonJson) {
  EXPECT_EQ(Decode("not json"), std::nullopt);
  EXPECT_EQ(Decode(""), std::nullopt);
  EXPECT_EQ(Decode("123"), std::nullopt);  // valid JSON, wrong shape.
  EXPECT_EQ(Decode("[]"), std::nullopt);   // valid JSON, wrong shape.
}

TEST(CbWireEnvelopeDecodeTest, RejectsMissingFields) {
  EXPECT_EQ(Decode(R"({"from":"browser"})"), std::nullopt);
  EXPECT_EQ(Decode(R"({"type":"offer"})"), std::nullopt);
  EXPECT_EQ(Decode(R"({"type":"offer","from":"server","data":null})"),
            std::nullopt);
}

TEST(CbWireEnvelopeDecodeTest, RejectsByeWithDataField) {
  // bye carries NO data field on the wire — even null is a violation.
  EXPECT_EQ(Decode(R"({"type":"bye","from":"browser","data":null})"),
            std::nullopt);
}

TEST(CbWireEnvelopeDecodeTest, RejectsOfferWithWrongInnerType) {
  // data.type must equal the envelope tag — see SdpPayload contract.
  EXPECT_EQ(Decode(R"({"type":"offer","from":"browser",
                       "data":{"type":"answer","sdp":"v=0\r\n"}})"),
            std::nullopt);
}

// ---------------------------------------------------------------------
// Round-trip — decode the real fixture, re-encode, semantic equality
// ---------------------------------------------------------------------

TEST(CbWireEnvelopeRoundTripTest, RealOffer) {
  auto env = Decode(kRealOfferEnvelope);
  ASSERT_TRUE(env);
  auto out = Encode(*env);
  ASSERT_TRUE(out);
  EXPECT_TRUE(JsonSemanticEq(*out, kRealOfferEnvelope));
}

TEST(CbWireEnvelopeRoundTripTest, RealIce) {
  auto env = Decode(kRealIceEnvelope);
  ASSERT_TRUE(env);
  auto out = Encode(*env);
  ASSERT_TRUE(out);
  EXPECT_TRUE(JsonSemanticEq(*out, kRealIceEnvelope));
}

TEST(CbWireEnvelopeRoundTripTest, RealIceEndOfCandidates) {
  auto env = Decode(kRealIceEndOfCandidatesEnvelope);
  ASSERT_TRUE(env);
  auto out = Encode(*env);
  ASSERT_TRUE(out);
  EXPECT_TRUE(JsonSemanticEq(*out, kRealIceEndOfCandidatesEnvelope));
}

TEST(CbWireEnvelopeRoundTripTest, RealBye) {
  auto env = Decode(kRealByeEnvelope);
  ASSERT_TRUE(env);
  auto out = Encode(*env);
  ASSERT_TRUE(out);
  // Confirm the absence of `data` survives the round-trip — this is
  // the part that's easy to break (Encode emitting `"data":null` for
  // a monostate kBye would be a contract violation physics would
  // reject).
  EXPECT_TRUE(JsonSemanticEq(*out, kRealByeEnvelope));
  EXPECT_EQ(out->find("\"data\""), std::string::npos);
}

TEST(CbWireEnvelopeRoundTripTest, RealRequestRenegotiate) {
  auto env = Decode(kRealRequestRenegotiateEnvelope);
  ASSERT_TRUE(env);
  auto out = Encode(*env);
  ASSERT_TRUE(out);
  EXPECT_TRUE(JsonSemanticEq(*out, kRealRequestRenegotiateEnvelope));
}

TEST(CbWireEnvelopeRoundTripTest, RealProbeResult) {
  auto env = Decode(kRealProbeResultEnvelope);
  ASSERT_TRUE(env);
  auto out = Encode(*env);
  ASSERT_TRUE(out);
  EXPECT_TRUE(JsonSemanticEq(*out, kRealProbeResultEnvelope));
}

// ---------------------------------------------------------------------
// Encode-side invariants
// ---------------------------------------------------------------------

TEST(CbWireEnvelopeEncodeTest, ByeOmitsDataField) {
  Envelope env{EnvelopeType::kBye, PeerRole::kBrowser,
               EnvelopeData{std::monostate{}}};
  auto out = Encode(env);
  ASSERT_TRUE(out);
  EXPECT_EQ(out->find("\"data\""), std::string::npos);
}

TEST(CbWireEnvelopeEncodeTest, RequestRenegotiateEmitsNullData) {
  Envelope env{EnvelopeType::kRequestRenegotiate, PeerRole::kBrowser,
               EnvelopeData{std::monostate{}}};
  auto out = Encode(env);
  ASSERT_TRUE(out);
  // Should contain `"data":null` literally — request_renegotiate's
  // payload is null on the wire, not omitted.
  EXPECT_NE(out->find("\"data\":null"), std::string::npos);
}

TEST(CbWireEnvelopeEncodeTest, MakeIceEndOfCandidatesHelper) {
  Envelope env = MakeIceEndOfCandidates(PeerRole::kBrowser);
  EXPECT_EQ(env.type, EnvelopeType::kIce);
  EXPECT_EQ(env.from, PeerRole::kBrowser);
  auto out = Encode(env);
  ASSERT_TRUE(out);
  EXPECT_NE(out->find("\"data\":null"), std::string::npos);
  // Round-trips to the captured EOC fixture.
  EXPECT_TRUE(JsonSemanticEq(*out, kRealIceEndOfCandidatesEnvelope));
}

TEST(CbWireEnvelopeEncodeTest, ActiveVariantMustMatchTag) {
  // Programming-bug case: kOffer with an IceCandidatePayload variant.
  // We surface this as Encode returning nullopt — not a CHECK, so
  // tests can exercise it without crashing the process.
  Envelope wrong{EnvelopeType::kOffer, PeerRole::kBrowser,
                 EnvelopeData{IceCandidatePayload{}}};
  EXPECT_EQ(Encode(wrong), std::nullopt);
}

}  // namespace
}  // namespace signaling
}  // namespace cloud_browser

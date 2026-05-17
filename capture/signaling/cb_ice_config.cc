// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Implementation of M3 R3 (CV2-53) — ICE-server config loader.
//
// Shape matches the legacy streamer.js parser at
// capture/streamer-page/streamer.js:126-178; keep the two in
// behavioural sync until streamer.js is deleted by the M0 R3
// streamer-page-absence assertion landing on the integration branch.

#include "capture/signaling/cb_ice_config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "base/values.h"

namespace cloud_browser::signaling {

namespace {

// Env var names — kept in one place so the embedder + tests + this
// file's TODO comments agree.
constexpr char kEnvIceServers[] = "WEBRTC_ICE_SERVERS";
constexpr char kEnvIceTransportPolicy[] = "WEBRTC_ICE_TRANSPORT_POLICY";

// streamer.js's DEFAULT_ICE_SERVERS literal — `[{ urls: ["stun:..."] }]`.
constexpr char kDefaultStunUrl[] = "stun:stun.l.google.com:19302";

// Read a raw env var via getenv(3). Returns the empty string when
// the var is unset; treats an unset var and an explicitly empty
// var as equivalent (matches streamer.js, which checks `if (!raw)`).
//
// TODO(M3-R3-env-source): swap for base::Environment::Create() ->
// GetVar(name, &value) — see header.
std::string GetEnv(const char* name) {
  const char* v = std::getenv(name);
  return v == nullptr ? std::string() : std::string(v);
}

// ASCII lowercase — used for case-insensitive transport-policy
// matching. Locale-independent on purpose (env values are wire-
// shape strings, not user-visible text).
std::string AsciiLower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(
        (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c);
  }
  return out;
}

// Append a single normalised IceServer to |out| if at least one
// non-empty url survived. Mirrors streamer.js's per-entry filter:
//   urls : string OR non-empty array of non-empty strings → keep;
//   anything else → drop the entry.
// username / credential : copied verbatim when present + non-empty;
//   absent / empty → leave the IceServer field at its default
//   (empty std::string). webrtc treats empty username/credential as
//   "no credentials"; this is the same semantics as streamer.js's
//   conditional copy.
void TryAppendServer(const base::Value::Dict& entry,
                     std::vector<webrtc::PeerConnectionInterface::IceServer>*
                         out) {
  webrtc::PeerConnectionInterface::IceServer server;

  const base::Value* urls_value = entry.Find("urls");
  if (!urls_value) {
    return;
  }
  if (const std::string* single = urls_value->GetIfString()) {
    if (single->empty()) {
      return;
    }
    server.urls.push_back(*single);
  } else if (const base::Value::List* list = urls_value->GetIfList()) {
    for (const auto& v : *list) {
      const std::string* s = v.GetIfString();
      if (s && !s->empty()) {
        server.urls.push_back(*s);
      }
    }
    if (server.urls.empty()) {
      return;
    }
  } else {
    return;
  }

  if (const std::string* username = entry.FindString("username");
      username && !username->empty()) {
    server.username = *username;
  }
  // streamer.js calls the field `credential` on the JSON; webrtc's
  // IceServer struct calls it `password`. Map between the two so the
  // env JSON shape stays compatible with the legacy parser.
  if (const std::string* credential = entry.FindString("credential");
      credential && !credential->empty()) {
    server.password = *credential;
  }

  out->push_back(std::move(server));
}

}  // namespace

std::optional<std::vector<webrtc::PeerConnectionInterface::IceServer>>
ParseIceServersJson(std::string_view raw_json) {
  if (raw_json.empty()) {
    return std::nullopt;
  }

  std::optional<base::Value> parsed =
      base::JSONReader::Read(raw_json,
                             base::JSON_PARSE_RFC);
  if (!parsed.has_value()) {
    return std::nullopt;
  }

  // Accept either the bare array form `[...]` or the wrapped form
  // `{"iceServers": [...]}` — streamer.js accepts both.
  const base::Value::List* candidate = nullptr;
  if (parsed->is_list()) {
    candidate = &parsed->GetList();
  } else if (parsed->is_dict()) {
    candidate = parsed->GetDict().FindList("iceServers");
  }
  if (candidate == nullptr) {
    return std::nullopt;
  }

  std::vector<webrtc::PeerConnectionInterface::IceServer> servers;
  servers.reserve(candidate->size());
  for (const auto& entry : *candidate) {
    const base::Value::Dict* dict = entry.GetIfDict();
    if (dict == nullptr) {
      continue;
    }
    TryAppendServer(*dict, &servers);
  }

  if (servers.empty()) {
    return std::nullopt;
  }
  return servers;
}

webrtc::PeerConnectionInterface::IceTransportsType ParseIceTransportPolicy(
    std::string_view raw) {
  const std::string lower = AsciiLower(raw);
  if (lower == "relay") {
    return webrtc::PeerConnectionInterface::IceTransportsType::kRelay;
  }
  return webrtc::PeerConnectionInterface::IceTransportsType::kAll;
}

std::vector<webrtc::PeerConnectionInterface::IceServer>
BuildDefaultIceServers() {
  webrtc::PeerConnectionInterface::IceServer stun;
  stun.urls.emplace_back(kDefaultStunUrl);
  return {std::move(stun)};
}

IceConfig::Summary SummariseIceServers(
    const std::vector<webrtc::PeerConnectionInterface::IceServer>& servers) {
  IceConfig::Summary out;
  for (const auto& server : servers) {
    for (const std::string& url : server.urls) {
      // Cheap prefix match — no scheme parse needed for the
      // observability counter; matches streamer.js's
      // iceServerSummary().
      if (url.rfind("stun:", 0) == 0 || url.rfind("stuns:", 0) == 0) {
        ++out.stun;
      } else if (url.rfind("turn:", 0) == 0 || url.rfind("turns:", 0) == 0) {
        ++out.turn;
      } else {
        ++out.other;
      }
    }
  }
  return out;
}

// CV2-69 RENAME (was `LoadConfigFromEnv`): see cb_ice_config.h for rationale.
std::optional<IceConfig> LoadIceConfigFromEnv() {
  IceConfig config;

  // Servers: env JSON → parser → fallback.
  std::optional<std::vector<webrtc::PeerConnectionInterface::IceServer>>
      parsed = ParseIceServersJson(GetEnv(kEnvIceServers));
  config.servers = parsed.has_value() ? std::move(*parsed)
                                      : BuildDefaultIceServers();

  // Transport policy: env string → enum.
  config.transport_policy =
      ParseIceTransportPolicy(GetEnv(kEnvIceTransportPolicy));

  config.summary = SummariseIceServers(config.servers);
  return config;
}

}  // namespace cloud_browser::signaling

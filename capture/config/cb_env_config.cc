// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cb_env_config.cc — see cb_env_config.h.

#include "capture/config/cb_env_config.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace cloud_browser {
namespace config {

namespace {

// Single point where getenv(3) is called, so the empty-is-absent rule
// cannot drift between the three public entry points.
//
// Deliberately std::getenv rather than base::Environment::Create() ->
// GetVar(). cb_signaling_ws_client.h carries a TODO
// (M3-R2-env-source) suggesting the base:: idiom is preferable in the
// browser process, and it may well be — but switching is a behaviour
// question (base::Environment reads a snapshot on some platforms), and
// making it here would change the semantics of three call sites while
// they are being consolidated. Consolidate first, then that TODO can be
// resolved in ONE place instead of four.
const char* RawEnv(const char* name) {
  if (name == nullptr) return nullptr;
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') return nullptr;
  return raw;
}

}  // namespace

std::string GetEnvOr(const char* name, const char* fallback) {
  const char* raw = RawEnv(name);
  if (raw == nullptr) return fallback == nullptr ? std::string() : fallback;
  return std::string(raw);
}

std::optional<std::string> GetEnv(const char* name) {
  const char* raw = RawEnv(name);
  if (raw == nullptr) return std::nullopt;
  return std::string(raw);
}

bool GetEnvBool(const char* name, bool fallback) {
  const char* raw = RawEnv(name);
  if (raw == nullptr) return fallback;

  std::string_view v(raw);
  // Case-insensitive compare without pulling in a locale-aware helper;
  // the accepted set is ASCII by construction.
  auto eq = [v](std::string_view lit) {
    if (v.size() != lit.size()) return false;
    for (size_t i = 0; i < v.size(); ++i) {
      char c = v[i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (c != lit[i]) return false;
    }
    return true;
  };

  if (eq("0") || eq("false") || eq("no") || eq("off")) return false;
  return true;
}

}  // namespace config
}  // namespace cloud_browser

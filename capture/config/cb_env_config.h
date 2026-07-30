// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cb_env_config.h — the embedder's environment-variable surface, in one
// place.
//
// # Why this exists
//
// Not because config was untyped — it already is typed. `WsClientConfig`
// (cb_signaling_ws_client.h) and `IceConfig` (cb_ice_config.h) are proper
// structs with documented fields and their own `LoadConfigFromEnv()`, and
// that pattern is good. Two narrower problems:
//
//   1. `EnvOrDefault()` is copy-pasted, byte-identical, into three files —
//      cb_clipboard_relay.cc, cb_stats_relay.cc, cb_file_upload_relay.cc.
//      Three copies of four lines is not a crisis, but it is three places
//      to fix when the semantics need to change, and the semantics ARE
//      subtle: empty-string must be treated as unset (see below).
//
//   2. There was no single place to answer "what does this binary read
//      from the environment?" — you had to grep, and grep does not tell
//      you the defaults or which ones are load-bearing. For an OSS
//      consumer standing up their own deployment that is the first
//      question they have, and the answer lived in six files.
//
// This header is deliberately NOT a config framework. No registry, no
// validation DSL, no central struct that every subsystem must register
// with. Those would be a bigger change than the problem justifies, and
// they would fight the existing per-subsystem structs, which are fine.
// It is: one shared reader, and a documented inventory.
//
// # The empty-string rule
//
// `GetEnvOr(name, fallback)` treats an env var that is SET BUT EMPTY as
// absent, returning the fallback. This matches all three original copies
// and is the behaviour the deployment actually depends on: k8s renders an
// unset ConfigMap key as the empty string, so `""` reaching a URL parser
// means "operator did not configure this", not "operator wants an empty
// URL". Changing this to distinguish the two would silently break every
// pod whose ConfigMap omits an optional key.
//
// # Threading
//
// getenv(3) is not thread-safe against setenv(3). Nothing in this binary
// calls setenv after startup, and all callers here read during
// construction on the UI thread, so this is safe in practice. It is
// documented rather than enforced because enforcing it (a mutex, a
// one-shot snapshot) would imply a guarantee this header cannot actually
// make about the whole process.

#ifndef CAPTURE_CONFIG_CB_ENV_CONFIG_H_
#define CAPTURE_CONFIG_CB_ENV_CONFIG_H_

#include <optional>
#include <string>

namespace cloud_browser {
namespace config {

// Returns the value of `name`, or `fallback` when the variable is unset
// OR set to the empty string. See "The empty-string rule" above — the
// empty case is load-bearing, not an accident.
std::string GetEnvOr(const char* name, const char* fallback);

// Returns the value of `name`, or nullopt when unset or empty. For
// callers that must distinguish "not configured" from "configured to
// something" rather than falling back to a default — the signaling
// loaders need this shape (a missing host means "skip the subsystem",
// not "use a default host").
std::optional<std::string> GetEnv(const char* name);

// Boolean env var. "0", "false", "no", "off" (case-insensitive) are
// false; any other non-empty value is true; unset/empty yields
// `fallback`.
//
// The permissive true-case is deliberate: an operator who writes
// `CHROMELESS_FOO=1`, `=true`, `=yes` or `=on` means the same thing, and
// a strict parser that accepted only one spelling would fail silently in
// the direction of "feature quietly off" — the worst failure mode for a
// flag.
bool GetEnvBool(const char* name, bool fallback);

}  // namespace config
}  // namespace cloud_browser

#endif  // CAPTURE_CONFIG_CB_ENV_CONFIG_H_

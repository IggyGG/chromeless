// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Tests for the shared env reader. The empty-string rule is the reason
// this file exists: it is the one behaviour a future refactor is most
// likely to "clean up" without realising it is load-bearing.

#include "capture/config/cb_env_config.h"

#include <cstdlib>
#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace cloud_browser {
namespace config {
namespace {

// RAII env var, so a failing assertion cannot leak state into the next
// test in the binary.
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    const char* prev = std::getenv(name);
    had_prev_ = prev != nullptr;
    if (had_prev_) prev_ = prev;
    if (value == nullptr) {
      unsetenv(name);
    } else {
      setenv(name, value, /*overwrite=*/1);
    }
  }
  ~ScopedEnv() {
    if (had_prev_) {
      setenv(name_, prev_.c_str(), /*overwrite=*/1);
    } else {
      unsetenv(name_);
    }
  }

 private:
  const char* name_;
  bool had_prev_ = false;
  std::string prev_;
};

constexpr char kVar[] = "CB_ENV_CONFIG_TEST_VAR";

TEST(CbEnvConfigTest, GetEnvOrReturnsValueWhenSet) {
  ScopedEnv e(kVar, "hello");
  EXPECT_EQ(GetEnvOr(kVar, "fallback"), "hello");
}

TEST(CbEnvConfigTest, GetEnvOrReturnsFallbackWhenUnset) {
  ScopedEnv e(kVar, nullptr);
  EXPECT_EQ(GetEnvOr(kVar, "fallback"), "fallback");
}

TEST(CbEnvConfigTest, GetEnvOrTreatsEmptyAsUnset) {
  // THE load-bearing case. k8s renders an unset ConfigMap key as "", so
  // an empty value means "operator did not configure this" — not
  // "operator wants an empty URL". All three original copies of this
  // helper behaved this way; a refactor that "fixed" it would silently
  // hand empty strings to URL parsers in every pod with an optional key
  // omitted.
  ScopedEnv e(kVar, "");
  EXPECT_EQ(GetEnvOr(kVar, "fallback"), "fallback");
}

TEST(CbEnvConfigTest, GetEnvDistinguishesUnsetFromValue) {
  {
    ScopedEnv e(kVar, "x");
    EXPECT_EQ(GetEnv(kVar), std::optional<std::string>("x"));
  }
  {
    ScopedEnv e(kVar, nullptr);
    EXPECT_EQ(GetEnv(kVar), std::nullopt);
  }
  {
    ScopedEnv e(kVar, "");
    EXPECT_EQ(GetEnv(kVar), std::nullopt);  // same rule as GetEnvOr.
  }
}

TEST(CbEnvConfigTest, GetEnvBoolFalseSpellings) {
  for (const char* v : {"0", "false", "FALSE", "False", "no", "NO", "off", "OFF"}) {
    ScopedEnv e(kVar, v);
    EXPECT_FALSE(GetEnvBool(kVar, true)) << "value: " << v;
  }
}

TEST(CbEnvConfigTest, GetEnvBoolTrueSpellings) {
  // Permissive on purpose: an operator writing 1 / true / yes / on means
  // the same thing, and a parser accepting only one spelling fails in
  // the direction of "feature quietly off".
  for (const char* v : {"1", "true", "TRUE", "yes", "on", "enabled", "anything"}) {
    ScopedEnv e(kVar, v);
    EXPECT_TRUE(GetEnvBool(kVar, false)) << "value: " << v;
  }
}

TEST(CbEnvConfigTest, GetEnvBoolFallsBackWhenUnsetOrEmpty) {
  {
    ScopedEnv e(kVar, nullptr);
    EXPECT_TRUE(GetEnvBool(kVar, true));
    EXPECT_FALSE(GetEnvBool(kVar, false));
  }
  {
    ScopedEnv e(kVar, "");
    EXPECT_TRUE(GetEnvBool(kVar, true));
    EXPECT_FALSE(GetEnvBool(kVar, false));
  }
}

TEST(CbEnvConfigTest, NullNameIsSafe) {
  // Defensive: a null name would be a programming error, but crashing
  // the browser process over one is worse than returning the fallback.
  EXPECT_EQ(GetEnvOr(nullptr, "fallback"), "fallback");
  EXPECT_EQ(GetEnv(nullptr), std::nullopt);
  EXPECT_TRUE(GetEnvBool(nullptr, true));
}

}  // namespace
}  // namespace config
}  // namespace cloud_browser

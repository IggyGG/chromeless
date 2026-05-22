// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserMainDelegate — see cloud_browser_main.h.

#include "capture/build-integration/cloud_browser_main.h"

#include <memory>
#include <optional>
#include <variant>

#include "base/logging.h"
#include "capture/build-integration/content_browser_client.h"
#include "components/crash/core/common/crash_key.h"
#include "content/public/app/initialize_mojo_core.h"
#include "ui/base/resource/resource_bundle.h"

namespace cloud_browser {

CloudBrowserMainDelegate::CloudBrowserMainDelegate() = default;

CloudBrowserMainDelegate::~CloudBrowserMainDelegate() = default;

content::ContentBrowserClient*
CloudBrowserMainDelegate::CreateContentBrowserClient() {
  // ContentMain calls this exactly once on the browser process and
  // expects the returned pointer to remain valid for the lifetime of
  // the runner. We hold the instance in browser_client_ and hand back
  // the raw pointer, mirroring content_shell + headless.
  browser_client_ = std::make_unique<CloudBrowserContentBrowserClient>();
  return browser_client_.get();
}

void CloudBrowserMainDelegate::PreSandboxStartup() {
  // ============== CV2-69 (M55-R5-merge-with-m3-r4-r6) F1 + F3 ==============
  //
  // PreSandboxStartup is chromium's canonical pre-sandbox embedder init
  // site. Anything that touches files (pak loading, log paths,
  // crashpad uploader-process exec) must happen BEFORE the sandbox
  // locks the process down. Both reference embedders override here.

  // ---- F1: ResourceBundle init -----------------------------------------
  // Without this, GPU code path triggers
  // `Check failed: g_shared_instance_ != nullptr.` (ui/base/resource/
  // resource_bundle.cc:384) at +97s — observed by gpu-test-lead on
  // Phase C with --use-gl=angle --use-angle=gl. SOFTWARE path
  // (swiftshader) bypasses the GL paths that touch the ResourceBundle
  // and survives indefinitely.
  //
  // Minimum-viable init: locale-only, no pak file shipped. If a
  // future build wants real localized strings, the upgrade path is
  // to ship cloud_browser.pak (built via GN `repack` template, see
  // content/shell/BUILD.gn:repack_locale for pattern) and switch to
  // InitSharedInstanceWithPakPath. Out of scope for CV2-69.
  ui::ResourceBundle::InitSharedInstanceWithLocale(
      "en-US",
      /*delegate=*/nullptr,
      ui::ResourceBundle::DO_NOT_LOAD_COMMON_RESOURCES);

  // ---- F3: Crash-key string-table init ---------------------------------
  // Without this, chromium's SET_CRASH_KEY_VALUE call sites crash the
  // process — strewn throughout the GPU + network subsystems. Not
  // currently tripped (the +97s SIGABRT predates any crash-key use)
  // but parity-match with shell + headless future-proofs the next
  // chromium-subsystem expansion. Full Crashpad bring-up (uploader +
  // CrashReporterClient subclass + upload-endpoint env contract)
  // is deferred to a follow-up R# that owns ops-grade crash
  // reporting.
  crash_reporter::InitializeCrashKeys();
}

std::optional<int> CloudBrowserMainDelegate::PostEarlyInitialization(
    InvokedIn invoked_in) {
  // ============== CV2-69 F2 ==============
  //
  // PostEarlyInitialization runs after content has done its own first-
  // pass setup but before the message loop begins. Both reference
  // embedders call content::InitializeMojoCore() here for the browser
  // process only.
  //
  // cb-chromium's mojo paths work implicitly today (DevTools + viz
  // compositor are functional pre-fix), but explicit init is the
  // parity-match with shell + headless and future-proofs against
  // subtle mojo-ordering bugs under restricted PodSecurity. Cheap
  // and side-effect-light.
  if (std::holds_alternative<InvokedInBrowserProcess>(invoked_in)) {
    content::InitializeMojoCore();
  }
  return std::nullopt;
}

}  // namespace cloud_browser

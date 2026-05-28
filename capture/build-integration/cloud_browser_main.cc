// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserMainDelegate — see cloud_browser_main.h.

#include "capture/build-integration/cloud_browser_main.h"

#include <memory>
#include <optional>
#include <variant>

#include "base/check.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "capture/build-integration/cloud_browser_content_client.h"
#include "capture/build-integration/content_browser_client.h"
#include "components/crash/core/common/crash_key.h"
#include "content/public/app/initialize_mojo_core.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/resource/resource_scale_factor.h"

namespace {

base::FilePath RequiredResourcePak(const base::FilePath& resource_dir,
                                   const base::FilePath::CharType* name) {
  base::FilePath path = resource_dir.Append(name);
  CHECK(base::PathExists(path))
      << "required Chromium resource pack missing: " << path.value();
  return path;
}

}  // namespace

namespace cloud_browser {

CloudBrowserMainDelegate::CloudBrowserMainDelegate() = default;

CloudBrowserMainDelegate::~CloudBrowserMainDelegate() = default;

content::ContentClient* CloudBrowserMainDelegate::CreateContentClient() {
  // ContentMain calls this on EVERY process, very early (before
  // PreSandboxStartup) and keeps the returned pointer for the run's
  // lifetime. content_client_ is a value member, so the pointer is stable.
  // Delegating to ResourceBundle is safe even though this fires pre-sandbox
  // and pre-pak-load: ContentClient only *reads* ResourceBundle lazily, at
  // GetDataResource call time, which is after PreSandboxStartup has loaded
  // the pak. Without this bridge the renderer's Blink resource fetch hits
  // the empty base ContentClient and DCHECKs in css_default_style_sheets.cc.
  // Mirrors HeadlessContentMainDelegate::CreateContentClient.
  return &content_client_;
}

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
  // CV2-89 follow-up: SwiftShader made the renderer path live, and
  // that path reaches Blink's default stylesheet resources. Locale-only
  // ResourceBundle init is no longer enough; without the Chromium
  // resource packs, Blink can DCHECK while constructing the default SVG
  // stylesheet (css_default_style_sheets.cc).
  base::FilePath resource_dir;
  CHECK(base::PathService::Get(base::DIR_ASSETS, &resource_dir))
      << "base::DIR_ASSETS unavailable";

  ui::ResourceBundle::InitSharedInstanceWithPakPath(RequiredResourcePak(
      resource_dir, FILE_PATH_LITERAL("headless_lib_strings.pak")));
  // headless_lib_data.pak is the headless build's scale-INDEPENDENT data
  // pack (the //headless:pak repack of headless_lib.grd + blink/content
  // resources) — the headless equivalent of stock Chromium's
  // `resources.pak`. It carries the Blink UA stylesheets, including
  // IDR_UASTYLE_SVG_CSS. Those are looked up via the kScaleFactorNone
  // bucket (e.g. Blink's CSSDefaultStyleSheets default-SVG-stylesheet
  // construction at css_default_style_sheets.cc), so the pack MUST be
  // registered at kScaleFactorNone. The prior cv2-89 revision added it at
  // k100Percent (carried over from the chrome_100_percent.pak line it
  // replaced); a k100Percent pack does not answer scale-none lookups, so
  // IDR_UASTYLE_SVG_CSS resolved empty and Blink hit a FATAL DCHECK
  // (default_svg_style_->UniversalRules().size() == 1u, 0 vs 1) on the
  // first SVG layout. kScaleFactorNone matches how the original
  // resources.pak load and upstream headless register this pack.
  ui::ResourceBundle::GetSharedInstance().AddDataPackFromPath(
      RequiredResourcePak(resource_dir, FILE_PATH_LITERAL("headless_lib_data.pak")),
      ui::kScaleFactorNone);

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

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cloud_browser_worker — Linux entry point for the Phase-2 worker
// binary. Mirrors the non-Windows/non-iOS branch of
// content/shell/app/shell_main.cc: construct the main delegate,
// populate ContentMainParams with argc/argv, and hand off to
// content::ContentMain.
//
// Linux-only by design (BUILD.gn asserts !is_linux is unsupported).
// No sandbox_info plumbing here — on Linux, content's zygote/setuid
// sandbox is initialised transitively through //content/public/app:*
// (pulled in by the :embedder source_set). There is no
// sandbox_helper_linux target in chromium; only Windows ships one.
//
// Cross-references:
//   * content/shell/app/shell_main.cc       (template)
//   * headless/app/headless_shell_main.cc   (template)

#include <utility>

#include "capture/build-integration/cloud_browser_main.h"
#include "content/public/app/content_main.h"

int main(int argc, const char** argv) {
  cloud_browser::CloudBrowserMainDelegate delegate;
  content::ContentMainParams params(&delegate);
  params.argc = argc;
  params.argv = argv;
  return content::ContentMain(std::move(params));
}

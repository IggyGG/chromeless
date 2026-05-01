// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserMainDelegate — see cloud_browser_main.h.

#include "capture/build-integration/cloud_browser_main.h"

#include <memory>

#include "capture/build-integration/content_browser_client.h"

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

}  // namespace cloud_browser

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserContentClient — see cloud_browser_content_client.h.

#include "capture/build-integration/cloud_browser_content_client.h"

#include <string>
#include <string_view>

#include "base/logging.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/gfx/image/image.h"

namespace cloud_browser {

CloudBrowserContentClient::CloudBrowserContentClient() = default;

CloudBrowserContentClient::~CloudBrowserContentClient() = default;

std::string_view CloudBrowserContentClient::GetDataResource(
    int resource_id,
    ui::ResourceScaleFactor scale_factor) {
  return ui::ResourceBundle::GetSharedInstance().GetRawDataResourceForScale(
      resource_id, scale_factor);
}

base::RefCountedMemory* CloudBrowserContentClient::GetDataResourceBytes(
    int resource_id) {
  return ui::ResourceBundle::GetSharedInstance().LoadDataResourceBytes(
      resource_id);
}

std::string CloudBrowserContentClient::GetDataResourceString(int resource_id) {
  std::string data =
      ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
          resource_id);
  // CV2 resource-path provenance probe (2026-07-02). Blink builds its UA
  // stylesheets (svg/mathml/media-controls) through this exact call from
  // the RENDERER process; an empty return is what fed the FATAL
  // `css_default_style_sheets.cc:185` DCHECK class. Deployed binaries
  // repeatedly turned out to be stale (pre-ContentClient) despite fixed
  // source, so make the runtime truth observable on the guest serial
  // console: log the first CB_UA_RESOURCE line per process unconditionally,
  // and ALWAYS log when a lookup resolves empty. One-line cost per
  // process; empty hits are the bug signal and must never be silent.
  static bool logged_first = false;
  if (!logged_first || data.empty()) {
    logged_first = true;
    LOG(INFO) << "CB_UA_RESOURCE: GetDataResourceString id=" << resource_id
              << " bytes=" << data.size()
              << (data.empty() ? " EMPTY (UA-sheet DCHECK precursor)" : "");
  }
  return data;
}

gfx::Image& CloudBrowserContentClient::GetNativeImageNamed(int resource_id) {
  return ui::ResourceBundle::GetSharedInstance().GetNativeImageNamed(
      resource_id);
}

}  // namespace cloud_browser

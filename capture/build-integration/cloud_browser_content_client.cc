// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CloudBrowserContentClient — see cloud_browser_content_client.h.

#include "capture/build-integration/cloud_browser_content_client.h"

#include <string>
#include <string_view>

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
  return ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
      resource_id);
}

gfx::Image& CloudBrowserContentClient::GetNativeImageNamed(int resource_id) {
  return ui::ResourceBundle::GetSharedInstance().GetNativeImageNamed(
      resource_id);
}

}  // namespace cloud_browser

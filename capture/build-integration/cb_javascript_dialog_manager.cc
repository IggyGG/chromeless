// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "capture/build-integration/cb_javascript_dialog_manager.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "capture/build-integration/cb_control_channel.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

namespace cloud_browser {

namespace {

// How long a dialog waits for a human before applying its default. Long
// enough that someone can actually read and decide (a cookie banner or a
// delete-confirmation deserves more than a few seconds), short enough that
// a page cannot be wedged indefinitely by a viewer who walked away.
constexpr base::TimeDelta kDialogDeadline = base::Seconds(60);

// Cap on the message we forward. Page-controlled text: a hostile or buggy
// page can call alert() with megabytes of string, and that would otherwise
// ride the control channel verbatim. Truncation preserves the decision
// (the user still sees what they are answering) without letting the page
// choose our frame size.
constexpr size_t kMaxMessageChars = 4 * 1024;

std::string TruncateUtf8(const std::u16string& s) {
  if (s.size() <= kMaxMessageChars) {
    return base::UTF16ToUTF8(s);
  }
  return base::UTF16ToUTF8(s.substr(0, kMaxMessageChars)) + "…[truncated]";
}

const char* DialogTypeName(content::JavaScriptDialogType type) {
  switch (type) {
    case content::JAVASCRIPT_DIALOG_TYPE_ALERT:
      return "alert";
    case content::JAVASCRIPT_DIALOG_TYPE_CONFIRM:
      return "confirm";
    case content::JAVASCRIPT_DIALOG_TYPE_PROMPT:
      return "prompt";
  }
  return "unknown";
}

// The no-human default per dialog type. See the header for why
// beforeunload accepts rather than cancels.
bool DefaultAcceptFor(content::JavaScriptDialogType type) {
  switch (type) {
    case content::JAVASCRIPT_DIALOG_TYPE_ALERT:
      return true;  // no cancel branch exists; dismissing IS acknowledging
    case content::JAVASCRIPT_DIALOG_TYPE_CONFIRM:
    case content::JAVASCRIPT_DIALOG_TYPE_PROMPT:
      return false;  // the safe branch
  }
  return false;
}

std::string OriginOf(content::RenderFrameHost* rfh) {
  if (!rfh) {
    return std::string();
  }
  return rfh->GetLastCommittedOrigin().Serialize();
}

}  // namespace

CbJavaScriptDialogManager::CbJavaScriptDialogManager() = default;
CbJavaScriptDialogManager::~CbJavaScriptDialogManager() = default;

void CbJavaScriptDialogManager::SetControlChannel(CbControlChannel* channel) {
  control_channel_ = channel;
}

void CbJavaScriptDialogManager::AskOrDefault(
    const std::string& dialog_type_name,
    const std::u16string& message_text,
    const std::u16string& default_prompt_text,
    const std::string& origin,
    bool default_accept,
    DialogClosedCallback callback) {
  // No channel attached (pre-session, post-teardown, or headless
  // automation): apply the default immediately. This is the same outcome
  // chromium's no-delegate path produces, so nothing regresses for
  // unattended sessions.
  if (!control_channel_) {
    VLOG(1) << "CbJavaScriptDialogManager: " << dialog_type_name
            << " with no control channel — default accept=" << default_accept;
    std::move(callback).Run(default_accept, std::u16string());
    return;
  }

  base::DictValue payload;
  payload.Set("dialog_type", dialog_type_name);
  payload.Set("message", TruncateUtf8(message_text));
  payload.Set("default_prompt", TruncateUtf8(default_prompt_text));
  payload.Set("origin", origin);

  control_channel_->SendRequest(
      "js_dialog", std::move(payload), kDialogDeadline,
      base::BindOnce(
          [](DialogClosedCallback cb, bool default_accept,
             base::DictValue response) {
            // An empty dict is the "no answer" signal — closed channel,
            // timeout, or teardown. Fall back to the type's safe default
            // rather than to a permissive one.
            const std::optional<bool> accept = response.FindBool("accept");
            if (!accept) {
              std::move(cb).Run(default_accept, std::u16string());
              return;
            }
            // prompt_text is only meaningful on an accepted prompt;
            // harmless (and ignored by chromium) otherwise.
            const std::string* text = response.FindString("prompt_text");
            std::move(cb).Run(
                *accept, text ? base::UTF8ToUTF16(*text) : std::u16string());
          },
          std::move(callback), default_accept));
}

void CbJavaScriptDialogManager::RunJavaScriptDialog(
    content::WebContents* /*web_contents*/,
    content::RenderFrameHost* render_frame_host,
    content::JavaScriptDialogType dialog_type,
    const std::u16string& message_text,
    const std::u16string& default_prompt_text,
    DialogClosedCallback callback,
    bool* did_suppress_message) {
  // We are showing it (or deciding it), so nothing is suppressed. Leaving
  // this untouched would leave the caller reading uninitialised memory.
  if (did_suppress_message) {
    *did_suppress_message = false;
  }

  AskOrDefault(DialogTypeName(dialog_type), message_text, default_prompt_text,
               OriginOf(render_frame_host), DefaultAcceptFor(dialog_type),
               std::move(callback));
}

void CbJavaScriptDialogManager::RunBeforeUnloadDialog(
    content::WebContents* /*web_contents*/,
    content::RenderFrameHost* render_frame_host,
    bool is_reload,
    DialogClosedCallback callback) {
  // beforeunload carries no page-supplied message at this layer — chromium
  // stopped forwarding the page's custom string years ago (it was a
  // phishing vector), so the viewer renders its own copy. We pass the
  // reload bit so the overlay can say "reload" vs "leave".
  base::DictValue payload;
  payload.Set("is_reload", is_reload);

  if (!control_channel_) {
    VLOG(1) << "CbJavaScriptDialogManager: beforeunload with no control "
               "channel — allowing navigation";
    std::move(callback).Run(/*success=*/true, std::u16string());
    return;
  }

  payload.Set("dialog_type", "beforeunload");
  payload.Set("message", std::string());
  payload.Set("default_prompt", std::string());
  payload.Set("origin", OriginOf(render_frame_host));

  control_channel_->SendRequest(
      "js_dialog", std::move(payload), kDialogDeadline,
      base::BindOnce(
          [](DialogClosedCallback cb, base::DictValue response) {
            const std::optional<bool> accept = response.FindBool("accept");
            // Default true: see the header — refusing the navigation on an
            // unattended session strands the guest on a page it was told
            // to leave, and there is no unsaved human work to protect.
            std::move(cb).Run(accept.value_or(true), std::u16string());
          },
          std::move(callback)));
}

bool CbJavaScriptDialogManager::HandleJavaScriptDialog(
    content::WebContents* /*web_contents*/,
    bool /*accept*/,
    const std::u16string* /*prompt_override*/) {
  // Programmatic accept/dismiss (CDP's Page.handleJavaScriptDialog). Our
  // dialogs are not held in a native window we can reach back into — they
  // are in-flight control-channel requests keyed by id, and this API
  // carries no id. Returning false reports "not handled", which is honest;
  // the request still resolves via its own response/timeout path.
  //
  // A future CDP-driven-dialog feature would route through
  // CbControlChannel by id rather than through this method.
  return false;
}

void CbJavaScriptDialogManager::CancelDialogs(
    content::WebContents* /*web_contents*/,
    bool /*reset_state*/) {
  // Called when the WebContents is navigating away or being destroyed with
  // a dialog still up. Every outstanding callback MUST run or the
  // renderer's JS thread stays blocked.
  //
  // NOTE the deliberate over-cancel: CbControlChannel keys requests by id,
  // not by WebContents, so this resolves dialogs for ALL tabs rather than
  // just |web_contents|. That is the safe direction to err — an extra
  // dialog resolving with its default is a cosmetic loss, while a missed
  // one is a permanently wedged page. Making this precise requires
  // per-WebContents request tracking, which is only worth adding once
  // multi-tab dialogs are actually reachable.
  if (control_channel_) {
    control_channel_->CancelAllPending();
  }
}

}  // namespace cloud_browser

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbJavaScriptDialogManager — alert() / confirm() / prompt() /
// beforeunload, routed to the human watching the stream.
//
// WHY THIS EXISTS
//
// A //content embedder with no JavaScriptDialogManager gets chromium's
// default, which auto-dismisses every dialog. For a cloud browser that is
// not a cosmetic gap: a page that gates on `if (!confirm(...)) return;`
// silently takes the cancel branch, and the user sees a page that simply
// does not work with no indication why. Cookie banners, "leave site?"
// guards, delete-confirmations and a large slice of enterprise web apps
// all sit behind one of these calls.
//
// THE LIVENESS INVARIANT
//
// content::JavaScriptDialogManager::RunJavaScriptDialog hands us a
// DialogClosedCallback and BLOCKS THE CALLING PAGE'S JS THREAD until it
// runs. Every path through this class must therefore run it exactly once.
// A dropped portal, a closed DataChannel, a user who closed the tab, or a
// teardown mid-dialog must all still resolve — otherwise the page is
// wedged forever with no recovery short of killing the guest.
//
// CbControlChannel provides that guarantee structurally: its SendRequest
// contract is that the callback ALWAYS runs exactly once (synchronously on
// a closed channel, via a deadline timer on silence, via CancelAllPending
// on teardown). This class adds the policy layer on top — what "no answer"
// means per dialog type.
//
// DEFAULTS WHEN THERE IS NO HUMAN
//
// Deliberately identical to chromium's no-delegate behaviour, so an
// unattended agent-driven session behaves exactly as it did before this
// class existed:
//
//   alert        -> accept   (it has no cancel branch; dismissing IS
//                             acknowledging)
//   confirm      -> cancel   (the safe branch: do not delete, do not buy)
//   prompt       -> cancel   (empty input, not a fabricated value)
//   beforeunload -> accept   (allow the navigation to proceed; blocking it
//                             on a headless session would strand the guest
//                             on a page it was told to leave)
//
// The asymmetry on beforeunload is worth stating plainly: for a HUMAN the
// safe answer is "stay on the page, you might lose work", but for an
// automated session there is no unsaved work to lose and refusing to
// navigate wedges the automation instead. When a human IS attached they
// are asked, and their answer wins.

#ifndef CAPTURE_BUILD_INTEGRATION_CB_JAVASCRIPT_DIALOG_MANAGER_H_
#define CAPTURE_BUILD_INTEGRATION_CB_JAVASCRIPT_DIALOG_MANAGER_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "content/public/browser/javascript_dialog_manager.h"
// content::JavaScriptDialogType — lives in common/, not browser/, and is
// named directly in the RunJavaScriptDialog override below.
#include "content/public/common/javascript_dialog_type.h"

namespace cloud_browser {

class CbControlChannel;

class CbJavaScriptDialogManager : public content::JavaScriptDialogManager {
 public:
  CbJavaScriptDialogManager();
  ~CbJavaScriptDialogManager() override;

  CbJavaScriptDialogManager(const CbJavaScriptDialogManager&) = delete;
  CbJavaScriptDialogManager& operator=(const CbJavaScriptDialogManager&) =
      delete;

  // The control channel is per-session while this manager is
  // process-lifetime (it hangs off the WebContentsDelegate singleton), so
  // it is injected rather than owned. Pass nullptr on session teardown —
  // subsequent dialogs then take the no-human default path immediately
  // instead of dereferencing a dangling channel.
  void SetControlChannel(CbControlChannel* channel);

  // content::JavaScriptDialogManager:
  void RunJavaScriptDialog(content::WebContents* web_contents,
                           content::RenderFrameHost* render_frame_host,
                           content::JavaScriptDialogType dialog_type,
                           const std::u16string& message_text,
                           const std::u16string& default_prompt_text,
                           DialogClosedCallback callback,
                           bool* did_suppress_message) override;
  void RunBeforeUnloadDialog(content::WebContents* web_contents,
                             content::RenderFrameHost* render_frame_host,
                             bool is_reload,
                             DialogClosedCallback callback) override;
  bool HandleJavaScriptDialog(content::WebContents* web_contents,
                              bool accept,
                              const std::u16string* prompt_override) override;
  void CancelDialogs(content::WebContents* web_contents,
                     bool reset_state) override;

 private:
  // Shared tail for both Run* methods: mint the control request, or apply
  // |default_accept| immediately when no channel is attached.
  void AskOrDefault(const std::string& dialog_type_name,
                    const std::u16string& message_text,
                    const std::u16string& default_prompt_text,
                    const std::string& origin,
                    bool default_accept,
                    DialogClosedCallback callback);

  raw_ptr<CbControlChannel> control_channel_ = nullptr;

  base::WeakPtrFactory<CbJavaScriptDialogManager> weak_factory_{this};
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CB_JAVASCRIPT_DIALOG_MANAGER_H_

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CbLoginDelegate — the HTTP Basic/Digest auth box.
//
// WHAT IT REPLACES
// ----------------
// Nothing. ContentBrowserClient::CreateLoginDelegate() returned nullptr,
// which //content reads as "the embedder will not handle this" and cancels
// the challenge. A 401 therefore rendered as the server's own error body, or
// as a blank page, with no way to supply credentials — a whole class of
// intranet and appliance URLs simply could not be opened.
//
// THE CONTRACT (from content_browser_client.h:2502-2525, not the interface)
// ------------------------------------------------------------------------
// LoginDelegate itself is nearly empty — a virtual destructor and one type
// alias. Everything that matters is in the comment above the factory:
//
//   * The callback runs on the UI thread.
//   * It MUST NOT be called reentrantly. If the answer is known
//     synchronously, post it to a later loop iteration.
//   * DESTRUCTION IS CANCELLATION. If this object dies before the callback
//     runs, the request has been cancelled and the callback must NOT run.
//
// That last rule is why the response is bound through a WeakPtr: the control
// channel can answer at any time, including after //content has dropped us,
// and running the callback then is a use-after-free on the network stack's
// state rather than a harmless late reply.
//
// A cancelled login is std::nullopt, which //content turns back into the
// 401. That is also the default for every path that is not an explicit
// answer: no channel, a timeout, or a malformed reply.

#ifndef CAPTURE_BUILD_INTEGRATION_CB_LOGIN_DELEGATE_H_
#define CAPTURE_BUILD_INTEGRATION_CB_LOGIN_DELEGATE_H_

#include <string>

#include "base/memory/weak_ptr.h"
// base::DictValue appears in OnViewerResponded's signature below.
#include "base/values.h"
#include "content/public/browser/login_delegate.h"
#include "net/base/auth.h"
#include "url/gurl.h"

namespace cloud_browser {

class CbLoginDelegate : public content::LoginDelegate {
 public:
  // Asks the viewer immediately. |callback| runs on the UI thread exactly
  // once, unless this object is destroyed first — in which case it does not
  // run at all, per the contract above.
  CbLoginDelegate(const net::AuthChallengeInfo& auth_info,
                  const GURL& url,
                  bool first_auth_attempt,
                  LoginAuthRequiredCallback callback);
  ~CbLoginDelegate() override;

  CbLoginDelegate(const CbLoginDelegate&) = delete;
  CbLoginDelegate& operator=(const CbLoginDelegate&) = delete;

 private:
  void OnViewerResponded(base::DictValue response);

  LoginAuthRequiredCallback callback_;
  base::WeakPtrFactory<CbLoginDelegate> weak_factory_{this};
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CB_LOGIN_DELEGATE_H_

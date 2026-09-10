// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "capture/build-integration/cb_login_delegate.h"

#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
// SequencedTaskRunner::GetCurrentDefault() posts the synchronous-answer case
// to a later loop iteration, per the factory's no-reentrancy contract.
#include "base/task/sequenced_task_runner.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "capture/build-integration/cb_control_channel.h"
#include "capture/build-integration/cb_web_contents_delegate.h"

namespace cloud_browser {
namespace {

constexpr char kLog[] = "CV2-LOGIN: ";

// How long the auth box waits. Two minutes: a person has to read the realm,
// find the credentials and type them, and the request is already stalled —
// but an unattended session should not hold a socket open indefinitely.
constexpr base::TimeDelta kLoginDeadline = base::Minutes(2);

}  // namespace

CbLoginDelegate::CbLoginDelegate(const net::AuthChallengeInfo& auth_info,
                                 const GURL& url,
                                 bool first_auth_attempt,
                                 LoginAuthRequiredCallback callback)
    : callback_(std::move(callback)) {
  CbControlChannel* channel =
      GetCloudBrowserWebContentsDelegate()->control_channel();
  if (!channel) {
    LOG(INFO) << kLog << "no control channel; cancelling the auth challenge "
              << "for " << url.possibly_invalid_spec();
    // NOT called inline. The factory's contract says the callback "may not be
    // called reentrantly" and must be posted "to a separate event loop
    // iteration" if the answer is known synchronously — this is exactly that
    // case, and the WeakPtr also honours destruction-is-cancellation.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&CbLoginDelegate::OnViewerResponded,
                                  weak_factory_.GetWeakPtr(),
                                  base::DictValue()));
    return;
  }

  base::DictValue payload;
  payload.Set("url", url.possibly_invalid_spec());
  // The realm is server-supplied text shown to a person deciding whether to
  // hand over a password, so the client truncates and escapes it. is_proxy
  // matters: proxy credentials are a different secret from site ones, and a
  // prompt that conflates them invites the wrong password.
  payload.Set("realm", auth_info.realm);
  payload.Set("scheme", auth_info.scheme);
  payload.Set("is_proxy", auth_info.is_proxy);
  // False on a retry, i.e. "the last credentials were rejected" — worth
  // saying, otherwise the second prompt looks identical to the first and the
  // user retypes the same wrong password.
  payload.Set("first_attempt", first_auth_attempt);

  channel->SendRequest("login", std::move(payload), kLoginDeadline,
                       base::BindOnce(&CbLoginDelegate::OnViewerResponded,
                                      weak_factory_.GetWeakPtr()));
}

CbLoginDelegate::~CbLoginDelegate() {
  // Deliberately does NOT run the callback. Destruction IS cancellation
  // (content_browser_client.h:2503-2505): //content has already torn down
  // the request, and answering now would write into freed state. The
  // WeakPtrFactory invalidating here is what enforces it.
  if (callback_) {
    LOG(INFO) << kLog << "auth challenge cancelled before the viewer answered";
  }
}

void CbLoginDelegate::OnViewerResponded(base::DictValue response) {
  if (!callback_) {
    return;
  }
  const std::string* username = response.FindString("username");
  const std::string* password = response.FindString("password");
  if (!username || !password) {
    // An empty dict is the channel's "no answer" — closed, timed out, or
    // torn down. std::nullopt is a cancelled login, which //content turns
    // back into the 401 the page would have shown anyway.
    std::move(callback_).Run(std::nullopt);
    return;
  }
  LOG(INFO) << kLog << "viewer supplied credentials";
  std::move(callback_).Run(net::AuthCredentials(
      base::UTF8ToUTF16(*username), base::UTF8ToUTF16(*password)));
}

}  // namespace cloud_browser

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cloud-browser/capture/build-integration/cb_clipboard_relay.h"

#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "cloud-browser/capture/build-integration/cb_active_webcontents_resolver.h"  // WebContentsResolver
#include "components/input/native_web_keyboard_event.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/clipboard_buffer.h"
#include "ui/base/clipboard/clipboard_monitor.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"

namespace cloud_browser {

namespace {

constexpr int kProtocolVersion = 1;
constexpr char kTypeClipboardOffer[] = "clipboard_offer";
constexpr char kDirectionToCloud[] = "client->cloud";
constexpr char kDirectionToClient[] = "cloud->client";
constexpr char kSourceUserAction[] = "user_action";

// VKEY_V. Hard-coded for the same reason CbInputDispatchClipboard hard-codes
// VKEY_C: this file synthesises exactly one key, and pulling in the keyboard
// dispatcher's scancode table for it would couple two ranks for nothing.
constexpr int kVkeyV = 0x56;

int64_t NowMs() {
  return (base::Time::Now() - base::Time::UnixEpoch()).InMilliseconds();
}

}  // namespace

CbClipboardRelay::CbClipboardRelay(
    signaling::CbDataChannelHost* dc_host,
    WebContentsResolver* resolver,
    scoped_refptr<base::SequencedTaskRunner> ui_task_runner)
    : dc_host_(dc_host),
      resolver_(resolver),
      ui_task_runner_(std::move(ui_task_runner)) {
  // Constructed on the UI thread (RebuildNativeDataChannels). Registering
  // here is what makes copy work at all: without an observer nothing in the
  // guest ever learns that the page wrote its clipboard.
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);
  ui::ClipboardMonitor::GetInstance()->AddObserver(this);
  LOG(INFO) << "CV2-CLIPBOARD: relay bound — paste writes the guest clipboard "
               "and synthesises Ctrl+V; copy is forwarded within a "
            << kClipboardArmWindow.InMilliseconds()
            << " ms window after a copy gesture";
}

CbClipboardRelay::~CbClipboardRelay() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);
  ui::ClipboardMonitor::GetInstance()->RemoveObserver(this);
}

// ---------------------------------------------------------------------------
// Paste: client -> cloud
// ---------------------------------------------------------------------------

void CbClipboardRelay::OnMessage(const webrtc::DataBuffer& buffer) {
  // Signaling thread. Same shape as CbControlChannel::OnMessage: reject
  // what can be rejected without touching the UI thread, then hop.
  if (buffer.binary) {
    LOG(WARNING) << "CV2-CLIPBOARD: binary frame on the clipboard channel, "
                    "dropped (v1 is JSON text)";
    return;
  }
  if (buffer.data.size() > kMaxClipboardTextBytes + 512) {
    // The envelope wraps the text with ~100 bytes of framing; anything past
    // the cap plus slack cannot carry a legal payload, so do not parse it.
    LOG(WARNING) << "CV2-CLIPBOARD: oversized frame (" << buffer.data.size()
                 << " bytes) dropped before parse";
    return;
  }
  HandleInboundJson(
      std::string(reinterpret_cast<const char*>(buffer.data.data()),
                  buffer.data.size()));
}

void CbClipboardRelay::OnMessageForTesting(const std::string& json) {
  HandleInboundJson(json);
}

void CbClipboardRelay::HandleInboundJson(const std::string& raw) {
  // ReadDict with JSON_PARSE_RFC — strict, and `options` is REQUIRED at 7727
  // (docs/build/chromium-7727-api-pins.md). A remote peer's frame gets no
  // leniency.
  std::optional<base::DictValue> parsed =
      base::JSONReader::ReadDict(raw, base::JSON_PARSE_RFC);
  if (!parsed) {
    LOG(WARNING) << "CV2-CLIPBOARD: inbound frame is not a JSON object — "
                    "dropped";
    return;
  }
  const base::DictValue& envelope = *parsed;
  const std::optional<int> v = envelope.FindInt("v");
  const std::string* type = envelope.FindString("type");
  const base::DictValue* data = envelope.FindDict("data");
  if (!v || *v != kProtocolVersion || !type || *type != kTypeClipboardOffer ||
      !data) {
    LOG(WARNING) << "CV2-CLIPBOARD: envelope is not a v1 clipboard_offer — "
                    "dropped";
    return;
  }
  const std::string* direction = data->FindString("direction");
  const std::string* source = data->FindString("source");
  const std::string* text = data->FindString("text");
  if (!direction || *direction != kDirectionToCloud) {
    // A cloud->client envelope arriving here is the client echoing, or a
    // client bug. The spec says wrong-direction envelopes are dropped by the
    // receiver, so drop rather than apply.
    VLOG(1) << "CV2-CLIPBOARD: ignoring inbound envelope with direction "
            << (direction ? *direction : "<missing>");
    return;
  }
  if (!source || *source != kSourceUserAction) {
    LOG(WARNING) << "CV2-CLIPBOARD: source is not user_action — dropped";
    return;
  }
  if (!text) {
    LOG(WARNING) << "CV2-CLIPBOARD: clipboard_offer has no text — dropped";
    return;
  }
  if (text->size() > kMaxClipboardTextBytes) {
    LOG(WARNING) << "CV2-CLIPBOARD: paste of " << text->size()
                 << " bytes exceeds the 1 MiB cap — dropped, not truncated";
    return;
  }
  ui_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&CbClipboardRelay::ApplyPaste,
                                weak_factory_.GetWeakPtr(), *text));
}

void CbClipboardRelay::ApplyPaste(std::string text) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);

  // 1. The guest clipboard. Remember the text first so the ClipboardMonitor
  //    notification this write produces is recognised as our own echo.
  last_written_text_ = text;
  {
    ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
    writer.WriteText(base::UTF8ToUTF16(text));
    // The write commits, and the monitor fires, when |writer| goes out of
    // scope.
  }

  // 2. Paste it where the caret is. Without this step the text would sit in
  //    a clipboard the viewer cannot see; the paste gesture they made is the
  //    whole point.
  content::WebContents* wc =
      resolver_ ? resolver_->GetActiveWebContents() : nullptr;
  if (!wc) {
    LOG(WARNING) << "CV2-CLIPBOARD: paste written to the guest clipboard but "
                    "not dispatched — no active WebContents";
    return;
  }
  SynthesizePaste(wc);
  ++pastes_applied_;
  VLOG(1) << "CV2-CLIPBOARD: paste applied (" << text.size() << " bytes)";
}

void CbClipboardRelay::SynthesizePaste(content::WebContents* wc) {
  // Mirrors CbInputDispatchClipboard::DispatchCopy, for VKEY_V. Walk
  // WC -> RWHV -> RWH live; never cache (a cross-document navigation swaps
  // the widget). Ctrl is set on the per-event modifiers only — the shared
  // held-modifier state is not touched, because the viewer never reported
  // holding Ctrl on the wire.
  content::RenderWidgetHostView* rwhv = wc->GetRenderWidgetHostView();
  content::RenderWidgetHost* rwh = rwhv ? rwhv->GetRenderWidgetHost() : nullptr;
  if (!rwh) {
    LOG(WARNING) << "CV2-CLIPBOARD: paste not dispatched — no RenderWidgetHost";
    return;
  }
  const int modifiers =
      static_cast<int>(blink::WebInputEvent::Modifiers::kControlKey);
  const base::TimeTicks now = base::TimeTicks::Now();
  {
    input::NativeWebKeyboardEvent native(
        blink::WebInputEvent::Type::kRawKeyDown, modifiers, now);
    native.windows_key_code = kVkeyV;
    native.native_key_code = kVkeyV;
    rwh->ForwardKeyboardEvent(native);
  }
  {
    input::NativeWebKeyboardEvent native(blink::WebInputEvent::Type::kKeyUp,
                                         modifiers, now);
    native.windows_key_code = kVkeyV;
    native.native_key_code = kVkeyV;
    rwh->ForwardKeyboardEvent(native);
  }
  // No kChar: Ctrl+V inserts nothing itself; the renderer's editor command
  // performs the paste from the clipboard we just wrote.
}

// ---------------------------------------------------------------------------
// Copy: cloud -> client
// ---------------------------------------------------------------------------

void CbClipboardRelay::ArmCopyWindow() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);
  copy_armed_until_ = base::TimeTicks::Now() + kClipboardArmWindow;
}

void CbClipboardRelay::OnClipboardDataChanged() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);
  if (copy_armed_until_.is_null() ||
      base::TimeTicks::Now() > copy_armed_until_) {
    // No user gesture behind this change. A page writing its clipboard from
    // a timer, or our own paste write arriving after the window — either
    // way, not the viewer's to receive. This is the spec's "no silent
    // polling", and the reason the relay observes the monitor at all rather
    // than hooking the page.
    ++changes_ignored_;
    VLOG(1) << "CV2-CLIPBOARD: clipboard changed outside a copy window — "
               "not forwarded (ignored=" << changes_ignored_ << ")";
    return;
  }
  // One change per gesture: disarm before the async read so a second write
  // inside the window (a page reacting to the copy) does not also go out.
  copy_armed_until_ = base::TimeTicks();
  ui::Clipboard* clipboard = ui::Clipboard::GetForCurrentThread();
  if (!clipboard) {
    LOG(WARNING) << "CV2-CLIPBOARD: no clipboard for the UI thread";
    return;
  }
  clipboard->ReadText(ui::ClipboardBuffer::kCopyPaste,
                      /*data_dst=*/std::nullopt,
                      base::BindOnce(&CbClipboardRelay::OnClipboardTextRead,
                                     weak_factory_.GetWeakPtr()));
}

void CbClipboardRelay::OnClipboardTextRead(std::u16string text16) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);
  const std::string text = base::UTF16ToUTF8(text16);
  if (text.empty()) {
    return;  // A non-text copy (image, files). v1 is text only.
  }
  if (text == last_written_text_) {
    // Our own paste write, echoed back through the monitor inside a window
    // the viewer armed by pasting-then-copying quickly. Not a user copy.
    ++changes_ignored_;
    return;
  }
  if (text.size() > kMaxClipboardTextBytes) {
    LOG(WARNING) << "CV2-CLIPBOARD: copy of " << text.size()
                 << " bytes exceeds the 1 MiB cap — dropped, not truncated";
    return;
  }
  SendCopy(text);
}

void CbClipboardRelay::SendCopy(const std::string& text) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);
  base::DictValue data;
  data.Set("direction", kDirectionToClient);
  data.Set("source", kSourceUserAction);
  data.Set("text", text);
  base::DictValue envelope;
  envelope.Set("v", kProtocolVersion);
  envelope.Set("type", kTypeClipboardOffer);
  envelope.Set("t", static_cast<double>(NowMs()));
  envelope.Set("seq", static_cast<double>(next_seq_++));
  envelope.Set("data", std::move(data));

  std::string json;
  if (!base::JSONWriter::Write(base::Value(std::move(envelope)), &json)) {
    LOG(ERROR) << "CV2-CLIPBOARD: failed to serialise clipboard_offer";
    return;
  }
  ++copies_sent_;
  dc_host_->SendAsync(
      signaling::CbDcLabel::kClipboard, std::move(json), ui_task_runner_,
      base::BindOnce([](bool ok, std::string message) {
        if (!ok) {
          LOG(WARNING) << "CV2-CLIPBOARD: copy not delivered: " << message;
        }
      }));
  VLOG(1) << "CV2-CLIPBOARD: copy forwarded (" << text.size() << " bytes)";
}

// ---------------------------------------------------------------------------
// DataChannelObserver plumbing
// ---------------------------------------------------------------------------

void CbClipboardRelay::OnStateChange() {
  // Channel open/close is observed by the host's CbDataChannelHostObserver;
  // a closed channel simply stops delivering OnMessage.
}

void CbClipboardRelay::OnBufferedAmountChange(uint64_t /*sent_data_size*/) {
  // Clipboard frames are small and user-paced; no backpressure gate.
}

bool CbClipboardRelay::IsOkToCallOnTheNetworkThread() {
  // Stay on the signaling thread, like every other consumer: OnMessage
  // parses and hops, and the hop is what keeps clipboard work off libwebrtc's
  // network thread.
  return false;
}

}  // namespace cloud_browser

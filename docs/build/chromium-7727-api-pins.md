# Chromium 7727 API pins — verified signatures for the browser-fidelity work

**Source of truth:** the pinned tree on the build node, read directly.

- Node: `triform-7`, hostPath `/var/lib/longhorn/chromeless-build/chromium-src`
- Chromium root inside that path: `src/chromium/src`
- Version read from `chrome/VERSION`: **147.0.7727.144** (`branch-heads/7727`)
- Read on 2026-07-29 via a read-only busybox pod (`kubectl run` + hostPath mount).

Why this file exists: `cloud_browser_worker` cannot be compiled locally — the
only build is a ~4 h K8s Job. Every signature guessed wrong costs a full build
cycle. Reading the headers costs nothing. **Do not write embedder C++ against
remembered Chromium APIs; check here first, and re-verify this file after every
branch roll.**

Legend: ✅ = read from the header verbatim.

---

## content::WebContentsDelegate
`content/public/browser/web_contents_delegate.h`

✅ **AddNewContents** — `:199`. Returns `WebContents*` (not void). Takes an
**owned** `unique_ptr`, so the delegate must adopt it or it is destroyed.
```cpp
virtual WebContents* AddNewContents(
    WebContents* source,
    std::unique_ptr<WebContents> new_contents,
    const GURL& target_url,
    WindowOpenDisposition disposition,
    const blink::mojom::WindowFeatures& window_features,
    bool user_gesture,
    bool* was_blocked);
```

✅ **WebContentsCreated** — `:407`. Note it takes the opener's process/frame
ids and a `frame_name`, and returns void — it is a notification, not the
adoption hook. Adoption belongs in `AddNewContents`.
```cpp
virtual void WebContentsCreated(WebContents* source_contents,
                                int opener_render_process_id,
                                int opener_render_frame_id,
                                const std::string& frame_name,
                                const GURL& target_url,
                                WebContents* new_contents) {}
```

✅ **OpenURLFromTab** — `:164`. The trailing navigation-handle callback IS
present at 7727.
```cpp
virtual WebContents* OpenURLFromTab(
    WebContents* source,
    const OpenURLParams& params,
    base::OnceCallback<void(NavigationHandle&)> navigation_handle_callback);
```

✅ **Fullscreen** — `:525`–`:546`. **`CanEnterFullscreenModeForTab` must return
true or `EnterFullscreenModeForTab` is never called.** This gate was not in the
original plan; overriding only the enter/exit pair would silently do nothing.
`IsFullscreenForTabOrPending` is **non-const** and takes `const WebContents*`.
```cpp
virtual bool CanEnterFullscreenModeForTab(RenderFrameHost* requesting_frame);
virtual void EnterFullscreenModeForTab(
    RenderFrameHost* requesting_frame,
    const blink::mojom::FullscreenOptions& options) {}
virtual void ExitFullscreenModeForTab(WebContents*) {}
virtual bool IsFullscreenForTabOrPending(const WebContents* web_contents);
virtual FullscreenState GetFullscreenState(const WebContents*) const;
```

✅ **CanDownload** — `:313`. No `request_initiator` parameter at 7727.
```cpp
virtual void CanDownload(const GURL& url,
                         const std::string& request_method,
                         base::OnceCallback<void(bool)> callback);
```

✅ **RunFileChooser / EnumerateDirectory** — `:477`, `:491`. Listener is
`scoped_refptr`. `EnumerateDirectory` is the `webkitdirectory` companion and
carries the same listener contract.
```cpp
virtual void RunFileChooser(RenderFrameHost* render_frame_host,
                            scoped_refptr<FileSelectListener> listener,
                            const blink::mojom::FileChooserParams& params);
virtual void EnumerateDirectory(WebContents* web_contents,
                                scoped_refptr<FileSelectListener> listener,
                                const base::FilePath& path);
```

✅ **CONTEXT MENU — resolved.** It is the `WebContentsDelegate` hook, *not*
`WebContentsViewDelegate`. `WebContentsImpl::ShowContextMenu`
(`content/browser/web_contents/web_contents_impl.cc:8882`) calls
`delegate_->HandleContextMenu(...)` and only falls through to
`render_view_host_delegate_view_->ShowContextMenu` at `:8886` when it returns
false. So returning **true suppresses** the default menu.
```cpp
// web_contents_delegate.h:324
virtual bool HandleContextMenu(RenderFrameHost& render_frame_host,
                               const ContextMenuParams& params);
```

---

## content::JavaScriptDialogManager
`content/public/browser/javascript_dialog_manager.h`

✅ Three pure virtuals (`= 0`) — `RunJavaScriptDialog`, `RunBeforeUnloadDialog`,
`CancelDialogs` — plus one non-pure `HandleJavaScriptDialog`. `is_reload` IS
present. There is no extra `RunAppModalDialog`-shaped pure virtual.
```cpp
using DialogClosedCallback =
    base::OnceCallback<void(bool /* success */,
                            const std::u16string& /* user_input */)>;

virtual void RunJavaScriptDialog(WebContents*, RenderFrameHost*,
                                 JavaScriptDialogType dialog_type,
                                 const std::u16string& message_text,
                                 const std::u16string& default_prompt_text,
                                 DialogClosedCallback callback,
                                 bool* did_suppress_message) = 0;
virtual void RunBeforeUnloadDialog(WebContents*, RenderFrameHost*,
                                   bool is_reload,
                                   DialogClosedCallback callback) = 0;
virtual bool HandleJavaScriptDialog(WebContents*, bool accept,
                                    const std::u16string* prompt_override);
virtual void CancelDialogs(WebContents*, bool reset_state) = 0;
```

---

## RenderWidgetHostImpl drag API
`content/browser/renderer_host/render_widget_host_impl.h:284`–`:311`

✅ All four, exactly as the repo's commented-out draft in
`cb_input_dispatch_drag.cc` guessed. `DragTargetDrop` takes **no** operations
mask and a plain `base::OnceClosure`. `DragTargetDragLeave` takes both points.
```cpp
void DragTargetDragEnter(const DropData& drop_data,
                         const gfx::PointF& client_pt,
                         const gfx::PointF& screen_pt,
                         blink::DragOperationsMask operations_allowed,
                         int key_modifiers,
                         DragOperationCallback callback) override;
void DragTargetDragOver(const gfx::PointF& client_point,
                        const gfx::PointF& screen_point,
                        blink::DragOperationsMask operations_allowed,
                        int key_modifiers,
                        DragOperationCallback callback) override;
void DragTargetDragLeave(const gfx::PointF& client_point,
                         const gfx::PointF& screen_point) override;
void DragTargetDrop(const DropData& drop_data,
                    const gfx::PointF& client_point,
                    const gfx::PointF& screen_point,
                    int key_modifiers,
                    base::OnceClosure callback) override;
```

⚠️ **`FilterDropData` is MANDATORY and was missing from the plan.** The header
says: *"`drop_data` must have been filtered. The embedder should call
`FilterDropData` before passing the drop data to RWHI."* It is public on the
**base** class, so no Impl cast is needed for it:
`content/public/browser/render_widget_host.h:344` —
`virtual void FilterDropData(DropData* drop_data) {}`.
Call `rwh->FilterDropData(&drop_data)` before both `DragTargetDragEnter` and
`DragTargetDrop`.

`DragOperationCallback` is declared at `render_widget_host.h:303`.

### content::DropData field types
`content/public/common/drop_data.h`
```cpp
std::vector<ui::ClipboardUrlInfo> url_infos;                 // :93
std::vector<ui::FileInfo> filenames;                         // :106
std::optional<std::u16string> text;                          // :117
std::optional<std::u16string> html;                          // :122
std::unordered_map<std::u16string, std::u16string> custom_data;  // :132
```

⚠️ **CORRECTED 2026-08-19 — this file previously listed `GURL url; // :42`
and that was WRONG.** Line 42 is `FileSystemFileInfo::url`, a field of a
NESTED struct; `DropData` itself has no top-level `url`. The build failed
with *"no member named 'url' in 'content::DropData'"*.

At 7727 the single-valued URL is `std::vector<ui::ClipboardUrlInfo>
url_infos` (`ui/base/clipboard/clipboard_url_info.h`: `{GURL url;
std::u16string title;}`), so `//ui/base` is needed for that header too.
The vector is strictly better here — the old single-valued field meant a
multi-URL drag silently kept only the last item.

**Reading a line number out of a header is not the same as reading the
struct.** Grep for the field, then check what scope it is actually in.
⚠️ The existing `TODO` in `cb_input_dispatch_drag.cc:551` guessing
`content::DropData::FileInfo` is **stale** — it is `ui::FileInfo`
(`ui/base/clipboard/file_info.h`), so `//ui/base` must be added to that
`source_set`'s deps.

---

## base::Value — the dictionary type is `base::DictValue`

⚠️ **`base::Value::Dict` DOES NOT EXIST at 7727.** `DictValue` and
`ListValue` are namespace-level `base::` classes declared *before* `class
Value` (`base/values.h:247` and `:48`), not nested types. There is no
back-compat alias.

```cpp
base::DictValue d;              // NOT base::Value::Dict
base::ListValue l;              // NOT base::Value::List
```

The pinned tree contains **1326** uses of `base::DictValue` and **zero** of
`base::Value::Dict`. Our own already-compiling code uses the right one, and
`capture/signaling/cb_wire_envelope.h:171` even documents the rename — the
Wave 1 files were written against the older spelling anyway and cost a build
cycle. Cost: ~25 errors across three files, all from this one name.

Constructing one needs the full `base/values.h`; a forward declaration is
not enough.

---

## base::JSONReader — `options` is REQUIRED, there is no default

`Read`, `ReadDict` and `ReadList` all take `(std::string_view json, int
options, size_t max_depth = kAbsoluteMaxDepth)`. Only `max_depth` is
defaulted. A bare `JSONReader::Read(raw)` fails with *"too few arguments to
function call, expected at least 2, have 1"*.

```cpp
// Parses AND extracts the top-level object in one call.
std::optional<base::DictValue> d =
    base::JSONReader::ReadDict(raw, base::JSON_PARSE_RFC);
```

Prefer `ReadDict` over `Read` + `is_dict()` + `GetDict()` — shorter, and it
returns the dict type directly. Use `JSON_PARSE_RFC` (strict: no comments,
no trailing commas) for anything arriving from a remote peer; leniency there
is attack surface, not politeness.

---

## blink::mojom::StreamDevicesSet needs the FULL mojom header

`content/public/browser/media_stream_request.h` includes only
`media_stream.mojom-**shared**.h`, which is enough to NAME the type in a
callback signature but not to CONSTRUCT one — you get *"invalid use of
incomplete type"*. Add:

```cpp
#include "third_party/blink/public/mojom/mediastream/media_stream.mojom.h"
```

That is what every in-tree caller building a `StreamDevicesSet` does (e.g.
`content/browser/media/captured_surface_controller.cc:22`).

---

## webrtc: sender() returns a scoped_refptr

`RtpTransceiverInterface::sender()` is
`virtual scoped_refptr<RtpSenderInterface> sender() const`
(`third_party/webrtc/api/rtp_transceiver_interface.h:78`) — **not** a raw
pointer. Assigning it to `RtpSenderInterface*` fails with *"no viable
conversion"*. Hold the refptr:

```cpp
webrtc::scoped_refptr<webrtc::RtpSenderInterface> sender =
    transceiver->sender();
```

---

## content::PermissionControllerDelegate
`content/public/browser/permission_controller_delegate.h`

**Highest-drift surface in the whole workstream — 13 virtuals.**

⚠️ **Permissions are keyed on `blink::mojom::PermissionDescriptorPtr`, NOT the
`blink::PermissionType` enum.** The roadmap's policy table was written against
the enum; it must be rewritten against descriptors. Only `ResetPermission`
still takes `blink::PermissionType`.

⚠️ Note `UnsubscribeFromPermissionResultChange` (**Result**, not *Status*),
while the paired add-hook is `OnPermissionStatusChangeSubscriptionAdded`. The
asymmetry is real — do not "fix" it.

Virtuals at `:48, 59, 69, 74, 89, 98, 109, 116, 121, 127, 135, 140`:
`RequestPermissions`, `RequestPermissionsFromCurrentDocument`,
`GetPermissionStatus`, `GetPermissionResultForOriginWithoutContext`,
`GetPermissionResultForCurrentDocument`, `GetPermissionResultForWorker`,
`GetPermissionResultForEmbeddedRequester`, `ResetPermission`,
`OnPermissionStatusChangeSubscriptionAdded`,
`UnsubscribeFromPermissionResultChange`, `GetExclusionAreaBoundsInScreen`,
`IsPermissionOverridable`.

**Copy `content/shell/browser/shell_permission_manager.{h,cc}` verbatim** and
replace only the policy body. Its exact override list (verified present in the
tree) is: `RequestPermissions`, `ResetPermission`,
`RequestPermissionsFromCurrentDocument`, `GetPermissionStatus`,
`GetPermissionResultForOriginWithoutContext`,
`GetPermissionResultForCurrentDocument` (takes `bool
should_include_device_status`), `GetPermissionResultForWorker`,
`GetPermissionResultForEmbeddedRequester`. Callbacks are
`base::OnceCallback<void(const std::vector<PermissionResult>&)>`.

---

## content::FileSelectListener
`content/public/browser/file_select_listener.h:28`, `:35`
```cpp
virtual void FileSelected(std::vector<blink::mojom::FileChooserFileInfoPtr> files,
                          const base::FilePath& base_dir,
                          blink::mojom::FileChooserParams::Mode mode) = 0;
virtual void FileSelectionCanceled() = 0;
```
It is `base::RefCounted`; the dtor is protected. Exactly one of the two MUST be
called before it is released.

---

## content::DownloadManagerDelegate
`content/public/browser/download_manager_delegate.h`

⚠️ **Neither form the plan guessed.** `DetermineDownloadTarget` returns
**`bool`** and takes the callback **by pointer**:
```cpp
virtual bool DetermineDownloadTarget(download::DownloadItem* item,
                                     download::DownloadTargetCallback* callback);  // :109
virtual void GetNextId(DownloadIdCallback callback);                               // :94
```
Note `DownloadIdCallback`, not `IdCallback`.

---

## Crash observers

✅ `content::WebContentsObserver::PrimaryMainFrameRenderProcessGone`
(`content/public/browser/web_contents_observer.h:274`) — this spelling exists
at 7727; the older `RenderProcessGone` does **not**.

✅ `content::GpuDataManagerObserver::OnGpuProcessCrashed()`
(`content/public/browser/gpu_data_manager_observer.h:24`) — **no**
`base::TerminationStatus` parameter.

---

## Display / viewport (workstream 2)

✅ `display::Display::SetScaleAndBounds(float device_scale_factor, const
gfx::Rect& bounds_in_pixel)` — `ui/display/display.h:188`. Takes **pixel**
bounds.

✅ `display::ScreenBase::ProcessDisplayChanged(const Display& changed_display,
bool is_primary)` — `ui/display/screen_base.h:53`. **This is the correct
mutator** — it is the embedder helper that fires `OnDisplayMetricsChanged`.
Prefer it over poking `display_list()` directly, which would skip observer
fan-out and leave `devicePixelRatio` stale in the renderer.

---

## content::ContentBrowserClient
`content/public/browser/content_browser_client.h:1417`

✅ Parameter is `is_primary_main_frame_request` (not `is_main_frame_request`):
```cpp
virtual void AllowCertificateError(
    WebContents* web_contents, int cert_error, const net::SSLInfo& ssl_info,
    const GURL& request_url, bool is_primary_main_frame_request,
    bool strict_enforcement,
    base::OnceCallback<void(CertificateRequestResultType)> callback);
```

---

## Batch A (2026-09-05): CloseContents, clipboard, keyframe, user-data-dir

All read from the tree in the recon pod on 2026-09-05.

✅ **WebContentsDelegate::CloseContents** — `web_contents_delegate.h:234`,
`virtual void CloseContents(WebContents* source) {}`. Default is a NO-OP, which
is why `window.close()` did nothing until it was overridden. content_shell's
override (`shell.cc:626`) just calls its own `Close()`.

✅ **RtpSenderInterface::GenerateKeyFrame** — `api/rtp_sender_interface.h:139`,
`virtual RTCError GenerateKeyFrame(const std::vector<std::string>& rids)`.
Has a default body ("make pure virtual again after Chrome roll"), so it exists
on every sender; `pc/rtp_sender.h:412,478` and the proxy (`rtp_sender_proxy.h:64`)
implement it. Empty `rids` = every layer. Must be called on the signaling
thread; `webrtc::Thread::PostTask` takes `absl::AnyInvocable<void() &&>`
(`api/task_queue/task_queue_base.h:65`), so a lambda converts.

✅ **ui::ScopedClipboardWriter** — `ui/base/clipboard/scoped_clipboard_writer.h:37`,
`explicit ScopedClipboardWriter(ClipboardBuffer buffer,
std::unique_ptr<DataTransferEndpoint> src = nullptr)`; `WriteText(std::u16string_view)`
at `:52`. The write commits in the destructor, which is also when
`ClipboardMonitor::NotifyClipboardDataChanged` fires (`clipboard_ozone.cc:408`).

✅ **ui::Clipboard::ReadText** — `ui/base/clipboard/clipboard.h:204`,
`virtual void ReadText(ClipboardBuffer, const std::optional<DataTransferEndpoint>&,
ReadTextCallback) const = 0`, with `ReadTextCallback =
base::OnceCallback<void(std::u16string)>` (`:54`). Asynchronous — there is no
synchronous text read on this interface. `GetForCurrentThread()` at `:137`;
`ClipboardOzone` DCHECKs the calling thread, so UI thread only.

✅ **ui::ClipboardBuffer** — `ui/base/clipboard/clipboard_buffer.h:13`,
`enum class ClipboardBuffer { kCopyPaste, kSelection, kDrag }`.

✅ **ui::ClipboardMonitor / ClipboardObserver** —
`clipboard_monitor.h:25,31,34` (`GetInstance`, `AddObserver`, `RemoveObserver`);
`clipboard_observer.h:17` `virtual void OnClipboardDataChanged()`, destructor
protected. GN target: `//ui/base/clipboard` (`component("clipboard")`,
`ui/base/clipboard/BUILD.gn:109`); `//ui/base` does NOT re-export it on Linux
(`ui/base/BUILD.gn:580` adds it only for mac/win), so depend on it directly.

✅ **base::CommandLine::GetSwitchValuePath** — `base/command_line.h:206`,
`FilePath GetSwitchValuePath(std::string_view) const`.

✅ **WebContents::GetTitle / GetLastCommittedURL** — `web_contents.h:720`
`virtual const std::u16string& GetTitle()` (non-const), `:474`
`virtual const GURL& GetLastCommittedURL() const`. `DevToolsAgentHost::GetId`
at `devtools_agent_host.h:199`.

---

## Re-reading these pins

```bash
kubectl run -n chromeless-build cb-api-recon --image=busybox:1.36 --restart=Never \
  --overrides='{"spec":{"nodeName":"triform-7","tolerations":[{"operator":"Exists"}],
  "volumes":[{"name":"src","hostPath":{"path":"/var/lib/longhorn/chromeless-build/chromium-src","type":"Directory"}}],
  "containers":[{"name":"recon","image":"busybox:1.36","command":["sleep","3600"],
  "volumeMounts":[{"name":"src","mountPath":"/work/src","readOnly":true}]}]}}'

kubectl exec -n chromeless-build cb-api-recon -- \
  grep -n -A12 'virtual.*AddNewContents' \
  /work/src/src/chromium/src/content/public/browser/web_contents_delegate.h

kubectl delete pod -n chromeless-build cb-api-recon
```
Read-only mount; safe to run against a live build tree. Costs seconds, versus
~4 h per wrong guess.

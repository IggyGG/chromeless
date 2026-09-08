// The ask-a-human channel for the chromeless client.
//
// Wraps the "control" RTCDataChannel. Wire format:
// docs/protocols/cb-cdp-methods.md and the header comment in
// capture/build-integration/cb_control_channel.h.
//
// Everything else the guest sends is telemetry or pixels. This is the one
// path where the browser STOPS and waits for a person: a page calls
// confirm(), and chromium hands the guest a callback it will not proceed
// without. Without a consumer on this channel the guest applies its own
// safe default and the page silently takes the "cancel" branch — which is
// exactly what happens today, because nothing here handled the label.
//
// ─── THE INVARIANT THAT MATTERS ───────────────────────────────────────
//
// The guest NEVER depends on us to make progress. It arms a deadline
// before sending (cb_control_channel.cc, "Arm the deadline BEFORE
// sending"), resolves synchronously with the default when the channel is
// closed, and cancels everything outstanding on teardown. So a client that
// is slow, absent, or buggy degrades to today's behaviour rather than
// wedging a renderer.
//
// That cuts both ways: a response we send after the guest's deadline is
// dropped with a log on its side (unknown id), not applied late. Answering
// is best-effort by construction.

import type { SessionLogLevel } from "./session.js";

export const PROTOCOL_VERSION = 1 as const;

// Page-controlled text reaches us here. The guest already truncates to
// 4 KiB (cb_javascript_dialog_manager.cc kMaxMessageChars) so this cap is
// a second line of defence, not the primary one — but a hostile guest is
// not the threat model we can assume away, and rendering megabytes into
// the DOM is a denial of service against the viewer's own browser.
export const MAX_TEXT_CHARS = 8192;

export type ControlEnvelopeType = "ui_request" | "ui_response" | "ui_event";

export interface ControlRequestData {
  id: string;
  kind: string;
  deadline_ms?: number;
  // js_dialog payload — present when kind === "js_dialog".
  dialog_type?: "alert" | "confirm" | "prompt" | "beforeunload";
  message?: string;
  default_prompt?: string;
  origin?: string;
  is_reload?: boolean;
  // file_chooser payload — present when kind === "file_chooser". The page
  // clicked <input type=file>; the guest has parked chromium's
  // FileSelectListener and is waiting for us to send bytes on the "files"
  // channel. `accept` mirrors the input's accept attribute and is advisory:
  // the page's own validation is what decides, so filtering here could only
  // disagree with it.
  accept?: string[];
  multiple?: boolean;
  title?: string;
}

export interface ControlRequestEnvelope {
  v: typeof PROTOCOL_VERSION;
  type: "ui_request";
  t: number;
  seq: number;
  data: ControlRequestData;
}

export interface ControlResponseData {
  id: string;
  // For js_dialog: the user pressed OK. For file_chooser: a file is on its
  // way over the "files" channel. In BOTH cases `false` or an absent field
  // means "resolve with your default" — for the chooser that is the page
  // seeing "no file selected", exactly as if a human had closed the picker.
  accept?: boolean;
  prompt_text?: string;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

// A request we are willing to act on. Deliberately strict on the envelope
// and lenient on `kind`: the guest treats kind as a free-form string
// (cb_control_channel.h SendRequest), and four kinds are documented but
// unimplemented. An unknown kind must reach the caller so it can be
// declined explicitly rather than silently ignored — see the note on
// respondDecline below.
export function isValidRequest(env: unknown): env is ControlRequestEnvelope {
  if (!env || typeof env !== "object") return false;
  const e = env as Partial<ControlRequestEnvelope>;
  if (e.v !== PROTOCOL_VERSION) return false;
  if (e.type !== "ui_request") return false;
  if (typeof e.t !== "number" || typeof e.seq !== "number") return false;
  const d = e.data as Partial<ControlRequestData> | undefined;
  if (!d || typeof d !== "object") return false;
  // id is a STRING on the wire — the guest mints it with NumberToString
  // (cb_control_channel.cc). A number here means a different producer, so
  // reject rather than coerce.
  if (typeof d.id !== "string" || d.id === "") return false;
  if (typeof d.kind !== "string" || d.kind === "") return false;
  return true;
}

// Fire-and-forget notices. No id, no reply, no bookkeeping — by definition
// nothing depends on us receiving one.
export function isValidEvent(env: unknown): boolean {
  if (!env || typeof env !== "object") return false;
  const e = env as { v?: unknown; type?: unknown };
  return e.v === PROTOCOL_VERSION && e.type === "ui_event";
}

// Truncate for display. Returns the original string when short enough so
// the common path allocates nothing.
export function clampText(s: unknown): string {
  if (typeof s !== "string") return "";
  return s.length <= MAX_TEXT_CHARS ? s : `${s.slice(0, MAX_TEXT_CHARS)}…`;
}

// ---------------------------------------------------------------------------
// Response envelopes
// ---------------------------------------------------------------------------

let nextSeq = 0;

export function buildResponse(data: ControlResponseData): string {
  return JSON.stringify({
    v: PROTOCOL_VERSION,
    type: "ui_response" satisfies ControlEnvelopeType,
    t: Date.now(),
    seq: nextSeq++,
    data,
  });
}

// Exposed for tests, which would otherwise inherit seq state across cases.
export function resetSeqForTesting(): void {
  nextSeq = 0;
}

// ---------------------------------------------------------------------------
// Prompt presentation
// ---------------------------------------------------------------------------

export interface ControlPrompt {
  id: string;
  kind: string;
  dialogType: string;
  message: string;
  defaultPrompt: string;
  origin: string;
}

export interface ControlChannelHandle {
  dispose(): void;
  // Test seam: feed a raw frame as if it arrived on the wire.
  handleFrameForTesting(raw: string): void;
}

// What the guest told us about the file input the page opened.
export interface FileChooserRequest {
  id: string;
  // MIME types / extensions from the input's accept attribute. Advisory.
  accept: string[];
  multiple: boolean;
  title: string;
}

export interface AttachControlOptions {
  // Where to mount the prompt. Defaults to document.body.
  mount?: HTMLElement;
  // Injectable for tests; defaults to the DOM renderer below.
  present?: (prompt: ControlPrompt, respond: (r: ControlResponseData) => void) => () => void;
  // Called when the streamed page opens a file picker. Return true if a
  // file is being sent on the "files" channel, false to decline — the
  // page then sees "no file selected".
  //
  // Absent, every chooser is declined. That is the honest default: without
  // a picker on this side there is no file to send, and leaving the guest
  // to wait out its five-minute deadline would look like a frozen page.
  onFileChooser?: (req: FileChooserRequest) => Promise<boolean> | boolean;
  log?: (level: SessionLogLevel, msg: string, extra?: unknown) => void;
}

// Build the prompt view model from a validated request. Pure, so the
// truncation and defaulting rules are testable without a DOM.
export function toPrompt(d: ControlRequestData): ControlPrompt {
  return {
    id: d.id,
    kind: d.kind,
    dialogType: typeof d.dialog_type === "string" ? d.dialog_type : "confirm",
    message: clampText(d.message),
    defaultPrompt: clampText(d.default_prompt),
    origin: clampText(d.origin),
  };
}

// The default DOM presenter. Plain elements — the standalone client has no
// framework, and main.ts builds its log lines the same way.
//
// Returns a teardown function. Callers MUST call it: a prompt left mounted
// after the channel closes is a dialog the user can answer into a void.
function presentInDom(
  mount: HTMLElement,
  prompt: ControlPrompt,
  respond: (r: ControlResponseData) => void,
): () => void {
  const overlay = document.createElement("div");
  overlay.className = "cb-control-overlay";
  overlay.setAttribute("role", "dialog");
  overlay.setAttribute("aria-modal", "true");
  overlay.setAttribute("aria-label", "Page dialog");

  const box = document.createElement("div");
  box.className = "cb-control-box";

  const originEl = document.createElement("div");
  originEl.className = "cb-control-origin";
  // textContent, never innerHTML: message and origin are page-controlled.
  originEl.textContent = prompt.origin || "(unknown origin)";

  const msgEl = document.createElement("p");
  msgEl.className = "cb-control-message";
  msgEl.textContent =
    prompt.dialogType === "beforeunload"
      ? "Leave this page? Changes you made may not be saved."
      : prompt.message;

  box.appendChild(originEl);
  box.appendChild(msgEl);

  let input: HTMLInputElement | null = null;
  if (prompt.dialogType === "prompt") {
    input = document.createElement("input");
    input.type = "text";
    input.className = "cb-control-input";
    input.value = prompt.defaultPrompt;
    box.appendChild(input);
  }

  const actions = document.createElement("div");
  actions.className = "cb-control-actions";

  let done = false;
  const finish = (accept: boolean) => {
    // Exactly-once. The guest drops a duplicate id, but sending one means
    // we lost track of our own state.
    if (done) return;
    done = true;
    const r: ControlResponseData = { id: prompt.id, accept };
    if (accept && input) r.prompt_text = input.value;
    respond(r);
    teardown();
  };

  // alert() has no cancel branch — dismissing IS acknowledging, and
  // offering a Cancel that maps to the same outcome would be a lie.
  if (prompt.dialogType !== "alert") {
    const cancel = document.createElement("button");
    cancel.type = "button";
    cancel.className = "cb-control-cancel";
    cancel.textContent = prompt.dialogType === "beforeunload" ? "Stay" : "Cancel";
    cancel.addEventListener("click", () => finish(false));
    actions.appendChild(cancel);
  }

  const ok = document.createElement("button");
  ok.type = "button";
  ok.className = "cb-control-ok";
  ok.textContent = prompt.dialogType === "beforeunload" ? "Leave" : "OK";
  ok.addEventListener("click", () => finish(true));
  actions.appendChild(ok);

  box.appendChild(actions);
  overlay.appendChild(box);
  mount.appendChild(overlay);

  (input ?? ok).focus();

  // Enter = OK, Escape = Cancel. Every native dialog this replaces has this
  // behaviour, so a user who types an answer into prompt() and presses Enter
  // reasonably expects it to submit. Without it the only way to answer is to
  // find and click the button — and for a prompt the keyboard is already
  // where their hands are.
  //
  // Bound on the OVERLAY, not on window: the overlay holds focus (see the
  // .focus() above), a keydown inside it bubbles here, and a window-level
  // listener would also fire for keystrokes the user aims at the remote page
  // through the input channel.
  //
  // Escape is ignored for alert(), which has no cancel branch — mapping it to
  // finish(false) would send accept:false for a dialog whose only honest
  // answer is "acknowledged", and the guest's default for alert is true.
  const onKeyDown = (ev: KeyboardEvent) => {
    if (ev.key === "Enter") {
      // Not for a multi-line value; there is no textarea here, but guard the
      // composition case so an IME's Enter does not submit mid-word.
      if (ev.isComposing) return;
      ev.preventDefault();
      finish(true);
    } else if (ev.key === "Escape" && prompt.dialogType !== "alert") {
      ev.preventDefault();
      finish(false);
    }
  };
  overlay.addEventListener("keydown", onKeyDown);

  function teardown(): void {
    // Remove the listener explicitly rather than relying on the element being
    // detached. finish() calls teardown() and a stale handler on a detached
    // node that something still references would keep firing into a closure
    // whose `done` guard silently swallows it — a bug that looks like a dead
    // keyboard rather than a leak.
    try {
      overlay.removeEventListener("keydown", onKeyDown);
    } catch {
      /* never throws in practice; belt and braces on teardown paths */
    }
    try {
      overlay.remove();
    } catch {
      /* already detached */
    }
  }
  return teardown;
}

// ---------------------------------------------------------------------------
// Channel attachment
// ---------------------------------------------------------------------------

export function attachControlChannel(
  dc: RTCDataChannel,
  opts: AttachControlOptions = {},
): ControlChannelHandle {
  const log = opts.log ?? (() => {});
  const onFileChooser = opts.onFileChooser;
  const mount = opts.mount ?? (typeof document !== "undefined" ? document.body : null);
  const present =
    opts.present ??
    ((p, respond) => {
      if (!mount) return () => {};
      return presentInDom(mount, p, respond);
    });

  // At most one prompt at a time. The guest can have several requests in
  // flight (each has its own deadline), but stacking modals on a viewer is
  // worse than answering them in order — and a second dialog appearing
  // under the first is how a user answers the wrong question.
  let openTeardown: (() => void) | null = null;
  let openId: string | null = null;
  let disposed = false;

  const send = (r: ControlResponseData): void => {
    if (dc.readyState !== "open") {
      // Not an error: the guest's deadline or CancelAllPending already
      // resolved this one. Say so plainly rather than warning.
      log("warn", `control: channel not open, dropping response for id=${r.id}`);
      return;
    }
    try {
      dc.send(buildResponse(r));
    } catch (err) {
      log("err", `control: send failed for id=${r.id}: ${String(err)}`);
    }
  };

  const closeOpen = (): void => {
    if (openTeardown) {
      try {
        openTeardown();
      } catch {
        /* ignore */
      }
    }
    openTeardown = null;
    openId = null;
  };

  const onFrame = (raw: string): void => {
    if (disposed) return;

    let parsed: unknown;
    try {
      parsed = JSON.parse(raw);
    } catch {
      log("warn", "control: inbound frame is not JSON — dropped");
      return;
    }

    if (isValidEvent(parsed)) {
      // fullscreen_changed today; nothing to answer. Logged so a future
      // event type is visible rather than invisible.
      const k = (parsed as { data?: { kind?: unknown } }).data?.kind;
      log("ok", `control: event ${typeof k === "string" ? k : "(unknown)"}`);
      return;
    }

    if (!isValidRequest(parsed)) {
      log("warn", "control: inbound frame is not a valid ui_request — dropped");
      return;
    }

    const d = parsed.data;

    // The page opened <input type=file>. The guest is holding chromium's
    // FileSelectListener and will not let the page proceed until we answer.
    if (d.kind === "file_chooser") {
      const req: FileChooserRequest = {
        id: d.id,
        accept: Array.isArray(d.accept)
          ? d.accept.filter((a): a is string => typeof a === "string")
          : [],
        multiple: d.multiple === true,
        title: clampText(d.title),
      };
      if (!onFileChooser) {
        log("warn", "control: file_chooser with no handler — declining");
        send({ id: d.id, accept: false });
        return;
      }
      // Resolve the promise into a single response. A handler that throws
      // must still produce an answer, or the page waits out the guest's
      // deadline staring at a picker that already closed.
      void (async () => {
        let accepted = false;
        try {
          accepted = (await onFileChooser(req)) === true;
        } catch (err) {
          log("err", `control: file_chooser handler threw: ${String(err)}`);
        }
        send({ id: d.id, accept: accepted });
      })();
      return;
    }

    // Only js_dialog and file_chooser have producers today. An unknown kind
    // is DECLINED explicitly rather than ignored: the guest is blocking on
    // it, and an empty-dict response makes it apply its safe default
    // immediately instead of waiting out the full deadline.
    if (d.kind !== "js_dialog") {
      log("warn", `control: declining unsupported kind "${d.kind}"`);
      send({ id: d.id });
      return;
    }

    // A second request while one is open: answer the older one with the
    // safe default so the guest is not left waiting, then show the new one.
    if (openId) {
      log("warn", `control: superseding open prompt id=${openId}`);
      send({ id: openId });
      closeOpen();
    }

    const prompt = toPrompt(d);
    openId = prompt.id;
    openTeardown = present(prompt, (r) => {
      // The presenter tears itself down; just drop our handle so a later
      // close() does not double-teardown.
      openTeardown = null;
      openId = null;
      send(r);
    });
    log("ok", `control: prompt ${prompt.dialogType} from ${prompt.origin || "?"}`);
  };

  const onMessage = (ev: MessageEvent): void => {
    if (typeof ev.data !== "string") return;
    onFrame(ev.data);
  };

  const onClose = (): void => {
    // Drop any open prompt. Answering it would be pointless — the guest
    // has already applied its default — and leaving it on screen invites
    // the user to answer a question nobody is listening to.
    closeOpen();
  };

  dc.addEventListener("message", onMessage);
  dc.addEventListener("close", onClose);

  return {
    dispose(): void {
      if (disposed) return;
      disposed = true;
      dc.removeEventListener("message", onMessage);
      dc.removeEventListener("close", onClose);
      closeOpen();
    },
    handleFrameForTesting(raw: string): void {
      onFrame(raw);
    },
  };
}

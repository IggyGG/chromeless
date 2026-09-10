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
  // permission payload — kind === "permission". The guest sends readable
  // names ("geolocation", "notifications"), already mapped from blink's
  // PermissionDescriptor, so the client never sees a raw descriptor.
  permissions?: string[];
  // login payload — kind === "login". `realm` is SERVER-supplied text shown
  // to someone deciding whether to type a password, so it is truncated and
  // never rendered as markup. `first_attempt` is false on a retry, i.e. the
  // last credentials were rejected — without saying so the second prompt
  // looks identical to the first and the user retypes the same password.
  realm?: string;
  scheme?: string;
  is_proxy?: boolean;
  first_attempt?: boolean;
  // cert_error payload — kind === "cert_error".
  cert_error?: number;
  subject?: string;
  issuer?: string;
  is_main_frame?: boolean;
  // Shared by permission / cert_error / login. `origin` is already declared
  // above for js_dialog and means the same thing here.
  url?: string;
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
  // permission: an explicit true grants; anything else denies.
  granted?: boolean;
  // cert_error: an explicit true proceeds past the certificate.
  proceed?: boolean;
  // login: BOTH must be present or the guest treats it as cancelled, which
  // //content turns back into the 401.
  username?: string;
  password?: string;
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

export interface PermissionRequest {
  id: string;
  /** Readable names, already mapped from blink's descriptors by the guest. */
  permissions: string[];
  /** The site asking. Empty only if the guest could not resolve a frame. */
  origin: string;
}

export interface CertErrorRequest {
  id: string;
  url: string;
  /** chromium's net error code, negative. Kept for the log; see certErrorText. */
  certError: number;
  subject: string;
  issuer: string;
  isMainFrame: boolean;
}

export interface LoginRequest {
  id: string;
  url: string;
  /** Server-supplied. Truncated, and never rendered as markup. */
  realm: string;
  scheme: string;
  isProxy: boolean;
  /** False on a retry: the previous credentials were rejected. */
  firstAttempt: boolean;
}

/**
 * The handful of TLS errors worth naming. Everything else falls back to the
 * number, which is still greppable against net_error_list.h — better than
 * inventing prose for an error we have not thought about.
 */
export function certErrorText(code: number): string {
  // Read from net/base/net_error_list.h on the pinned tree, NOT remembered:
  // a first draft had -200 as "unknown authority" and -202 as "name
  // mismatch", which are exactly swapped, plus REVOKED and WEAK at the
  // wrong numbers. A prompt that names the wrong reason is worse than one
  // that names none, because it invites a decision on false grounds.
  switch (code) {
    case -200: return "the certificate does not match this site's name";
    case -201: return "the certificate has expired or is not yet valid";
    case -202: return "the certificate is signed by an unknown authority";
    case -206: return "the certificate has been revoked";
    case -208: return "the certificate is weakly signed";
    default:   return `certificate error ${code}`;
  }
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
  // A guest ui_event. `fullscreen_changed {fullscreen}` fires when the
  // streamed page enters or leaves the Fullscreen API; tab_opened /
  // tab_closed are advisory nudges. Absent, events are logged and dropped,
  // which is what happened to fullscreen_changed for its whole existence:
  // the guest sent it, nothing listened, and the viewer's chrome stayed put
  // while the page believed it was fullscreen.
  onEvent?: (kind: string, data: Record<string, unknown>) => void;

  // The streamed page asked for a permission (geolocation, notifications,
  // camera...). Return true to grant. Absent, permissions are DENIED — which
  // is what the guest did unconditionally before batch C, so an unhandled
  // kind is no worse than the status quo and never accidentally permissive.
  onPermission?: (req: PermissionRequest) => Promise<boolean> | boolean;

  // A TLS error. Return true to proceed past it. Absent, the navigation is
  // cancelled, which is chromium's own default.
  //
  // The guest never sends this when HSTS applies — the site has said its
  // certificate must be valid, so there is no legitimate "proceed anyway".
  onCertError?: (req: CertErrorRequest) => Promise<boolean> | boolean;

  // HTTP 401/407. Return credentials, or null to cancel (the guest turns
  // that back into the 401 the page would otherwise have shown).
  onLogin?: (req: LoginRequest) =>
    Promise<{ username: string; password: string } | null>
    | { username: string; password: string } | null;
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
  const onEvent = opts.onEvent;
  const onPermission = opts.onPermission;
  const onCertError = opts.onCertError;
  const onLogin = opts.onLogin;

  /**
   * Run a handler and answer the guest exactly once.
   *
   * Shared by all three of batch C's blocking kinds because the failure
   * modes are identical and easy to get subtly different: no handler, a
   * throwing handler, and a rejected promise must all produce the SAFE
   * answer rather than silence. The guest is holding a chromium callback
   * whose own contract says it must run.
   */
  function settleWith<Req, Res>(
    id: string,
    handler: ((req: Req) => Promise<Res> | Res) | undefined,
    req: Req,
    kindName: string,
    toResponse: (result: Res | undefined) => ControlResponseData,
  ): void {
    if (!handler) {
      log("warn", `control: ${kindName} with no handler — declining`);
      send(toResponse(undefined));
      return;
    }
    void (async () => {
      let result: Res | undefined;
      try {
        result = await handler(req);
      } catch (err) {
        log("err", `control: ${kindName} handler threw: ${String(err)}`);
      }
      send(toResponse(result));
    })();
  }
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
      // Fire-and-forget: nothing to answer, but not nothing to DO. Logged
      // either way so an event kind nobody handles is visible rather than
      // invisible.
      const d = (parsed as { data?: Record<string, unknown> }).data ?? {};
      const k = d["kind"];
      const kind = typeof k === "string" ? k : "(unknown)";
      log("ok", `control: event ${kind}`);
      if (onEvent && typeof k === "string") {
        try {
          onEvent(k, d);
        } catch (err) {
          // A throwing handler must not kill the channel: every later
          // frame, including the requests the guest BLOCKS on, arrives
          // through this same callback.
          log("err", `control: event handler threw for ${kind}: ${String(err)}`);
        }
      }
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

    // Batch C's three blocking kinds. Each answers EXACTLY ONCE — the guest
    // is holding a chromium callback that its own contract says must run,
    // and a dropped answer is a page (or a network request) that hangs
    // until the deadline.
    //
    // Every one of them denies by default: no handler, a handler that
    // throws, or a handler that returns the wrong shape. That is the same
    // outcome the guest produced before batch C existed, so an unhandled
    // kind is never a regression and never accidentally permissive.
    if (d.kind === "permission") {
      const req: PermissionRequest = {
        id: d.id,
        permissions: Array.isArray(d.permissions)
          ? d.permissions.filter((x): x is string => typeof x === "string")
          : [],
        origin: clampText(d.origin),
      };
      settleWith(d.id, onPermission, req, "permission",
                 (ok) => ({ id: d.id, granted: ok === true }));
      return;
    }

    if (d.kind === "cert_error") {
      const req: CertErrorRequest = {
        id: d.id,
        url: clampText(d.url),
        certError: typeof d.cert_error === "number" ? d.cert_error : 0,
        subject: clampText(d.subject),
        issuer: clampText(d.issuer),
        isMainFrame: d.is_main_frame === true,
      };
      settleWith(d.id, onCertError, req, "cert_error",
                 (ok) => ({ id: d.id, proceed: ok === true }));
      return;
    }

    if (d.kind === "login") {
      const req: LoginRequest = {
        id: d.id,
        url: clampText(d.url),
        realm: clampText(d.realm),
        scheme: clampText(d.scheme),
        isProxy: d.is_proxy === true,
        firstAttempt: d.first_attempt !== false,
      };
      settleWith(d.id, onLogin, req, "login", (creds) => {
        // BOTH fields or neither: the guest treats a partial answer as
        // cancelled, and sending half a credential would look like a bug
        // rather than a decision.
        if (creds && typeof creds === "object" &&
            typeof creds.username === "string" &&
            typeof creds.password === "string") {
          return { id: d.id, username: creds.username,
                   password: creds.password };
        }
        return { id: d.id };
      });
      return;
    }

    // Only js_dialog and file_chooser remain. An unknown kind is DECLINED
    // explicitly rather than ignored: the guest is blocking on it, and an
    // empty-dict response makes it apply its safe default immediately
    // instead of waiting out the full deadline.
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

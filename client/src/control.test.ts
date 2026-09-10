// Unit tests for client/src/control.ts.
//
// The envelopes here are copied from what the guest actually emits
// (capture/build-integration/cb_control_channel.cc SendRequest and
// cb_javascript_dialog_manager.cc), not from the prose spec — a test
// written against the doc would still pass if the doc were wrong.

import { describe, it, expect, beforeEach, vi } from "vitest";
import {
  attachControlChannel,
  buildResponse,
  clampText,
  isValidEvent,
  isValidRequest,
  resetSeqForTesting,
  toPrompt,
  MAX_TEXT_CHARS,
  PROTOCOL_VERSION,
  type ControlPrompt,
  type ControlResponseData,
  type FileChooserRequest,
  type PermissionRequest,
  type LoginRequest,
  certErrorText,
} from "./control.js";

class FakeChannel {
  readyState: "connecting" | "open" | "closing" | "closed" = "open";
  sent: string[] = [];
  private handlers: { [k: string]: Array<(e: unknown) => void> } = {};
  send(d: string): void { this.sent.push(d); }
  addEventListener(type: string, h: (e: unknown) => void): void {
    (this.handlers[type] ??= []).push(h);
  }
  removeEventListener(type: string, h: (e: unknown) => void): void {
    const hs = this.handlers[type];
    if (!hs) return;
    const i = hs.indexOf(h);
    if (i >= 0) hs.splice(i, 1);
  }
  pushMessage(raw: string): void {
    for (const h of this.handlers["message"] ?? []) h({ data: raw });
  }
  fireClose(): void {
    for (const h of this.handlers["close"] ?? []) h({});
  }
  listenerCount(type: string): number {
    return (this.handlers[type] ?? []).length;
  }
}

// Exactly the shape cb_control_channel.cc builds: envelope {v,type,t,seq,data}
// with id/kind/deadline_ms merged INTO data alongside the payload.
function guestRequest(over: Record<string, unknown> = {}): string {
  return JSON.stringify({
    v: 1,
    type: "ui_request",
    t: 1_700_000_000_000,
    seq: 1,
    data: {
      id: "1",
      kind: "js_dialog",
      deadline_ms: 60000,
      dialog_type: "confirm",
      message: "Delete everything?",
      default_prompt: "",
      origin: "https://example.test",
      ...over,
    },
  });
}

beforeEach(() => resetSeqForTesting());

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

describe("isValidRequest", () => {
  it("accepts the envelope the guest actually emits", () => {
    expect(isValidRequest(JSON.parse(guestRequest()))).toBe(true);
  });

  it("rejects a wrong protocol version", () => {
    expect(isValidRequest(JSON.parse(guestRequest().replace('"v":1', '"v":2')))).toBe(false);
  });

  it("rejects a response or event masquerading as a request", () => {
    const base = JSON.parse(guestRequest());
    expect(isValidRequest({ ...base, type: "ui_response" })).toBe(false);
    expect(isValidRequest({ ...base, type: "ui_event" })).toBe(false);
  });

  it("rejects a missing or empty id", () => {
    const base = JSON.parse(guestRequest());
    expect(isValidRequest({ ...base, data: { ...base.data, id: "" } })).toBe(false);
    const { id: _drop, ...noId } = base.data;
    expect(isValidRequest({ ...base, data: noId })).toBe(false);
  });

  // The guest mints ids with NumberToString. A numeric id means a different
  // producer, and coercing it would let us answer with a mismatched type.
  it("rejects a NUMERIC id rather than coercing it", () => {
    const base = JSON.parse(guestRequest());
    expect(isValidRequest({ ...base, data: { ...base.data, id: 1 } })).toBe(false);
  });

  it("rejects a missing kind", () => {
    const base = JSON.parse(guestRequest());
    const { kind: _drop, ...noKind } = base.data;
    expect(isValidRequest({ ...base, data: noKind })).toBe(false);
  });

  it("accepts an UNKNOWN kind — the caller declines it explicitly", () => {
    expect(isValidRequest(JSON.parse(guestRequest({ kind: "cert_error" })))).toBe(true);
  });

  it("rejects junk", () => {
    expect(isValidRequest(null)).toBe(false);
    expect(isValidRequest("nope")).toBe(false);
    expect(isValidRequest({})).toBe(false);
  });
});

describe("isValidEvent", () => {
  it("accepts a fire-and-forget notice", () => {
    expect(isValidEvent({ v: 1, type: "ui_event", t: 1, seq: 1, data: { kind: "fullscreen_changed" } })).toBe(true);
  });
  it("rejects a request", () => {
    expect(isValidEvent(JSON.parse(guestRequest()))).toBe(false);
  });
});

// ---------------------------------------------------------------------------
// Text handling
// ---------------------------------------------------------------------------

describe("clampText", () => {
  it("passes short text through unchanged", () => {
    expect(clampText("hello")).toBe("hello");
  });
  it("truncates page-controlled text", () => {
    const out = clampText("x".repeat(MAX_TEXT_CHARS + 500));
    expect(out.length).toBe(MAX_TEXT_CHARS + 1); // + the ellipsis
    expect(out.endsWith("…")).toBe(true);
  });
  it("coerces non-strings to empty", () => {
    expect(clampText(undefined)).toBe("");
    expect(clampText(42)).toBe("");
  });
});

describe("toPrompt", () => {
  it("defaults a missing dialog_type to confirm — never to alert", () => {
    // alert has no cancel branch, so defaulting there would remove the
    // user's ability to decline.
    const p = toPrompt({ id: "1", kind: "js_dialog" });
    expect(p.dialogType).toBe("confirm");
  });
});

// ---------------------------------------------------------------------------
// Response envelopes
// ---------------------------------------------------------------------------

describe("buildResponse", () => {
  it("emits the envelope the guest validates against", () => {
    const env = JSON.parse(buildResponse({ id: "7", accept: true }));
    expect(env.v).toBe(PROTOCOL_VERSION);
    expect(env.type).toBe("ui_response");
    expect(typeof env.t).toBe("number");
    expect(typeof env.seq).toBe("number");
    expect(env.data).toEqual({ id: "7", accept: true });
  });

  it("increments seq per response", () => {
    const a = JSON.parse(buildResponse({ id: "1", accept: true }));
    const b = JSON.parse(buildResponse({ id: "2", accept: false }));
    expect(b.seq).toBeGreaterThan(a.seq);
  });
});

// ---------------------------------------------------------------------------
// Channel behaviour
// ---------------------------------------------------------------------------

describe("attachControlChannel", () => {
  // A presenter that captures the prompt and lets the test answer it.
  function capturing() {
    const seen: ControlPrompt[] = [];
    let answer: ((r: ControlResponseData) => void) | null = null;
    let tornDown = 0;
    const present = (p: ControlPrompt, respond: (r: ControlResponseData) => void) => {
      seen.push(p);
      answer = respond;
      return () => { tornDown++; };
    };
    return {
      present,
      seen,
      answerWith: (r: ControlResponseData) => answer?.(r),
      get tornDown() { return tornDown; },
    };
  }

  it("presents a js_dialog request and sends the user's answer", () => {
    const dc = new FakeChannel();
    const cap = capturing();
    const h = attachControlChannel(dc as unknown as RTCDataChannel, { present: cap.present });

    dc.pushMessage(guestRequest());
    expect(cap.seen).toHaveLength(1);
    expect(cap.seen[0]!.message).toBe("Delete everything?");
    expect(cap.seen[0]!.origin).toBe("https://example.test");

    cap.answerWith({ id: "1", accept: true });
    expect(dc.sent).toHaveLength(1);
    const env = JSON.parse(dc.sent[0]!);
    expect(env.type).toBe("ui_response");
    expect(env.data).toEqual({ id: "1", accept: true });
    h.dispose();
  });

  // The guest BLOCKS on every request it sends. Ignoring an unsupported
  // kind would make it wait out the full deadline; an empty-dict response
  // makes it apply its safe default immediately.
  //
  // The example has moved twice now, which is the test working: it was
  // `file_chooser` until batch B implemented it, then `cert_error` until
  // batch C did. Batch C implemented every kind the protocol documents, so
  // the example is now a deliberately INVENTED kind — the guest treats kind
  // as a free-form string (cb_control_channel.h SendRequest), so an unknown
  // one is a real case the client must decline rather than ignore, not a
  // placeholder waiting to be implemented.
  //
  // If you add a kind called "not_a_real_kind", you have earned this.
  it("DECLINES an unsupported kind immediately instead of ignoring it", () => {
    const dc = new FakeChannel();
    const cap = capturing();
    const h = attachControlChannel(dc as unknown as RTCDataChannel, { present: cap.present });

    dc.pushMessage(guestRequest({ kind: "not_a_real_kind", id: "9" }));

    expect(cap.seen).toHaveLength(0);
    expect(dc.sent).toHaveLength(1);
    const env = JSON.parse(dc.sent[0]!);
    expect(env.data).toEqual({ id: "9" }); // no `accept` => guest's default
    h.dispose();
  });

  // ui_event: fire-and-forget notices. fullscreen_changed is the one that
  // matters — the guest has emitted it since it was written and nothing
  // listened, so a page that went fullscreen did so inside a window that
  // still showed the sidebar.
  describe("ui_event", () => {
    const guestEvent = (kind: string, extra: Record<string, unknown> = {}) =>
      JSON.stringify({ v: 1, type: "ui_event", t: 1, seq: 1,
                       data: { kind, ...extra } });

    it("hands the kind and data to onEvent", () => {
      const dc = new FakeChannel();
      const seen: Array<[string, Record<string, unknown>]> = [];
      const h = attachControlChannel(dc as unknown as RTCDataChannel, {
        onEvent: (k, d) => { seen.push([k, d]); },
      });

      dc.pushMessage(guestEvent("fullscreen_changed", { fullscreen: true }));

      expect(seen).toHaveLength(1);
      expect(seen[0]![0]).toBe("fullscreen_changed");
      expect(seen[0]![1]["fullscreen"]).toBe(true);
      // An event is fire-and-forget: answering one would be a protocol error.
      expect(dc.sent).toHaveLength(0);
      h.dispose();
    });

    it("survives a handler that THROWS", () => {
      const dc = new FakeChannel();
      let requests = 0;
      const h = attachControlChannel(dc as unknown as RTCDataChannel, {
        onEvent: () => { throw new Error("handler exploded"); },
        onFileChooser: () => { requests++; return false; },
      });

      dc.pushMessage(guestEvent("fullscreen_changed", { fullscreen: true }));
      // The channel must still be alive: every later frame arrives through
      // the same callback, INCLUDING the requests the guest blocks on.
      dc.pushMessage(guestRequest({ kind: "file_chooser", id: "20" }));

      expect(requests).toBe(1);
      h.dispose();
    });

    it("does not invoke onEvent for a ui_request", () => {
      const dc = new FakeChannel();
      const seen: string[] = [];
      const cap = capturing();
      const h = attachControlChannel(dc as unknown as RTCDataChannel, {
        present: cap.present,
        onEvent: (k) => { seen.push(k); },
      });

      dc.pushMessage(guestRequest({ kind: "js_dialog", id: "21" }));

      expect(seen).toEqual([]);
      expect(cap.seen).toHaveLength(1);
      h.dispose();
    });
  });

  // Batch C's three blocking kinds. Each holds a chromium callback on the
  // guest side whose contract says it MUST run, so the property under test
  // is the same for all three: exactly one answer, always, and a safe one
  // when anything goes wrong.
  describe("permission", () => {
    it("grants only on an explicit true", async () => {
      const dc = new FakeChannel();
      const h = attachControlChannel(dc as unknown as RTCDataChannel, {
        onPermission: () => true,
      });
      dc.pushMessage(guestRequest({
        kind: "permission", id: "30",
        permissions: ["geolocation"], origin: "https://example.com",
      }));
      await Promise.resolve(); await Promise.resolve();
      expect(JSON.parse(dc.sent[0]!).data).toEqual({ id: "30", granted: true });
      h.dispose();
    });

    it("DENIES with no handler — the pre-batch-C behaviour, not a regression", () => {
      const dc = new FakeChannel();
      const h = attachControlChannel(dc as unknown as RTCDataChannel, {});
      dc.pushMessage(guestRequest({ kind: "permission", id: "31" }));
      expect(JSON.parse(dc.sent[0]!).data).toEqual({ id: "31", granted: false });
      h.dispose();
    });

    it("DENIES when the handler throws", async () => {
      const dc = new FakeChannel();
      const h = attachControlChannel(dc as unknown as RTCDataChannel, {
        onPermission: () => { throw new Error("boom"); },
      });
      dc.pushMessage(guestRequest({ kind: "permission", id: "32" }));
      await Promise.resolve(); await Promise.resolve();
      expect(JSON.parse(dc.sent[0]!).data).toEqual({ id: "32", granted: false });
      h.dispose();
    });

    it("drops a non-string permission name rather than passing it through", async () => {
      const dc = new FakeChannel();
      const seen: PermissionRequest[] = [];
      const h = attachControlChannel(dc as unknown as RTCDataChannel, {
        onPermission: (r) => { seen.push(r); return false; },
      });
      dc.pushMessage(guestRequest({
        kind: "permission", id: "33", permissions: ["geolocation", 7, null],
      }));
      await Promise.resolve(); await Promise.resolve();
      expect(seen[0]!.permissions).toEqual(["geolocation"]);
      h.dispose();
    });
  });

  describe("cert_error", () => {
    it("proceeds only on an explicit true", async () => {
      const dc = new FakeChannel();
      const h = attachControlChannel(dc as unknown as RTCDataChannel, {
        onCertError: () => true,
      });
      dc.pushMessage(guestRequest({
        kind: "cert_error", id: "40", url: "https://bad.example",
        cert_error: -202, subject: "bad.example", issuer: "Nobody",
      }));
      await Promise.resolve(); await Promise.resolve();
      expect(JSON.parse(dc.sent[0]!).data).toEqual({ id: "40", proceed: true });
      h.dispose();
    });

    it("CANCELS with no handler — chromium's own default", () => {
      const dc = new FakeChannel();
      const h = attachControlChannel(dc as unknown as RTCDataChannel, {});
      dc.pushMessage(guestRequest({ kind: "cert_error", id: "41" }));
      expect(JSON.parse(dc.sent[0]!).data).toEqual({ id: "41", proceed: false });
      h.dispose();
    });
  });

  describe("login", () => {
    it("sends both credentials when the handler supplies them", async () => {
      const dc = new FakeChannel();
      const h = attachControlChannel(dc as unknown as RTCDataChannel, {
        onLogin: () => ({ username: "u", password: "p" }),
      });
      dc.pushMessage(guestRequest({
        kind: "login", id: "50", url: "https://intranet/",
        realm: "Staff", scheme: "basic",
      }));
      await Promise.resolve(); await Promise.resolve();
      expect(JSON.parse(dc.sent[0]!).data)
        .toEqual({ id: "50", username: "u", password: "p" });
      h.dispose();
    });

    it("cancels on null — the guest turns that back into the 401", async () => {
      const dc = new FakeChannel();
      const h = attachControlChannel(dc as unknown as RTCDataChannel, {
        onLogin: () => null,
      });
      dc.pushMessage(guestRequest({ kind: "login", id: "51" }));
      await Promise.resolve(); await Promise.resolve();
      expect(JSON.parse(dc.sent[0]!).data).toEqual({ id: "51" });
      h.dispose();
    });

    it("sends NEITHER credential when only one is supplied", async () => {
      // Half a credential reads as a bug rather than a decision, and the
      // guest treats a partial answer as cancelled anyway.
      const dc = new FakeChannel();
      const h = attachControlChannel(dc as unknown as RTCDataChannel, {
        onLogin: () => ({ username: "u" } as unknown as
                        { username: string; password: string }),
      });
      dc.pushMessage(guestRequest({ kind: "login", id: "52" }));
      await Promise.resolve(); await Promise.resolve();
      expect(JSON.parse(dc.sent[0]!).data).toEqual({ id: "52" });
      h.dispose();
    });

    it("reports a retry as first_attempt=false", async () => {
      const dc = new FakeChannel();
      const seen: LoginRequest[] = [];
      const h = attachControlChannel(dc as unknown as RTCDataChannel, {
        onLogin: (r) => { seen.push(r); return null; },
      });
      dc.pushMessage(guestRequest({
        kind: "login", id: "53", first_attempt: false,
      }));
      await Promise.resolve(); await Promise.resolve();
      // Without this the second prompt is indistinguishable from the first
      // and the user retypes the same rejected password.
      expect(seen[0]!.firstAttempt).toBe(false);
      h.dispose();
    });
  });

  describe("certErrorText", () => {
    // These were WRONG in a first draft — -200 and -202 were swapped, and
    // REVOKED/WEAK were at -207/-211 instead of -206/-208. A prompt naming
    // the wrong reason invites a decision on false grounds, so the numbers
    // are pinned here against net/base/net_error_list.h.
    it("names the errors at the codes chromium actually uses", () => {
      expect(certErrorText(-200)).toContain("name");
      expect(certErrorText(-201)).toContain("expired");
      expect(certErrorText(-202)).toContain("unknown authority");
      expect(certErrorText(-206)).toContain("revoked");
      expect(certErrorText(-208)).toContain("weakly signed");
    });
    it("falls back to the number for anything unrecognised", () => {
      expect(certErrorText(-999)).toBe("certificate error -999");
    });
  });

  // file_chooser: the guest is holding chromium's FileSelectListener and the
  // page cannot proceed until we answer. Every branch below must produce
  // exactly one response.
  describe("file_chooser", () => {
    it("declines when no handler is wired, rather than leaving the page stuck", () => {
      const dc = new FakeChannel();
      const h = attachControlChannel(dc as unknown as RTCDataChannel, {});

      dc.pushMessage(guestRequest({ kind: "file_chooser", id: "10" }));

      expect(dc.sent).toHaveLength(1);
      expect(JSON.parse(dc.sent[0]!).data).toEqual({ id: "10", accept: false });
      h.dispose();
    });

    it("passes accept/multiple/title to the handler and answers accept=true", async () => {
      const dc = new FakeChannel();
      const seen: FileChooserRequest[] = [];
      const h = attachControlChannel(dc as unknown as RTCDataChannel, {
        onFileChooser: (req) => { seen.push(req); return true; },
      });

      dc.pushMessage(guestRequest({
        kind: "file_chooser", id: "11",
        accept: [".pdf", "image/*"], multiple: true, title: "Pick a file",
      }));
      await Promise.resolve();
      await Promise.resolve();

      expect(seen).toEqual([
        { id: "11", accept: [".pdf", "image/*"], multiple: true, title: "Pick a file" },
      ]);
      expect(dc.sent).toHaveLength(1);
      expect(JSON.parse(dc.sent[0]!).data).toEqual({ id: "11", accept: true });
      h.dispose();
    });

    it("still answers when the handler THROWS", async () => {
      const dc = new FakeChannel();
      const h = attachControlChannel(dc as unknown as RTCDataChannel, {
        onFileChooser: () => { throw new Error("picker exploded"); },
      });

      dc.pushMessage(guestRequest({ kind: "file_chooser", id: "12" }));
      await Promise.resolve();
      await Promise.resolve();

      // The page must not be left blocked because our picker failed.
      expect(dc.sent).toHaveLength(1);
      expect(JSON.parse(dc.sent[0]!).data).toEqual({ id: "12", accept: false });
      h.dispose();
    });

    it("does not present a dialog — a chooser is not a js_dialog", () => {
      const dc = new FakeChannel();
      const cap = capturing();
      const h = attachControlChannel(dc as unknown as RTCDataChannel, {
        present: cap.present,
        onFileChooser: () => true,
      });

      dc.pushMessage(guestRequest({ kind: "file_chooser", id: "13" }));

      expect(cap.seen).toHaveLength(0);
      h.dispose();
    });

    it("drops a non-string entry in accept rather than passing it through", async () => {
      const dc = new FakeChannel();
      const seen: FileChooserRequest[] = [];
      const h = attachControlChannel(dc as unknown as RTCDataChannel, {
        onFileChooser: (req) => { seen.push(req); return false; },
      });

      // The guest is a remote peer; `accept` is whatever arrived on the wire.
      dc.pushMessage(guestRequest({
        kind: "file_chooser", id: "14", accept: [".pdf", 7, null],
      }));
      await Promise.resolve();
      await Promise.resolve();

      expect(seen[0]!.accept).toEqual([".pdf"]);
      h.dispose();
    });
  });

  it("answers a superseded prompt with the default rather than orphaning it", () => {
    const dc = new FakeChannel();
    const cap = capturing();
    const h = attachControlChannel(dc as unknown as RTCDataChannel, { present: cap.present });

    dc.pushMessage(guestRequest({ id: "1" }));
    dc.pushMessage(guestRequest({ id: "2" }));

    // The first is answered with a bare id (guest applies its default) and
    // its view is torn down; the second is now the open one.
    expect(dc.sent).toHaveLength(1);
    expect(JSON.parse(dc.sent[0]!).data).toEqual({ id: "1" });
    expect(cap.tornDown).toBe(1);
    expect(cap.seen).toHaveLength(2);
    h.dispose();
  });

  it("carries prompt_text only on an accepted prompt", () => {
    const dc = new FakeChannel();
    const cap = capturing();
    const h = attachControlChannel(dc as unknown as RTCDataChannel, { present: cap.present });

    dc.pushMessage(guestRequest({ dialog_type: "prompt", default_prompt: "hi" }));
    cap.answerWith({ id: "1", accept: true, prompt_text: "typed" });

    expect(JSON.parse(dc.sent[0]!).data).toEqual({ id: "1", accept: true, prompt_text: "typed" });
    h.dispose();
  });

  it("drops a response when the channel is not open", () => {
    const dc = new FakeChannel();
    const cap = capturing();
    const h = attachControlChannel(dc as unknown as RTCDataChannel, { present: cap.present });

    dc.pushMessage(guestRequest());
    dc.readyState = "closed";
    cap.answerWith({ id: "1", accept: true });

    expect(dc.sent).toHaveLength(0); // no throw, no send
    h.dispose();
  });

  it("tears down an open prompt when the channel closes", () => {
    const dc = new FakeChannel();
    const cap = capturing();
    const h = attachControlChannel(dc as unknown as RTCDataChannel, { present: cap.present });

    dc.pushMessage(guestRequest());
    expect(cap.tornDown).toBe(0);
    dc.fireClose();
    expect(cap.tornDown).toBe(1);
    h.dispose();
  });

  it("ignores malformed frames without throwing", () => {
    const dc = new FakeChannel();
    const cap = capturing();
    const h = attachControlChannel(dc as unknown as RTCDataChannel, { present: cap.present });

    expect(() => dc.pushMessage("{not json")).not.toThrow();
    expect(() => dc.pushMessage(JSON.stringify({ v: 1, type: "ui_request" }))).not.toThrow();
    expect(cap.seen).toHaveLength(0);
    expect(dc.sent).toHaveLength(0);
    h.dispose();
  });

  it("acknowledges ui_event notices without replying", () => {
    const dc = new FakeChannel();
    const cap = capturing();
    const h = attachControlChannel(dc as unknown as RTCDataChannel, { present: cap.present });

    dc.pushMessage(JSON.stringify({
      v: 1, type: "ui_event", t: 1, seq: 1, data: { kind: "fullscreen_changed" },
    }));

    expect(cap.seen).toHaveLength(0);
    expect(dc.sent).toHaveLength(0);
    h.dispose();
  });

  it("dispose() removes listeners and is idempotent", () => {
    const dc = new FakeChannel();
    const cap = capturing();
    const h = attachControlChannel(dc as unknown as RTCDataChannel, { present: cap.present });

    expect(dc.listenerCount("message")).toBe(1);
    h.dispose();
    h.dispose();
    expect(dc.listenerCount("message")).toBe(0);

    dc.pushMessage(guestRequest());
    expect(cap.seen).toHaveLength(0);
  });
});

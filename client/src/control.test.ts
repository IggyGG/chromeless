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
  // `cert_error` is the example because it is genuinely unimplemented. This
  // test used to use `file_chooser`, which now HAS a producer — if you are
  // here because a kind you just implemented broke this test, that is the
  // test doing its job: move it to another unimplemented kind.
  it("DECLINES an unsupported kind immediately instead of ignoring it", () => {
    const dc = new FakeChannel();
    const cap = capturing();
    const h = attachControlChannel(dc as unknown as RTCDataChannel, { present: cap.present });

    dc.pushMessage(guestRequest({ kind: "cert_error", id: "9" }));

    expect(cap.seen).toHaveLength(0);
    expect(dc.sent).toHaveLength(1);
    const env = JSON.parse(dc.sent[0]!);
    expect(env.data).toEqual({ id: "9" }); // no `accept` => guest's default
    h.dispose();
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

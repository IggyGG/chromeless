import { describe, it, expect, vi, afterEach } from "vitest";
import { normalizeUrl, navigate, goBack, currentUrl, currentPage } from "./navigate.js";

describe("normalizeUrl", () => {
  it("passes through a full http(s) URL", () => {
    expect(normalizeUrl("https://example.com/a?b=1")).toBe("https://example.com/a?b=1");
    expect(normalizeUrl("http://example.com")).toBe("http://example.com");
    expect(normalizeUrl("HTTPS://Example.com")).toBe("HTTPS://Example.com");
  });

  // A local dev server almost never has TLS. Defaulting to https here would
  // fail for everyone typing the most common local address there is.
  it("prefixes localhost with http, not https", () => {
    expect(normalizeUrl("localhost:3000")).toBe("http://localhost:3000");
    expect(normalizeUrl("localhost")).toBe("http://localhost");
    expect(normalizeUrl("127.0.0.1:8080/path")).toBe("http://127.0.0.1:8080/path");
  });

  it("treats a dotted, space-free token as a hostname", () => {
    expect(normalizeUrl("example.com")).toBe("https://example.com");
    expect(normalizeUrl("sub.example.co.uk/path")).toBe("https://sub.example.co.uk/path");
  });

  // Without this rung, typing a phrase produces "https://what is a webrtc
  // offer" — a DNS failure and an error page instead of a search.
  it("searches anything else", () => {
    expect(normalizeUrl("what is a webrtc offer")).toBe(
      "https://lite.duckduckgo.com/lite/?q=what%20is%20a%20webrtc%20offer",
    );
    expect(normalizeUrl("chromeless")).toContain("lite.duckduckgo.com");
  });

  // "example.com and more" has a dot but also spaces: it is a search, not a
  // hostname. The space check is what separates the two.
  it("prefers search over hostname when there are spaces", () => {
    expect(normalizeUrl("example.com and more")).toContain("lite.duckduckgo.com");
  });

  it("returns empty for blank input", () => {
    expect(normalizeUrl("")).toBe("");
    expect(normalizeUrl("   ")).toBe("");
  });

  it("trims surrounding whitespace", () => {
    expect(normalizeUrl("  example.com  ")).toBe("https://example.com");
  });
});

describe("navigation requests", () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it("posts the normalized URL and carries the session cookie", async () => {
    const calls: Array<{ path: string; init: RequestInit }> = [];
    vi.stubGlobal("fetch", ((path: string, init: RequestInit) => {
      calls.push({ path, init });
      return Promise.resolve(
        new Response(JSON.stringify({ url: "https://example.com" }), { status: 200 }),
      );
    }) as unknown as typeof fetch);

    const res = await navigate("example.com");

    expect(res.ok).toBe(true);
    expect(calls[0]!.path).toBe("/api/navigate");
    expect(JSON.parse(String(calls[0]!.init.body))).toEqual({ url: "https://example.com" });
    // Without this the gateway answers 401 and navigation silently stops
    // working while everything else still looks fine.
    expect(calls[0]!.init.credentials).toBe("same-origin");
  });

  it("surfaces the gateway's error message", async () => {
    vi.stubGlobal("fetch", vi.fn(async () =>
      new Response(JSON.stringify({ error: "only http:// and https:// urls are allowed" }), { status: 400 }),
    ));

    const res = await navigate("https://example.com");
    expect(res.ok).toBe(false);
    expect(res.error).toContain("only http");
  });

  it("reports a network failure rather than throwing", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => { throw new Error("offline"); }));

    const res = await navigate("example.com");
    expect(res.ok).toBe(false);
    expect(res.error).toBe("offline");
  });

  // Back with nothing behind it is a no-op, not an error — it must not raise
  // a banner just for pressing Back on the first page.
  it("treats a declined history command as ok:false without an error", async () => {
    vi.stubGlobal("fetch", vi.fn(async () =>
      new Response(JSON.stringify({ ok: false, reason: "no history" }), { status: 200 }),
    ));

    const res = await goBack();
    expect(res.ok).toBe(false);
    expect(res.error).toBeUndefined();
  });

  it("returns null from currentUrl when the endpoint fails", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => new Response("", { status: 502 })));
    expect(await currentUrl()).toBeNull();
  });

  it("returns the current url on success", async () => {
    vi.stubGlobal("fetch", vi.fn(async () =>
      new Response(JSON.stringify({ url: "https://example.com/" }), { status: 200 }),
    ));
    expect(await currentUrl()).toBe("https://example.com/");
  });

  // The gateway parsed the page title off DevTools /json and threw it away,
  // so the client could show an address and nothing else — a URL bar with no
  // page behind it. currentPage() carries both.
  describe("currentPage", () => {
    it("returns the url AND the page title", async () => {
      vi.stubGlobal("fetch", vi.fn(async () =>
        new Response(JSON.stringify({
          url: "https://example.com/", title: "Example Domain",
        }), { status: 200 }),
      ));
      expect(await currentPage()).toEqual({
        url: "https://example.com/", title: "Example Domain",
      });
    });

    it("tolerates an OLDER gateway that omits title", async () => {
      // Additive field: a client newer than its gateway must not break, and
      // this stack is rolled in two pieces (the bundle is baked into the
      // gateway image, the worker is separate), so version skew is normal.
      vi.stubGlobal("fetch", vi.fn(async () =>
        new Response(JSON.stringify({ url: "https://example.com/" }), { status: 200 }),
      ));
      expect(await currentPage()).toEqual({
        url: "https://example.com/", title: "",
      });
    });

    it("ignores a non-string title rather than rendering it", async () => {
      // The body comes over the network; a number here would end up in
      // document.title as "[object Object]" or similar.
      vi.stubGlobal("fetch", vi.fn(async () =>
        new Response(JSON.stringify({ url: "https://x/", title: 42 }), { status: 200 }),
      ));
      expect((await currentPage()).title).toBe("");
    });

    it("returns a null url and empty title when the endpoint fails", async () => {
      vi.stubGlobal("fetch", vi.fn(async () => new Response("", { status: 502 })));
      expect(await currentPage()).toEqual({ url: null, title: "" });
    });
  });
});

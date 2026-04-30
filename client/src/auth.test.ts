import { describe, it, expect } from "vitest";
import { fetchSessionToken, withToken } from "./auth.js";

describe("withToken", () => {
  it("appends ?token to a clean URL", () => {
    expect(withToken("ws://h/ws/abc", "TOK")).toBe("ws://h/ws/abc?token=TOK");
  });
  it("appends &token when query already present", () => {
    expect(withToken("ws://h/ws/abc?other=1", "T O K")).toBe("ws://h/ws/abc?other=1&token=T%20O%20K");
  });
});

describe("fetchSessionToken", () => {
  const base = "ws://localhost:8080/ws";

  it("returns null on fetch reject", async () => {
    const tok = await fetchSessionToken("dev", "client", {
      signalingBase: base,
      fetchImpl: () => Promise.reject(new Error("offline")),
    });
    expect(tok).toBeNull();
  });

  it("returns null on non-2xx", async () => {
    const tok = await fetchSessionToken("dev", "client", {
      signalingBase: base,
      fetchImpl: () => Promise.resolve(new Response("nope", { status: 401 })) as Promise<Response>,
    });
    expect(tok).toBeNull();
  });

  it("returns null on shape mismatch", async () => {
    const tok = await fetchSessionToken("dev", "client", {
      signalingBase: base,
      fetchImpl: () => Promise.resolve(new Response(JSON.stringify({ junk: 1 }), { status: 200, headers: { "Content-Type": "application/json" } })) as Promise<Response>,
    });
    expect(tok).toBeNull();
  });

  it("returns the parsed body on happy path", async () => {
    const body = { token: "x.y.z", exp: 12345, role: "client", sid: "dev", sub: "t" };
    const tok = await fetchSessionToken("dev", "client", {
      signalingBase: base,
      fetchImpl: () => Promise.resolve(new Response(JSON.stringify(body), { status: 200, headers: { "Content-Type": "application/json" } })) as Promise<Response>,
    });
    expect(tok).toEqual(body);
  });

  it("translates ws:// to http:// when issuing the request", async () => {
    let captured: string | URL = "";
    const fetchImpl: typeof fetch = (url) => {
      captured = url as string;
      return Promise.resolve(new Response(JSON.stringify({ token: "t", exp: 0, role: "client", sid: "s", sub: "u" }), { status: 200 })) as Promise<Response>;
    };
    await fetchSessionToken("s", "client", { signalingBase: base, fetchImpl });
    expect(String(captured)).toMatch(/^http:\/\/localhost:8080\/issue-token\?/);
    expect(String(captured)).toContain("role=client");
    expect(String(captured)).toContain("session_id=s");
  });
});

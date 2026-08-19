import { describe, it, expect } from "vitest";
import { fetchTurnConfig, resolveCredentialsURL } from "./turn.js";

describe("resolveCredentialsURL", () => {
  it("defaults to a same-origin path", () => {
    expect(resolveCredentialsURL()).toBe("/turn-credentials");
    expect(resolveCredentialsURL("")).toBe("/turn-credentials");
  });
  it("rewrites ws:// → http://", () => {
    expect(resolveCredentialsURL("ws://localhost:8080/ws")).toBe("http://localhost:8080/turn-credentials");
  });
  it("rewrites wss:// → https://", () => {
    expect(resolveCredentialsURL("wss://signaling.example.com/ws")).toBe("https://signaling.example.com/turn-credentials");
  });
  it("keeps http(s) base untouched", () => {
    expect(resolveCredentialsURL("https://signaling.example.com")).toBe("https://signaling.example.com/turn-credentials");
  });
});

describe("fetchTurnConfig", () => {
  it("returns fallback when fetch rejects", async () => {
    const cfg = await fetchTurnConfig("ws://localhost:8080", () => Promise.reject(new Error("offline")));
    expect(cfg.iceServers[0]!.urls).toEqual(expect.arrayContaining(["stun:stun.cloudflare.com:3478"]));
  });

  it("returns fallback on non-2xx", async () => {
    const cfg = await fetchTurnConfig(
      "ws://localhost:8080",
      () => Promise.resolve(new Response("oops", { status: 500 })) as Promise<Response>,
    );
    expect(cfg.iceServers).toHaveLength(1);
  });

  it("returns fallback on malformed JSON shape", async () => {
    const cfg = await fetchTurnConfig(
      "ws://localhost:8080",
      () => Promise.resolve(new Response(JSON.stringify({ wrong: true }), { status: 200, headers: { "Content-Type": "application/json" } })) as Promise<Response>,
    );
    expect(cfg.iceServers).toHaveLength(1);
  });

  it("passes through a valid response", async () => {
    const payload = {
      iceServers: [
        { urls: ["stun:s.example.com:3478"] },
        { urls: "turn:t.example.com:3478", username: "u", credential: "p" },
      ],
    };
    const cfg = await fetchTurnConfig(
      "ws://localhost:8080",
      () => Promise.resolve(new Response(JSON.stringify(payload), { status: 200, headers: { "Content-Type": "application/json" } })) as Promise<Response>,
    );
    expect(cfg).toEqual(payload);
  });

  // The standalone gateway gates /turn-credentials on the session cookie.
  // Withholding it yields a 401, which falls through to the STUN-only
  // FALLBACK — that "works" on a LAN and then fails to traverse any real NAT,
  // so the failure is a silent downgrade rather than an error.
  it("sends same-origin credentials so the gateway's session cookie is attached", async () => {
    let init: RequestInit | undefined;
    const fetchImpl: typeof fetch = (_url, i) => {
      init = i;
      return Promise.resolve(new Response(JSON.stringify({ iceServers: [] }), { status: 200, headers: { "Content-Type": "application/json" } })) as Promise<Response>;
    };
    await fetchTurnConfig("ws://localhost:8080", fetchImpl);
    expect(init?.credentials).toBe("same-origin");
  });
});

import { describe, it, expect } from "vitest";
import {
  resolveSignalingUrl,
  FALLBACK_SIGNALING_URL,
  type ConfigCarrier,
} from "./config.js";

const noQuery = { search: "" };
const noConfig: ConfigCarrier = {};

describe("resolveSignalingUrl", () => {
  it("falls back to the built-in default when nothing is configured", () => {
    expect(resolveSignalingUrl(noQuery, noConfig)).toBe(FALLBACK_SIGNALING_URL);
  });

  it("reads window.__CHROMELESS_CONFIG__ (what compose injects)", () => {
    expect(
      resolveSignalingUrl(noQuery, {
        __CHROMELESS_CONFIG__: { signalingUrl: "ws://signaling:8080/ws" },
      }),
    ).toBe("ws://signaling:8080/ws");
  });

  it("accepts the legacy __CBWRTC_CONFIG__ spelling", () => {
    expect(
      resolveSignalingUrl(noQuery, {
        __CBWRTC_CONFIG__: { signalingUrl: "ws://legacy:8080/ws" },
      }),
    ).toBe("ws://legacy:8080/ws");
  });

  it("prefers the current name over the legacy one", () => {
    expect(
      resolveSignalingUrl(noQuery, {
        __CHROMELESS_CONFIG__: { signalingUrl: "ws://new/ws" },
        __CBWRTC_CONFIG__: { signalingUrl: "ws://old/ws" },
      }),
    ).toBe("ws://new/ws");
  });

  it("lets ?signaling= override injected config", () => {
    expect(
      resolveSignalingUrl(
        { search: "?signaling=wss://override.example/ws" },
        { __CHROMELESS_CONFIG__: { signalingUrl: "ws://injected:8080/ws" } },
      ),
    ).toBe("wss://override.example/ws");
  });

  it("url-decodes the query param", () => {
    expect(
      resolveSignalingUrl({ search: "?signaling=ws%3A%2F%2Fh%3A9%2Fws" }, noConfig),
    ).toBe("ws://h:9/ws");
  });

  it("ignores other query params", () => {
    expect(resolveSignalingUrl({ search: "?session=abc&foo=1" }, noConfig)).toBe(
      FALLBACK_SIGNALING_URL,
    );
  });

  // An unset SIGNALING_URL can template through as an empty string. Treating
  // that as an override would produce a broken dial with no diagnostic; it
  // must fall through to the next source instead.
  it("treats a blank query param as absent", () => {
    expect(resolveSignalingUrl({ search: "?signaling=" }, noConfig)).toBe(
      FALLBACK_SIGNALING_URL,
    );
  });

  it("treats a whitespace-only query param as absent", () => {
    expect(resolveSignalingUrl({ search: "?signaling=%20%20" }, noConfig)).toBe(
      FALLBACK_SIGNALING_URL,
    );
  });

  it("treats a blank injected value as absent", () => {
    expect(
      resolveSignalingUrl(noQuery, { __CHROMELESS_CONFIG__: { signalingUrl: "   " } }),
    ).toBe(FALLBACK_SIGNALING_URL);
  });

  it("falls through an empty config object", () => {
    expect(resolveSignalingUrl(noQuery, { __CHROMELESS_CONFIG__: {} })).toBe(
      FALLBACK_SIGNALING_URL,
    );
  });

  it("trims surrounding whitespace off a real value", () => {
    expect(
      resolveSignalingUrl(noQuery, {
        __CHROMELESS_CONFIG__: { signalingUrl: "  ws://h:8080/ws  " },
      }),
    ).toBe("ws://h:8080/ws");
  });
});

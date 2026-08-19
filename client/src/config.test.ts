import { describe, it, expect } from "vitest";
import {
  resolveSignalingUrl,
  deriveSignalingUrlFromOrigin,
  FALLBACK_SIGNALING_URL,
  type ConfigCarrier,
} from "./config.js";

// No protocol/host ⇒ origin derivation yields null ⇒ FALLBACK_SIGNALING_URL.
// Kept as-is so the pre-existing precedence tests below still exercise the
// terminal rung.
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

  // --- origin derivation (the standalone single-port gateway) ---

  it("derives wss:// from an https page", () => {
    expect(
      resolveSignalingUrl(
        { search: "", protocol: "https:", host: "localhost:8443" },
        noConfig,
      ),
    ).toBe("wss://localhost:8443/ws");
  });

  it("derives ws:// from an http page", () => {
    expect(
      resolveSignalingUrl(
        { search: "", protocol: "http:", host: "localhost:3000" },
        noConfig,
      ),
    ).toBe("ws://localhost:3000/ws");
  });

  // The whole point of deriving: the operator picks the port, and no
  // compiled-in default can be right.
  it("honours a non-default port and a remote host", () => {
    expect(
      resolveSignalingUrl(
        { search: "", protocol: "https:", host: "browser.example.com:9999" },
        noConfig,
      ),
    ).toBe("wss://browser.example.com:9999/ws");
  });

  it("still lets injected config win over the origin", () => {
    expect(
      resolveSignalingUrl(
        { search: "", protocol: "https:", host: "localhost:8443" },
        { __CHROMELESS_CONFIG__: { signalingUrl: "ws://elsewhere:8080/ws" } },
      ),
    ).toBe("ws://elsewhere:8080/ws");
  });

  it("still lets ?signaling= win over the origin", () => {
    expect(
      resolveSignalingUrl(
        {
          search: "?signaling=ws://override:1/ws",
          protocol: "https:",
          host: "localhost:8443",
        },
        noConfig,
      ),
    ).toBe("ws://override:1/ws");
  });
});

describe("deriveSignalingUrlFromOrigin", () => {
  it("maps https → wss and http → ws", () => {
    expect(
      deriveSignalingUrlFromOrigin({ search: "", protocol: "https:", host: "h:1" }),
    ).toBe("wss://h:1/ws");
    expect(
      deriveSignalingUrlFromOrigin({ search: "", protocol: "http:", host: "h:1" }),
    ).toBe("ws://h:1/ws");
  });

  // A file:// page has an empty host. Returning a URL there would produce a
  // dial at "wss:///ws"; the caller must fall back instead.
  it("returns null when there is no host", () => {
    expect(
      deriveSignalingUrlFromOrigin({ search: "", protocol: "file:", host: "" }),
    ).toBeNull();
    expect(
      deriveSignalingUrlFromOrigin({ search: "", protocol: "https:", host: "  " }),
    ).toBeNull();
  });

  it("returns null for a non-http(s) scheme", () => {
    expect(
      deriveSignalingUrlFromOrigin({ search: "", protocol: "file:", host: "x" }),
    ).toBeNull();
  });

  it("returns null when protocol/host are absent entirely", () => {
    expect(deriveSignalingUrlFromOrigin({ search: "" })).toBeNull();
  });
});

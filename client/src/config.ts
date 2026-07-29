// Runtime configuration resolution for the browser client.
//
// OSS-W1. Before this module the signaling endpoint was a bare
// `const DEFAULT_SIGNALING = "ws://localhost:8080/ws"` in main.ts.
// infra/compose.yaml has always generated a config.js carrying
// SIGNALING_URL into the page, but index.html never loaded it and nothing
// ever read the global — so the documented "override SIGNALING_URL at
// compose-up time" knob silently did nothing and the client always dialled
// localhost. Anyone running the stack on a non-default host had no way to
// point the client at it short of editing the source and rebuilding.
//
// Kept as a standalone pure module (rather than living in main.ts) for two
// reasons: main.ts resolves DOM nodes at import time, so it cannot be
// imported by a unit test; and this is library surface, not demo surface.

/** Shape of the config object a host page may inject. */
export interface ChromelessRuntimeConfig {
  /** Signaling base URL, e.g. "ws://localhost:8080/ws". No session suffix. */
  signalingUrl?: string;
}

/** The window fields this module reads. Both names are optional. */
export interface ConfigCarrier {
  __CHROMELESS_CONFIG__?: ChromelessRuntimeConfig | undefined;
  /** @deprecated legacy name, still emitted by older compose files. */
  __CBWRTC_CONFIG__?: ChromelessRuntimeConfig | undefined;
}

/** Compile-time fallback — the local dev stack's default. */
export const FALLBACK_SIGNALING_URL = "ws://localhost:8080/ws";

/**
 * Resolve the signaling base URL. Most-specific source wins:
 *
 *   1. `?signaling=` query param — per-load override. Lets a hosted demo
 *      page target a local broker without a rebuild.
 *   2. `window.__CHROMELESS_CONFIG__.signalingUrl` — injected by config.js
 *      (see infra/compose.yaml). `__CBWRTC_CONFIG__` is accepted as the
 *      legacy spelling so an older generated config.js keeps working.
 *   3. {@link FALLBACK_SIGNALING_URL}.
 *
 * Blank/whitespace-only values at any level are treated as absent rather
 * than as an override, so an unset `SIGNALING_URL` that templates through
 * as an empty string falls through instead of producing a broken dial.
 *
 * Both parameters are injectable for testing.
 */
export function resolveSignalingUrl(
  location: { search: string } = window.location,
  carrier: ConfigCarrier = window as unknown as ConfigCarrier,
): string {
  const fromQuery = new URLSearchParams(location.search).get("signaling");
  if (fromQuery && fromQuery.trim()) {
    return fromQuery.trim();
  }

  const injected =
    carrier.__CHROMELESS_CONFIG__?.signalingUrl ??
    carrier.__CBWRTC_CONFIG__?.signalingUrl;
  if (injected && injected.trim()) {
    return injected.trim();
  }

  return FALLBACK_SIGNALING_URL;
}

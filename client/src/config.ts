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
//
// Later: the terminal fallback became origin-derivation rather than a fixed
// `ws://localhost:8080/ws`. The standalone stack serves the page and proxies
// signaling from one origin, where a hardcoded scheme is actively wrong — a
// https:// page dialling ws:// is blocked as mixed content, so the fixed
// default made the TLS deployment unconnectable rather than merely
// misconfigured. See deriveSignalingUrlFromOrigin.

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

/**
 * Last-resort fallback, used only when the page origin cannot be read (a
 * non-browser host, or `file://` — see {@link deriveSignalingUrlFromOrigin}).
 * In a real page {@link resolveSignalingUrl} derives from the origin instead.
 */
export const FALLBACK_SIGNALING_URL = "ws://localhost:8080/ws";

/** The subset of `window.location` this module reads. */
export interface LocationLike {
  search: string;
  protocol?: string | undefined;
  host?: string | undefined;
}

/**
 * Build a signaling base from the page's own origin: `https://h/` → `wss://h/ws`.
 *
 * This is the normal case for the standalone stack, where one gateway serves
 * the page AND proxies the broker on a single port. Deriving rather than
 * hardcoding matters for two reasons:
 *
 *   - The scheme must track the page's. A `https://` page dialling `ws://` is
 *     blocked outright as mixed content, so a fixed `ws://` default would make
 *     the TLS deployment silently unconnectable.
 *   - The port is the operator's choice (`CHROMELESS_PORT`), so no compiled-in
 *     value can be right.
 *
 * Returns null when there is no usable origin — `file://` has no host, and
 * `location.protocol` is neither http nor https. The caller then falls back.
 */
export function deriveSignalingUrlFromOrigin(
  location: LocationLike,
): string | null {
  const proto = location.protocol ?? "";
  const host = location.host ?? "";
  if (!host.trim()) return null;
  if (proto === "https:") return `wss://${host}/ws`;
  if (proto === "http:") return `ws://${host}/ws`;
  return null;
}

/**
 * Resolve the signaling base URL. Most-specific source wins:
 *
 *   1. `?signaling=` query param — per-load override. Lets a hosted demo
 *      page target a local broker without a rebuild.
 *   2. `window.__CHROMELESS_CONFIG__.signalingUrl` — injected by a host page
 *      or a generated config.js. `__CBWRTC_CONFIG__` is accepted as the legacy
 *      spelling so an older generated config.js keeps working.
 *   3. The page's own origin (see {@link deriveSignalingUrlFromOrigin}).
 *   4. {@link FALLBACK_SIGNALING_URL}, only when the origin is unusable.
 *
 * Rung 3 is the one that makes the same-origin gateway work with no
 * configuration at all. It also removes a long-standing trap: the fallback
 * used to be an unconditional `ws://localhost:8080/ws`, so serving the page
 * from any other host or port produced a dial at localhost with no diagnostic.
 *
 * Blank/whitespace-only values at any level are treated as absent rather
 * than as an override, so an unset `SIGNALING_URL` that templates through
 * as an empty string falls through instead of producing a broken dial.
 *
 * Both parameters are injectable for testing.
 */
export function resolveSignalingUrl(
  location: LocationLike = window.location,
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

  return deriveSignalingUrlFromOrigin(location) ?? FALLBACK_SIGNALING_URL;
}

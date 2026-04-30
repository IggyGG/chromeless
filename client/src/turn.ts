// Fetches the ICE / TURN config from the signaling server before
// constructing an RTCPeerConnection. See
// docs/ice-and-turn.md for the design rationale.
//
// The signaling server's /turn-credentials endpoint (T25) returns:
//   { "iceServers": [
//       { "urls": ["stun:..."] },
//       { "urls": ["turn:..."], "username": "...", "credential": "..." }
//   ]}
//
// We accept that JSON verbatim and pass it into RTCPeerConnection.
// On any network/parse failure we fall back to a STUN-only default so
// the client still has a chance of working on permissive networks
// (LAN, dev box) — but we emit a console warning so the operator
// notices.

export interface IceServerConfig {
  urls: string | string[];
  username?: string;
  credential?: string;
}

export interface TurnConfig {
  iceServers: IceServerConfig[];
}

const FALLBACK: TurnConfig = {
  iceServers: [
    { urls: ["stun:stun.cloudflare.com:3478", "stun:stun.l.google.com:19302"] },
  ],
};

/**
 * Fetch the ICE config from `signalingBase`/turn-credentials.
 *
 * `signalingBase` may be:
 *   - empty / undefined → same origin (`/turn-credentials`)
 *   - a `ws://` or `wss://` URL → translated to http(s)://host
 *   - a `http(s)://` URL → used as-is
 *
 * Returns the fallback STUN-only config on any error.
 */
export async function fetchTurnConfig(signalingBase?: string, fetchImpl: typeof fetch = fetch): Promise<TurnConfig> {
  const url = resolveCredentialsURL(signalingBase);
  try {
    const res = await fetchImpl(url, { credentials: "omit", cache: "no-store" });
    if (!res.ok) {
      console.warn(`turn-credentials: HTTP ${res.status}; using fallback`);
      return FALLBACK;
    }
    const body = (await res.json()) as unknown;
    if (!isTurnConfig(body)) {
      console.warn("turn-credentials: malformed payload; using fallback", body);
      return FALLBACK;
    }
    return body;
  } catch (err) {
    console.warn("turn-credentials: fetch failed; using fallback", err);
    return FALLBACK;
  }
}

export function resolveCredentialsURL(signalingBase?: string): string {
  if (!signalingBase) return "/turn-credentials";
  let base = signalingBase.replace(/^ws(s?):/i, "http$1:");
  // Strip trailing path segments like "/ws" so we hit the same host root.
  try {
    const u = new URL(base);
    return new URL("/turn-credentials", u).toString();
  } catch {
    base = base.replace(/\/+$/, "");
    return `${base}/turn-credentials`;
  }
}

function isTurnConfig(v: unknown): v is TurnConfig {
  if (typeof v !== "object" || v === null) return false;
  const ice = (v as { iceServers?: unknown }).iceServers;
  if (!Array.isArray(ice)) return false;
  return ice.every((entry) => {
    if (typeof entry !== "object" || entry === null) return false;
    const urls = (entry as { urls?: unknown }).urls;
    return typeof urls === "string" || (Array.isArray(urls) && urls.every(u => typeof u === "string"));
  });
}

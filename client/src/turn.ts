// Fetches the ICE / TURN config before constructing an RTCPeerConnection.
// See docs/ice-and-turn.md for the design rationale.
//
// Two endpoints, in priority order:
//
//   1. T76 turn-issuer (`POST /issue-turn-cred`, Authorization: Bearer <token>).
//      The Phase-3 path: returns RFC 7635 HMAC-REST credentials scoped to
//      the calling tenant + session. Used when `authToken` is provided.
//
//   2. T25 signaling /turn-credentials (`GET`).
//      The Phase-1 path: anonymous, returns ICE config or empty STUN list.
//      Used when no authToken is supplied (dev / auth-disabled deploys).
//
// On any network/parse failure of *either* path we fall back to a
// STUN-only default so the client still has a chance of working on
// permissive networks (LAN, dev box) — but we emit a console warning so
// the operator notices.

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
 * Options that route between the T25 anonymous endpoint and the T76
 * tenant-scoped issuer.
 *
 *   - `signalingBase`: same as before; resolves the T25 URL.
 *   - `issuerBase`: optional; T76 issuer base URL. If unset and an
 *      `authToken` is provided, we derive `<signalingBase>/issue-turn-cred`
 *      as a sane same-host default.
 *   - `authToken`: T48 session token. Presence routes to the issuer.
 *   - `sessionId` / `ttlSeconds`: forwarded to the issuer body. Both
 *      optional — the issuer falls back to the token's `sid` claim and
 *      a 1-hour TTL respectively.
 */
export interface FetchTurnOptions {
  signalingBase?: string;
  issuerBase?: string;
  authToken?: string;
  sessionId?: string;
  ttlSeconds?: number;
  fetchImpl?: typeof fetch;
}

/**
 * Fetch the ICE config. Picks the issuer endpoint when `authToken` is
 * provided, otherwise falls back to the signaling-server endpoint.
 *
 * Returns the STUN-only fallback config on any error from either path.
 *
 * Backwards-compatible signature: `fetchTurnConfig("ws://signaling/")`
 * still works exactly as before.
 */
export async function fetchTurnConfig(
  optsOrBase?: FetchTurnOptions | string,
  legacyFetchImpl?: typeof fetch,
): Promise<TurnConfig> {
  let opts: FetchTurnOptions;
  if (typeof optsOrBase === "string" || optsOrBase === undefined) {
    // exactOptionalPropertyTypes: only set fields when defined.
    opts = {};
    if (optsOrBase !== undefined) opts.signalingBase = optsOrBase;
    if (legacyFetchImpl !== undefined) opts.fetchImpl = legacyFetchImpl;
  } else {
    opts = optsOrBase;
  }
  const fetchImpl = opts.fetchImpl ?? fetch;

  if (opts.authToken) {
    const cfg = await fetchFromIssuer(opts, fetchImpl);
    if (cfg) return cfg;
    // Issuer failed; fall through to the anonymous endpoint so we still
    // pick up STUN URLs operators wired into signaling. If THAT also
    // fails we land on FALLBACK below.
  }

  const url = resolveCredentialsURL(opts.signalingBase);
  try {
    // "same-origin" so the standalone gateway's session cookie is sent; it
    // gates this endpoint. With "omit" the 401 falls through to the STUN-only
    // FALLBACK below, which "works" on a LAN and then fails to traverse any
    // real NAT — a silent downgrade rather than an error.
    const res = await fetchImpl(url, { credentials: "same-origin", cache: "no-store" });
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

/**
 * Talk to the T76 issuer. Returns null on any failure so the caller can
 * fall through to the signaling-server endpoint.
 */
async function fetchFromIssuer(
  opts: FetchTurnOptions,
  fetchImpl: typeof fetch,
): Promise<TurnConfig | null> {
  const url = resolveIssuerURL(opts.issuerBase ?? opts.signalingBase);
  const body: Record<string, unknown> = {};
  if (opts.sessionId) body.sessionId = opts.sessionId;
  if (typeof opts.ttlSeconds === "number") body.ttlSeconds = opts.ttlSeconds;

  try {
    const res = await fetchImpl(url, {
      method: "POST",
      // The issuer authenticates with the Bearer token below, so the cookie is
      // not required here — but a same-origin deployment may still put this
      // endpoint behind the gateway, and withholding the cookie there costs a
      // silent fall-through to the anonymous endpoint.
      credentials: "same-origin",
      cache: "no-store",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${opts.authToken}`,
      },
      body: JSON.stringify(body),
    });
    if (!res.ok) {
      console.warn(`turn-issuer: HTTP ${res.status}; falling back to signaling endpoint`);
      return null;
    }
    const payload = (await res.json()) as unknown;
    // Issuer responds with an `iceServers` field shaped like our
    // existing TurnConfig — use it directly.
    if (isTurnConfig(payload)) return payload;
    console.warn("turn-issuer: malformed payload; falling back", payload);
    return null;
  } catch (err) {
    console.warn("turn-issuer: fetch failed; falling back", err);
    return null;
  }
}

export function resolveIssuerURL(base?: string): string {
  if (!base) return "/issue-turn-cred";
  let s = base.replace(/^ws(s?):/i, "http$1:");
  try {
    const u = new URL(s);
    return new URL("/issue-turn-cred", u).toString();
  } catch {
    s = s.replace(/\/+$/, "");
    return `${s}/issue-turn-cred`;
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

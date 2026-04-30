// Session-token fetcher for T48 signaling auth.
//
// In production, the browser obtains its token from the application's
// own auth-aware backend, which signs claims using the Ed25519 private
// key shared with the signaling server (only the public key sits on
// signaling). This module just pulls a token from a known endpoint and
// hands it back as a string.
//
// In dev, the signaling container runs with `CBWRTC_DEV_ISSUER=1`,
// which exposes `/issue-token` on the same origin. Defaults here
// match that.
//
// This module does NOT cache the token across page lifetimes. The
// token's `exp` is checked at the call site (typically once per
// connect()), and re-fetched on reconnect (T37) so that long-lived
// pages can naturally roll over.

export interface IssuedToken {
  token: string;
  exp: number;
  role: "client" | "browser";
  sid: string;
  sub: string;
}

export interface FetchTokenOptions {
  /** Same shape as turn.ts: ws://host/ws or http://host. */
  signalingBase?: string;
  /** Override path; defaults to /issue-token. */
  path?: string;
  /** Tenant id; defaults to "dev". */
  tenant?: string;
  /** Override fetch (for tests). */
  fetchImpl?: typeof fetch;
}

export async function fetchSessionToken(
  sessionId: string,
  role: "client" | "browser",
  opts: FetchTokenOptions = {},
): Promise<IssuedToken | null> {
  const base = opts.signalingBase ?? "";
  const path = opts.path ?? "/issue-token";
  const tenant = opts.tenant ?? "dev";
  const url = resolveURL(base, path, { role, session_id: sessionId, tenant });
  const f = opts.fetchImpl ?? fetch;
  try {
    const res = await f(url, { credentials: "omit", cache: "no-store" });
    if (!res.ok) {
      console.warn(`session-token: HTTP ${res.status}`);
      return null;
    }
    const body = (await res.json()) as unknown;
    if (!isIssuedToken(body)) {
      console.warn("session-token: malformed response", body);
      return null;
    }
    return body;
  } catch (err) {
    console.warn("session-token: fetch failed", err);
    return null;
  }
}

/**
 * Append the token to a websocket URL via `?token=...`. Preserves
 * any pre-existing query parameters.
 */
export function withToken(wsUrl: string, token: string): string {
  const sep = wsUrl.includes("?") ? "&" : "?";
  return `${wsUrl}${sep}token=${encodeURIComponent(token)}`;
}

// ---------- internals ----------

function resolveURL(base: string, path: string, qs: Record<string, string>): string {
  const queryString = Object.entries(qs)
    .map(([k, v]) => `${encodeURIComponent(k)}=${encodeURIComponent(v)}`)
    .join("&");
  if (!base) return `${path}?${queryString}`;
  let httpBase = base.replace(/^ws(s?):/i, "http$1:");
  try {
    const u = new URL(httpBase);
    return new URL(path, u).toString() + "?" + queryString;
  } catch {
    httpBase = httpBase.replace(/\/+$/, "");
    return `${httpBase}${path}?${queryString}`;
  }
}

function isIssuedToken(v: unknown): v is IssuedToken {
  if (typeof v !== "object" || v === null) return false;
  const o = v as Record<string, unknown>;
  return typeof o["token"] === "string"
    && typeof o["exp"] === "number"
    && (o["role"] === "client" || o["role"] === "browser")
    && typeof o["sid"] === "string"
    && typeof o["sub"] === "string";
}

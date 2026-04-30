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

/**
 * T89: short-TTL refresh manager.
 *
 * Pairs with the signaling server's token revocation story: tokens
 * are issued with short `exp` (5–15 minutes) and refreshed proactively
 * before they expire. That way revocation by simply ceasing-to-issue
 * (the common case — off-boarding, tenant disable) takes effect within
 * a refresh interval without any explicit denylist hit, and the
 * denylist itself only needs to handle in-flight tokens.
 *
 * Usage:
 *
 *   const refresher = new TokenRefresher(sessionId, "client", { signalingBase });
 *   await refresher.start();              // initial fetch
 *   const tok = refresher.current();      // current token; auto-rolled
 *
 * The refresher does NOT mutate the websocket URL on its own — the
 * caller decides what to do when a refresh produces a new token. In
 * v1 the answerer-role client only attaches the token at connect
 * time, so a fresh token only matters on the next reconnect (T37).
 * Phase-3 push-notify-based revocation can wire `onRefresh` to call
 * `pc.setConfiguration` or trigger an explicit reconnect.
 */
export class TokenRefresher {
  private readonly sessionId: string;
  private readonly role: "client" | "browser";
  private readonly opts: FetchTokenOptions;
  private readonly leadMs: number;
  private readonly setTimeoutFn: (cb: () => void, ms: number) => number;
  private readonly clearTimeoutFn: (id: number) => void;
  private readonly nowFn: () => number;
  private token: IssuedToken | null = null;
  private timer: number | null = null;
  private stopped = false;
  private listeners = new Set<(t: IssuedToken) => void>();

  constructor(
    sessionId: string,
    role: "client" | "browser",
    opts: FetchTokenOptions & {
      /**
       * Refresh this many ms before `exp`. Defaults to 60_000 (1 min).
       * Must be smaller than the issuer's TTL or refresh fires
       * after expiry.
       */
      refreshLeadMs?: number;
      /** Override scheduler (tests). */
      setTimeout?: (cb: () => void, ms: number) => number;
      clearTimeout?: (id: number) => void;
      now?: () => number;
    } = {},
  ) {
    this.sessionId = sessionId;
    this.role = role;
    this.opts = opts;
    this.leadMs = opts.refreshLeadMs ?? 60_000;
    this.setTimeoutFn = opts.setTimeout ?? ((cb, ms) => globalThis.setTimeout(cb, ms) as unknown as number);
    this.clearTimeoutFn = opts.clearTimeout ?? ((id) => globalThis.clearTimeout(id));
    this.nowFn = opts.now ?? Date.now;
  }

  /** Subscribe to refresh events. Returns a detach function. */
  onRefresh(listener: (t: IssuedToken) => void): () => void {
    this.listeners.add(listener);
    return () => { this.listeners.delete(listener); };
  }

  /** Returns the current cached token, or null before start(). */
  current(): IssuedToken | null { return this.token; }

  /**
   * Initial fetch. Returns the issued token (or null on failure).
   * Schedules the next refresh.
   */
  async start(): Promise<IssuedToken | null> {
    this.stopped = false;
    const tok = await fetchSessionToken(this.sessionId, this.role, this.opts);
    this.token = tok;
    if (tok) this.schedule(tok);
    return tok;
  }

  /** Cancel the next refresh and stop the loop. */
  stop(): void {
    this.stopped = true;
    if (this.timer !== null) {
      this.clearTimeoutFn(this.timer);
      this.timer = null;
    }
  }

  private schedule(tok: IssuedToken): void {
    if (this.stopped) return;
    if (this.timer !== null) this.clearTimeoutFn(this.timer);
    // exp is unix-seconds; nowFn is wall-clock ms.
    const expMs = tok.exp * 1000;
    const delay = Math.max(1_000, expMs - this.leadMs - this.nowFn());
    this.timer = this.setTimeoutFn(() => { void this.refresh(); }, delay);
  }

  private async refresh(): Promise<void> {
    if (this.stopped) return;
    this.timer = null;
    const tok = await fetchSessionToken(this.sessionId, this.role, this.opts);
    if (!tok) {
      // Refresh failed; retry in 30s. Caller will see the stale token
      // until it actually expires; signaling will reject when it does.
      this.timer = this.setTimeoutFn(() => { void this.refresh(); }, 30_000);
      return;
    }
    this.token = tok;
    for (const l of this.listeners) l(tok);
    this.schedule(tok);
  }
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

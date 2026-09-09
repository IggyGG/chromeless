// Address-bar helpers: turn what a person typed into a URL, and drive the
// gateway's navigation endpoints.
//
// Navigation does NOT ride the peer connection. The WebRTC session carries
// pixels and input; the URL goes over HTTP to the gateway, which issues CDP
// Page.navigate to the worker. That is also how the triform portal works —
// there the control plane is physics rather than a gateway — so the two agree
// on shape, and the endpoints are named after physics' `/ops/*` verbs.
//
// It has to be this way for now: the embedder hardcodes about:blank and reads
// only the --remote-debugging-* switches, and no data channel carries a
// navigation verb. Adding one would be a C++ change, which cannot be compiled
// or verified in this repo.

/** Endpoints on the gateway. Same-origin, so no base URL is needed. */
const ENDPOINTS = {
  navigate: "/api/navigate",
  back: "/api/back",
  forward: "/api/forward",
  reload: "/api/reload",
  stop: "/api/stop",
  currentUrl: "/api/current-url",
} as const;

/**
 * Turn address-bar input into a URL, the way a browser does.
 *
 * Ported from the triform portal's `normalize_url`
 * (portal/src/canvas/browser_context.rs), whose rung order is what makes an
 * address bar feel real rather than pedantic:
 *
 *   1. Already a full http(s) URL   → use it.
 *   2. localhost / 127.0.0.1        → http://, because a local dev server
 *                                     almost never has TLS, and defaulting to
 *                                     https there fails for everyone.
 *   3. Contains a dot, no spaces    → https://, i.e. treat it as a hostname.
 *   4. Anything else                → a search query.
 *
 * Rung 4 matters more than it looks: without it, typing a phrase yields
 * "https://what is a webrtc offer", which fails DNS and shows an error page.
 *
 * DuckDuckGo's lite endpoint is the search target because it needs no
 * JavaScript and shows no consent wall — both of which matter when the page is
 * being software-rendered on a server and driven over a video stream.
 */
export function normalizeUrl(input: string): string {
  const trimmed = input.trim();
  if (!trimmed) return "";

  if (/^https?:\/\//i.test(trimmed)) return trimmed;
  if (/^(localhost|127\.0\.0\.1)(:\d+)?(\/|$)/i.test(trimmed)) return `http://${trimmed}`;
  if (trimmed.includes(".") && !/\s/.test(trimmed)) return `https://${trimmed}`;

  return `https://lite.duckduckgo.com/lite/?q=${encodeURIComponent(trimmed)}`;
}

export interface NavigationResult {
  ok: boolean;
  url?: string;
  error?: string;
}

/** POST helper. `credentials: "same-origin"` carries the session cookie. */
async function post(path: string, body?: unknown): Promise<NavigationResult> {
  try {
    const res = await fetch(path, {
      method: "POST",
      credentials: "same-origin",
      cache: "no-store",
      headers: body === undefined ? {} : { "Content-Type": "application/json" },
      ...(body === undefined ? {} : { body: JSON.stringify(body) }),
    });
    const payload = (await res.json().catch(() => ({}))) as {
      url?: string;
      error?: string;
      ok?: boolean;
    };
    if (!res.ok) {
      return { ok: false, error: payload.error ?? `HTTP ${res.status}` };
    }
    // The history verbs answer {ok:false, reason} when CDP declines — e.g.
    // Back with nothing behind it. That is a no-op, not an error.
    return { ok: payload.ok ?? true, ...(payload.url ? { url: payload.url } : {}) };
  } catch (err) {
    return { ok: false, error: err instanceof Error ? err.message : String(err) };
  }
}

/** Navigate the remote browser. `input` is raw address-bar text. */
export async function navigate(input: string): Promise<NavigationResult> {
  const url = normalizeUrl(input);
  if (!url) return { ok: false, error: "nothing to navigate to" };
  return post(ENDPOINTS.navigate, { url });
}

export const goBack = (): Promise<NavigationResult> => post(ENDPOINTS.back);
export const goForward = (): Promise<NavigationResult> => post(ENDPOINTS.forward);
export const reload = (): Promise<NavigationResult> => post(ENDPOINTS.reload);
export const stopLoading = (): Promise<NavigationResult> => post(ENDPOINTS.stop);

/** What the remote browser is currently showing. */
export interface CurrentPage {
  url: string | null;
  /** The page's own <title>. Empty string when the page has none. */
  title: string;
}

/**
 * The remote browser's current page.
 *
 * `title` was added to the gateway's /api/current-url response in 2026-09;
 * it is read defensively so this client still works against an older
 * gateway, which simply omits the field. The DevTools /json listing has
 * always carried it — the gateway parsed it into cdpTarget.Title and threw
 * it away, so the client had an address and nothing else to show.
 */
export async function currentPage(): Promise<CurrentPage> {
  try {
    const res = await fetch(ENDPOINTS.currentUrl, {
      credentials: "same-origin",
      cache: "no-store",
    });
    if (!res.ok) return { url: null, title: "" };
    const body = (await res.json()) as { url?: unknown; title?: unknown };
    return {
      url: typeof body.url === "string" ? body.url : null,
      title: typeof body.title === "string" ? body.title : "",
    };
  } catch {
    return { url: null, title: "" };
  }
}

/** What the remote browser is currently showing, or null if unavailable. */
export async function currentUrl(): Promise<string | null> {
  return (await currentPage()).url;
}

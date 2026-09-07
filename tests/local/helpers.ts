// Shared helpers for the real-Chrome validation specs.
//
// These exist because the interesting scenarios (cold arrival, second viewer,
// state preservation) all need the same three primitives, and duplicating them
// per-spec is how they drift apart.
import { expect, type Page } from "@playwright/test";

export const USER = process.env["CHROMELESS_USER"] ?? "chromeless";
export const PASS = process.env["CHROMELESS_PASS"] ?? "";

/** Log in and land on the app. Idempotent: a session cookie short-circuits. */
export async function login(page: Page): Promise<void> {
  await page.goto("/login");
  // Already authenticated (a reused context) — the gateway redirects away.
  if (!/\/login/.test(page.url())) return;
  await page.fill("#u", USER);
  await page.fill("#p", PASS);
  await page.getByRole("button", { name: "Sign in" }).click();
  await expect(page).not.toHaveURL(/\/login/, { timeout: 20_000 });
}

/**
 * Read framesDecoded off the live PeerConnection.
 *
 * Poll this rather than using waitForFunction: waitForFunction's handle
 * resolves to the LAST evaluation, which is `false` on the tick that ends the
 * wait — so the frame count came back as `false` even while frames flowed.
 */
export async function readFrames(page: Page): Promise<number> {
  return page.evaluate(async () => {
    const pc = (window as unknown as { __cbwrtc_pc?: RTCPeerConnection }).__cbwrtc_pc;
    if (!pc) return -1;
    const stats = await pc.getStats();
    let frames = 0;
    stats.forEach((r) => {
      if (r.type === "inbound-rtp" && (r as { kind?: string }).kind === "video") {
        frames = (r as { framesDecoded?: number }).framesDecoded ?? 0;
      }
    });
    return frames;
  });
}

/** Wait until framesDecoded exceeds `min`, returning the count actually seen. */
export async function waitForFrames(page: Page, min = 30, timeoutMs = 60_000): Promise<number> {
  let n = 0;
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    n = await readFrames(page);
    if (n > min) return n;
    await page.waitForTimeout(1000);
  }
  return n;
}


/**
 * Press Connect only on a bundle that is still idle after a beat.
 *
 * The client connects on load (client/main.ts autoConnect, 2026-08-24), and
 * from that moment #connect reads "Disconnect". Clicking it unconditionally —
 * which every spec here did — either waited on a disabled button or, once it
 * became usable, ended the session the spec was about to measure. This keeps
 * working against a bundle that still needs the click.
 */
export async function connectIfIdle(page: Page): Promise<void> {
  try {
    await expect(page.locator("#status")).not.toHaveAttribute("data-state", "idle", { timeout: 3_000 });
  } catch {
    await expect(page.locator("#connect")).toBeEnabled();
    await page.locator("#connect").click();
  }
}

/**
 * Connect and assert the full media path is live.
 *
 * `expectNoReload` is the whole point of the cold-arrival case: the stack must
 * hand a NEWLY ARRIVED viewer an offer without the user reloading the page.
 * Reloading masks the defect, because a reload re-joins the broker and races a
 * fresh offer into the replay buffer.
 */
export async function connectAndVerify(
  page: Page,
  label: string,
  connectTimeoutMs = 90_000,
): Promise<number> {
  await page.goto("/?e2e=1");
  await connectIfIdle(page);

  await expect(page.locator("#status"),
    `${label}: never reached connected (this is the "waiting for offer" hang)`)
    .toHaveAttribute("data-state", "connected", { timeout: connectTimeoutMs });
  await expect(page.locator("#state-ice")).toHaveText(/connected|completed/, { timeout: 30_000 });
  await expect(page.locator("#state-dc"),
    `${label}: input data channel never opened — video without control`)
    .toHaveText("open", { timeout: 30_000 });

  const frames = await waitForFrames(page);
  expect(frames, `${label}: no frames decoded (the documented fdec=0 failure)`).toBeGreaterThan(30);
  return frames;
}

/** Ask the gateway what page the REMOTE browser is on. */
export async function remoteUrl(page: Page): Promise<string> {
  return page.evaluate(async () => {
    const r = await fetch("/api/current-url");
    if (!r.ok) return `HTTP ${r.status}`;
    const j = (await r.json()) as { url?: string };
    return j.url ?? JSON.stringify(j);
  });
}

/** Drive the REMOTE browser to a URL via the gateway's nav bar. */
export async function navigateRemote(page: Page, url: string): Promise<void> {
  const bar = page.locator("#nav-url");
  await expect(bar).toBeVisible();
  await bar.fill(url);
  await page.locator("#nav-go").click();
}

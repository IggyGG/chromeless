// THE USER'S PATH. No test-only flags, no test-only hooks, judged by pixels.
//
// WHY THIS FILE EXISTS, AND WHY THE OTHER SPECS WERE NOT ENOUGH
//
// Every other spec here opens `/?e2e=1`. That query parameter is not cosmetic:
// client/main.ts:249 only pins the live RTCPeerConnection onto
// `window.__cbwrtc_pc` when it is set, precisely so that production users do
// NOT get an active PC exposed on a page that is internet-reachable.
//
// So those specs:
//   * load a page the user never loads, and
//   * measure success through `framesDecoded` on a handle that does not exist
//     on the user's page at all.
//
// Twice in a row the suite was green while the user saw "waiting for offer".
// Both times the tests were true statements about a different page.
//
// This spec loads exactly what the user types — https://localhost:8443/ — logs
// in through the form, clicks Connect, and drives the address bar. Success is
// judged the way a person judges it: THE PIXELS IN THE VIDEO CHANGED, and then
// held still. Nothing here reads a counter that a stalled encoder could keep
// incrementing.
//
// Three probe bugs found while writing it, all of which produced a confident
// wrong answer:
//   1. sampling the frame before `video.currentTime > 0` — the baseline was
//      null, so the first comparison could never fail;
//   2. accepting "loaded" as soon as /api/current-url reported the new site.
//      That URL flips on Page.navigate COMMIT, long before the encoder has
//      painted, so the probe fired all three navigations 1.2s apart and
//      reported PASS while the screenshot showed the previous site;
//   3. comparing only against the previous site's frame, so a page still
//      painting looked "unchanged" and reported a false FAILURE.
// The cure for all three is the same: wait for the frame to MOVE and then
// SETTLE before believing anything.
import { expect, test } from "@playwright/test";

const USER = process.env["CHROMELESS_USER"] ?? "chromeless";
const PASS = process.env["CHROMELESS_PASS"] ?? "";

test.describe.configure({ mode: "serial" });

/** A cheap perceptual hash of what the <video> is actually showing. */
async function frameHash(page: import("@playwright/test").Page): Promise<number | null> {
  return page.evaluate(() => {
    const v = document.querySelector("video") as HTMLVideoElement | null;
    if (!v || !v.videoWidth) return null;
    const c = document.createElement("canvas");
    c.width = 160;
    c.height = 90;
    const g = c.getContext("2d");
    if (!g) return null;
    g.drawImage(v, 0, 0, 160, 90);
    const d = g.getImageData(0, 0, 160, 90).data;
    let h = 0;
    for (let i = 0; i < d.length; i += 4) {
      h = (h * 31 + ((d[i]! << 16) | (d[i + 1]! << 8) | d[i + 2]!)) | 0;
    }
    return h;
  });
}

async function remoteUrl(page: import("@playwright/test").Page): Promise<string> {
  return page.evaluate(async () => {
    try {
      const r = await fetch("/api/current-url");
      const j = (await r.json()) as { url?: string };
      return j.url ?? "?";
    } catch {
      return "?";
    }
  });
}

test("the user's path: bare URL, log in, connect, browse real sites", async ({ page }) => {
  // 300s: the walk itself takes ~150s (three sites x up to 40s of settle
  // polling) and the connect can take 150s on a cold arrival. The config's
  // 180s default is not enough and truncated this test mid-walk.
  test.setTimeout(420_000);

  // ---- exactly what the user types ---------------------------------------
  await page.goto("/", { waitUntil: "domcontentloaded" });
  if (/\/login/.test(page.url())) {
    await page.fill("#u", USER);
    await page.fill("#p", PASS);
    await page.getByRole("button", { name: "Sign in" }).click();
    await expect(page).not.toHaveURL(/\/login/, { timeout: 20_000 });
  }

  await page.locator("#connect").click();
  await expect(page.locator("#status"),
    'the status pill never reached "connected" — this is the "waiting for offer" hang')
    .toHaveAttribute("data-state", "connected", { timeout: 150_000 });

  // A picture, not a promise of one.
  await page.waitForFunction(() => {
    const v = document.querySelector("video") as HTMLVideoElement | null;
    return !!v && v.videoWidth > 0 && v.currentTime > 0;
  }, null, { timeout: 30_000 });

  // ---- browse, like a person ---------------------------------------------
  const sites = [
    "https://en.wikipedia.org/wiki/WebRTC",
    "https://news.ycombinator.com",
    "https://example.com",
  ];

  for (const url of sites) {
    const host = new URL(url).hostname;
    const before = await frameHash(page);

    await page.locator("#nav-url").fill(url);
    await page.locator("#nav-go").click();

    // Wait for: the remote browser to be on this site, the picture to have
    // CHANGED, and then to hold still for 3s. The settle is what stops the
    // next navigation from being fired into a half-painted frame.
    let moved = false;
    let stable = 0;
    let last: number | null = null;
    let remote = "?";
    for (let i = 0; i < 40; i++) {
      await page.waitForTimeout(1000);
      remote = await remoteUrl(page);
      const now = await frameHash(page);
      if (before !== null && now !== null && now !== before) moved = true;
      stable = now !== null && now === last ? stable + 1 : 0;
      last = now;
      if (remote.includes(host) && moved && stable >= 3) break;
    }

    console.log(`  ${url} -> remote=${remote} repainted=${moved} stableFor=${stable}s`);
    expect(remote, `the remote browser never reached ${host}`).toContain(host);
    expect(moved, `the video never repainted after navigating to ${host} — ` +
      `the remote browser moved but the picture did not follow`).toBe(true);
    expect(stable, `the picture never settled on ${host}`).toBeGreaterThanOrEqual(3);
  }
});

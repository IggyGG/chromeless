// End-to-end validation of the chromeless standalone stack, driven by REAL
// Google Chrome on this laptop against the deployed k8s stack.
//
// Why real Chrome and not the bundled chromium: the in-cluster suite runs
// pod-to-pod, where host candidates pair directly and TURN is never exercised.
// A laptop is behind NAT, so the ONLY workable path is relay<->relay. That
// difference is what hid a live bug (a 23-minute-old buffered offer being
// replayed with dead ICE credentials) from every test that passed in CI.
import { expect, test } from "@playwright/test";
import { connectIfIdle } from "./helpers.js";

const USER = process.env["CHROMELESS_USER"] ?? "chromeless";
const PASS = process.env["CHROMELESS_PASS"] ?? "";

test.describe.configure({ mode: "serial" });

test("real Chrome: login, connect, decode video, and navigate", async ({ page }) => {
  const clientLog: string[] = [];
  page.on("console", (m) => clientLog.push(m.text()));

  // ---- 1. login -----------------------------------------------------------
  await page.goto("/login");
  await page.fill("#u", USER);
  await page.fill("#p", PASS);
  await page.getByRole("button", { name: "Sign in" }).click();
  await expect(page).not.toHaveURL(/\/login/, { timeout: 20_000 });

  // ---- 2. the app loads ---------------------------------------------------
  await page.goto("/?e2e=1");

  // ---- 3. connect ---------------------------------------------------------
  // (on load, by the client itself; helpers.ts connectIfIdle explains)
  await connectIfIdle(page);

  // The pill must reach "connected" — NOT "connecting", which is precisely the
  // state a broken stack sits in forever.
  await expect(page.locator("#status"),
    "peer connection never reached connected from real Chrome").
    toHaveAttribute("data-state", "connected", { timeout: 90_000 });

  await expect(page.locator("#state-ice")).toHaveText(/connected|completed/, { timeout: 30_000 });
  await expect(page.locator("#state-sig")).toHaveText("stable", { timeout: 10_000 });

  // ---- 4. the input data channel: can the user actually CONTROL it? -------
  await expect(page.locator("#state-dc"),
    "input data channel never opened — the browser would render but ignore every click").
    toHaveText("open", { timeout: 30_000 });

  // ---- 5. REAL VIDEO: frames must actually decode -------------------------
  // A receiver can exist with zero frames ever arriving; that is the
  // documented fdec=0 failure. Require the counter to CLIMB.
  // Poll explicitly rather than via waitForFunction: its handle resolves to
  // the LAST evaluation, which is `false` on the tick that stops the wait —
  // so the count was being read as `false` even though frames were flowing.
  const readFrames = () =>
    page.evaluate(async () => {
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

  let frameCount = 0;
  const framesDeadline = Date.now() + 60_000;
  while (Date.now() < framesDeadline) {
    frameCount = await readFrames();
    if (frameCount > 30) break;
    await page.waitForTimeout(1000);
  }
  console.log(`  framesDecoded = ${frameCount}`);
  expect(Number(frameCount), "video track exists but no frames decoded — the documented fdec=0 failure").toBeGreaterThan(30);

  // ---- 6. the <video> element is actually painting ------------------------
  const dims = await page.evaluate(() => {
    const v = document.querySelector("video") as HTMLVideoElement | null;
    return v ? { w: v.videoWidth, h: v.videoHeight, paused: v.paused } : null;
  });
  console.log(`  <video> = ${JSON.stringify(dims)}`);
  expect(dims, "no <video> element").not.toBeNull();
  expect(dims!.w, "video has zero width — nothing is painting").toBeGreaterThan(0);
  expect(dims!.h).toBeGreaterThan(0);

  // ---- 7. navigation: drive the REMOTE browser ---------------------------
  const before = frameCount;
  // The real control surface: #nav-url + #nav-go drive CDP Page.navigate on
  // the REMOTE browser. This is the leg that proves the whole loop — the
  // gateway reaching Chromium's DevTools, not just media flowing one way.
  const bar = page.locator("#nav-url");
  await expect(bar).toBeVisible();
  await bar.fill("https://example.com");
  await page.locator("#nav-go").click();

  // The remote page changing is observable two ways: the stream keeps
  // decoding (it did not freeze), and the gateway reports the new URL.
  await page.waitForTimeout(8000);
  const after = await readFrames();
  console.log(`  framesDecoded after navigate = ${after} (was ${before})`);
  expect(after, "stream froze after navigating the remote browser").toBeGreaterThan(Number(before));

  const currentUrl = await page.evaluate(async () => {
    const r = await fetch("/api/current-url");
    if (!r.ok) return `HTTP ${r.status}`;
    const j = (await r.json()) as { url?: string };
    return j.url ?? JSON.stringify(j);
  });
  console.log(`  remote browser URL = ${currentUrl}`);
  expect(currentUrl, "the remote browser did not navigate").toContain("example.com");

  const ice = clientLog.filter((l) => /typ relay|iceConnectionState/.test(l)).slice(0, 6);
  console.log("  client ICE lines:\n" + ice.map((l) => "    " + l).join("\n"));
});

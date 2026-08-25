// Does the remote browser actually RESPOND to input?
//
// An open data channel is not a working one. #state-dc reaching "open" only
// says the transport exists; it says nothing about whether a click reaches
// Chromium's input pipeline and changes the page. Those are separate failures
// with the same green indicator, and only this test separates them.
//
// The proof is deliberately indirect but unfakeable: we drive REAL mouse and
// keyboard through Playwright onto the <video> element (client/src/input.ts
// binds mousedown/mousemove/wheel to that element and keydown/keyup to the
// window), then ask the GATEWAY — over a completely different transport, plain
// HTTPS, not the data channel — what the remote browser's URL is. If typing a
// query and pressing Enter changes the remote URL, then input crossed the data
// channel, was decoded by the embedder, was injected into Chromium, and the
// page acted on it. No single component can fake that chain.
import { expect, test } from "@playwright/test";
import { connectAndVerify, login, navigateRemote, readFrames, remoteUrl } from "./helpers";

test.describe.configure({ mode: "serial" });

test("input: typing and clicking actually drive the remote page", async ({ page }) => {
  test.setTimeout(300_000);

  await login(page);
  const framesAtStart = await connectAndVerify(page, "input");

  // DuckDuckGo's HTML endpoint: no JS framework, no consent interstitial, and
  // — the property this test relies on — a page full of ordinary <a> links at
  // predictable positions. Clicking one navigates. That is the assertion.
  //
  // Why a CLICK on a results page rather than typing into a search box: a
  // click needs only to land somewhere in a large field of links, whereas
  // typing needs the caret to be in a specific input, which needs the click
  // BEFORE it to have hit a ~200px target. Both exercise the same transport;
  // only the click is robust to the remote page rendering slightly differently
  // than expected. The keyboard leg still runs below as a second signal.
  //
  // (A synthetic data: URL page that echoes its own input would be more
  // precise, but the gateway allowlists http/https only — validateNavigationURL
  // in infra/gateway/cdp.go — and that restriction is correct: data: URLs in a
  // navigation endpoint are an XSS vector. So the test uses a real site.)
  await navigateRemote(page, "https://html.duckduckgo.com/html/?q=chromeless");
  await page.waitForTimeout(8000);
  const urlBefore = await remoteUrl(page);
  console.log(`  remote URL before input = ${urlBefore}`);
  expect(urlBefore, "the results page never loaded").toContain("duckduckgo");

  const video = page.locator("video");
  await expect(video).toBeVisible();
  const box = await video.boundingBox();
  expect(box, "no <video> bounding box — cannot aim input").not.toBeNull();

  // ---- 1. MOUSE: click into the results ----------------------------------
  // Mid-page, where result links sit. If input crosses the data channel, gets
  // decoded, and reaches Chromium's input pipeline, the remote page navigates
  // AWAY from duckduckgo — something no single component can fake.
  const cx = box!.x + box!.width / 2;
  const cy = box!.y + box!.height / 2;
  await page.mouse.move(cx, cy);
  await page.mouse.click(cx, cy);

  let urlAfter = "";
  const deadline = Date.now() + 45_000;
  while (Date.now() < deadline) {
    urlAfter = await remoteUrl(page);
    if (!/duckduckgo/.test(urlAfter)) break;
    await page.waitForTimeout(2000);
  }
  console.log(`  remote URL after click  = ${urlAfter}`);
  expect(urlAfter,
    "the click never reached the remote page — the data channel is open but " +
    "input is not being injected into Chromium (an open DC is NOT a working one)")
    .not.toContain("duckduckgo");

  // ---- 2. KEYBOARD: a second, independent signal -------------------------
  // Tab-then-Enter, NOT typing into a search box.
  //
  // Two earlier drafts of this leg failed for harness reasons while the stack
  // was fine, and both are worth remembering:
  //   * Alt+Left — browser-level shortcuts are swallowed by the LOCAL Chrome
  //     before reaching the page, so nothing crosses the data channel at all.
  //   * type-into-the-search-box — needs a click to land on a specific input
  //     first, so it tests aim, not the keyboard. It failed at two different
  //     y-offsets while Tab/Enter worked on the first try.
  //
  // Tab moves focus and Enter activates whatever has it. Neither needs a
  // caret, a target, or a guessed coordinate — it isolates the KEYBOARD
  // transport, which is the thing under test. Verified against a live stack:
  // three Tabs and an Enter moved the remote browser off its start page.
  // Start from a KNOWN page. The click above navigates to whichever result
  // link it happened to hit — a different site every run — and on some of them
  // three Tabs land on a button that does not navigate, so the assertion
  // flaked while the keyboard transport was working fine. Re-navigating makes
  // the starting state deterministic.
  //
  // The DuckDuckGo HTML results page is a good target for this specifically
  // because its first focusable elements are result links: Tab moves onto one
  // and Enter follows it.
  await navigateRemote(page, "https://html.duckduckgo.com/html/?q=chromeless");
  await page.waitForTimeout(8000);

  const beforeKeys = await remoteUrl(page);
  expect(beforeKeys, "could not return to a known page for the keyboard leg")
    .toContain("duckduckgo");

  // Focus the remote page first: keystrokes go to whatever the remote browser
  // considers focused, and a fresh navigation may leave that unset.
  await page.mouse.click(box!.x + 10, box!.y + box!.height - 10);
  await page.waitForTimeout(1200);

  for (let i = 0; i < 3; i++) {
    await page.keyboard.press("Tab");
    await page.waitForTimeout(400);
  }
  await page.keyboard.press("Enter");

  let keyUrl = beforeKeys;
  const keyDeadline = Date.now() + 45_000;
  while (Date.now() < keyDeadline) {
    keyUrl = await remoteUrl(page);
    if (keyUrl !== beforeKeys) break;
    await page.waitForTimeout(2000);
  }
  console.log(`  remote URL after Tab*3+Enter = ${keyUrl}`);
  expect(keyUrl,
    "keystrokes did not reach the remote page — the data channel is open but " +
    "keyboard input is not being injected into Chromium")
    .not.toBe(beforeKeys);

  // ---- 4. and the stream survived the interaction ------------------------
  const framesEnd = await readFrames(page);
  console.log(`  framesDecoded ${framesAtStart} -> ${framesEnd}`);
  expect(framesEnd, "the stream froze while input was being driven")
    .toBeGreaterThan(framesAtStart);
});

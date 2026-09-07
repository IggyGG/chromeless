// Shared "get me a connected client" step for the specs.
//
// The client CONNECTS ON LOAD (client/main.ts autoConnect, since 2026-08-24):
// a user who has just logged in gets a browser, not a button. The button is
// still there, but from the moment the page loads it reads "Disconnect" and is
// disabled until the session settles. Four specs here still did
//
//   await page.locator("#connect").click();
//
// which, against the live stack, is a click on DISABLED "Disconnect" — so
// Playwright waited 10 s for it to become enabled, or (spec 01) asserted
// `toBeEnabled` and got "disabled". That is exactly the failure the E2E lane
// showed on every run from 2026-08-25 to 2026-09-07: specs 01/02/03/06 red on
// three different guest images and two broker builds, while the same stack
// passed 65/65 in tests/interactive (whose harness clicks #connect too, but
// only after a 3 s pause — by then the connect has settled and the click is
// a real Disconnect… followed by a reconnect it never noticed). The product
// was never the problem in those runs; the specs were asserting a UI that no
// longer exists.
//
// Use this instead of clicking. It waits for whichever state the auto-connect
// reaches, and only presses the button if the page has NOT auto-connected —
// i.e. it keeps working against a client that still needs a click.
import { expect, type Page } from "@playwright/test";

/** A cold worker can take ~35 s to emit its first offer; the client's own
 *  watchdog does not fire until 45 s. */
export const CONNECT_TIMEOUT_MS = 60_000;

/**
 * Navigate to `path` and wait for the peer connection to reach `connected`.
 * Returns once `#status[data-state="connected"]`.
 */
export async function openConnected(page: Page, path = "/"): Promise<void> {
  await page.goto(path);
  const status = page.locator("#status");
  const connect = page.locator("#connect");

  // Auto-connect flips the pill off "idle" synchronously on load. If it is
  // STILL idle after a beat, this bundle expects a click — do what a user
  // would.
  try {
    await expect(status).not.toHaveAttribute("data-state", "idle", { timeout: 3_000 });
  } catch {
    await expect(connect).toBeEnabled();
    await connect.click();
  }

  await expect(
    status,
    "peer connection never reached 'connected' — negotiation problem, not a media one",
  ).toHaveAttribute("data-state", "connected", { timeout: CONNECT_TIMEOUT_MS });
}

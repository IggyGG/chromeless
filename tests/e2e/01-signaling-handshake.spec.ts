// 01: Signaling handshake reaches the documented stub state.
//
// What this asserts:
//   - The client page loads at baseURL.
//   - Clicking "Connect" drives the status pill into either "connecting"
//     (the documented stub state per client/README.md and main.ts —
//     reached today, before T34 flips the client to the answerer role)
//     or "connected" (post-T34, when the streamer page actually answers).
//   - The peer connection's ICE gathering reaches "complete", which is
//     independent of any remote answer and therefore proves the SDP
//     machinery did real work.
//   - The signaling state reaches a sane value: "have-local-offer" today
//     (offer sent, awaiting answer) or "stable" once the round-trip
//     completes post-T34.
//
// This is the minimum viable "signaling client is alive" check. It does
// NOT depend on T34 — it passes against the current stub state and will
// continue to pass after T34 is wired up.

import { expect, test } from "@playwright/test";

test.describe("signaling handshake", () => {
  test("Connect drives status to connecting/connected within 10s", async ({ page }) => {
    await page.goto("/");

    // Sanity: the connect button is interactable.
    const connect = page.locator("#connect");
    await expect(connect).toBeVisible();
    await expect(connect).toBeEnabled();

    await connect.click();

    // Status pill should leave "idle" within 10 s. Pre-T34 it lands on
    // "connecting" and stays; post-T34 it transitions to "connected".
    const status = page.locator("#status");
    await expect(status).toHaveAttribute(
      "data-state",
      /connecting|connected/,
      { timeout: 10_000 }
    );

    // ICE gathering must reach "complete" — proves we built and applied
    // a local offer and the ICE agent actually ran.
    await expect(page.locator("#state-iceg")).toHaveText("complete", {
      timeout: 10_000,
    });

    // Signaling state: have-local-offer (stub) or stable (post-T34).
    await expect(page.locator("#state-sig")).toHaveText(
      /have-local-offer|stable/,
      { timeout: 10_000 }
    );
  });
});
